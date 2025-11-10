#include "io/io_uring.h"
#include <linux/io_uring.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <assert.h> 
#include <unistd.h>
#include <stdint.h>

struct io_task {
    io_cb cb;
    void* arg;
};

struct io_uring_service {
    int ring_fd;
    enum io_uring_mode mode;
    unsigned *sring_head;
    unsigned *sring_tail;
    unsigned *sring_mask;
    unsigned *sring_array;
    unsigned *cring_head;
    unsigned *cring_tail;
    unsigned *cring_mask;
    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
    unsigned sq_entries;
    unsigned cq_entries;
    int running;
};

static_assert(sizeof(struct io_uring_service) == IO_URING_SERVICE_SIZE, 
    "define wrong size of struct io_uring_service");

static_assert(_Alignof(struct io_uring_service) == IO_URING_SERVICE_ALIGNMENT, 
    "define wrong alignment of struct io_uring_service");

static int sys_io_uring_setup(unsigned entries, struct io_uring_params *p)
{
    int ret = syscall(__NR_io_uring_setup, entries, p);
    return (ret < 0) ? -errno : ret;
}

static int sys_io_uring_enter(int ring_fd, unsigned int to_submit,
                    unsigned int min_complete, unsigned int flags)
{
    int ret = syscall(__NR_io_uring_enter, ring_fd, to_submit,
                    min_complete, flags, NULL, 0);
    return (ret < 0) ? -errno : ret;
}

#define smp_store_release(p,v) atomic_store_explicit((_Atomic typeof(*(p)) *)(p), (v), memory_order_release) 
#define smp_load_acquire(p) atomic_load_explicit((_Atomic typeof(*(p)) *)(p), memory_order_acquire)

int io_uring_service_init(struct io_uring_service *service, unsigned entries, enum io_uring_mode mode) {
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    service->mode = mode;
    if (mode == IORING_MODE_SQPOLL) 
        p.flags |= IORING_SETUP_SQPOLL;
    int fd = sys_io_uring_setup(entries, &p);
    if(fd < 0) return fd;
    
    service->ring_fd = fd;
    int sq_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned); 
    int cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);

    if(p.features & IORING_FEAT_SINGLE_MMAP) { 
        if(cq_sz > sq_sz) 
            sq_sz = cq_sz;
        cq_sz = sq_sz; 
    }

    void *sq_ptr = mmap(
        NULL, sq_sz, PROT_READ | PROT_WRITE, 
        MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING
    );

    if(sq_ptr == MAP_FAILED)
        return -1;

    void *cq_ptr; 
    if(p.features & IORING_FEAT_SINGLE_MMAP) 
        cq_ptr = sq_ptr; 
    else {
        cq_ptr = mmap(
            NULL, cq_sz, PROT_READ | PROT_WRITE, 
            MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING
        );
        if(cq_ptr == MAP_FAILED) 
            return -1;
    }

    service->sring_head = sq_ptr + p.sq_off.head;
    service->sring_tail = sq_ptr + p.sq_off.tail;
    service->sring_mask = sq_ptr + p.sq_off.ring_mask;
    service->sring_array = sq_ptr + p.sq_off.array;
    
    service->sqes = mmap(
        NULL, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, 
        MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES
    );
    if(service->sqes == MAP_FAILED) 
        return -1;

    service->cring_head = cq_ptr + p.cq_off.head;
    service->cring_tail = cq_ptr + p.cq_off.tail;
    service->cring_mask = cq_ptr + p.cq_off.ring_mask;
    service->cqes = cq_ptr + p.cq_off.cqes;
    service->cq_entries = p.cq_entries;
    service->sq_entries = p.sq_entries;

    return 0;
}

int io_uring_service_submit(struct io_uring_service *service, int fd, int op, void *buf, unsigned len, off_t off, io_cb cb, void *arg) {
    unsigned tail = *service->sring_tail;

    if (service->mode == IORING_MODE_SQPOLL) {
        while ((tail - smp_load_acquire(service->sring_head)) >= service->sq_entries) {
            sys_io_uring_enter(service->ring_fd, 1, 0, IORING_ENTER_SQ_WAKEUP | IORING_ENTER_SQ_WAIT);
        }
    } else {
        if (tail - smp_load_acquire(service->sring_head) >= service->sq_entries) {
            // unexpected
            return -1;
        }
    }

    unsigned index = tail & *service->sring_mask;
    struct io_uring_sqe *sqe = &service->sqes[index];

    sqe->opcode = op; 
    sqe->fd = fd; 
    sqe->addr = (unsigned long)buf; 
    sqe->len = len; 
    sqe->off = off;

    struct io_task* io_task = malloc(sizeof(struct io_task));
    io_task->cb = cb;
    io_task->arg = arg;
    sqe->user_data = (uint64_t)io_task;

    service->sring_array[index] = index; 
    smp_store_release(service->sring_tail, tail + 1);

    if (service->mode != IORING_MODE_SQPOLL) {
        return sys_io_uring_enter(service->ring_fd, 1, 0, 0);
    }
    return 0;
}

void io_uring_service_start(struct io_uring_service *service) {
    
    const int SPIN_LIMIT = 1000;

    smp_store_release(&service->running, 1);
    while(smp_load_acquire(&service->running)) {
        unsigned head = *service->cring_head; 
        if(head == smp_load_acquire(service->cring_tail)) {
            // spin wait
            for (int spins = 0; spins < SPIN_LIMIT; spins++) {
                if (head != smp_load_acquire(service->cring_tail))
                    break;
            }
            sys_io_uring_enter(service->ring_fd, 0, 1, IORING_ENTER_GETEVENTS | IORING_ENTER_SQ_WAKEUP);
            continue;
        }

        while(head != smp_load_acquire(service->cring_tail)) {
            struct io_uring_cqe *cqe = &service->cqes[head & *service->cring_mask]; 
            struct io_task *io_task = (struct io_task *)cqe->user_data;
            if(io_task) {
                io_task->cb(cqe->res, io_task->arg);
                free(io_task);
            }
            cqe->user_data = 0;
            head++;
        }
        smp_store_release(service->cring_head, head);
    }
}

void io_uring_service_stop(struct io_uring_service *service) {
    smp_store_release(&service->running, 0);

    struct io_uring_sqe *sqe;
    unsigned tail = *service->sring_tail;
    unsigned index = tail & *service->sring_mask;
    sqe = &service->sqes[index];

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_NOP;
    sqe->user_data = index;

    service->sring_array[index] = index;
    smp_store_release(service->sring_tail, tail + 1);

    sys_io_uring_enter(service->ring_fd, 1, 0, IORING_ENTER_GETEVENTS);
}

void io_uring_service_finish(struct io_uring_service *service) {
    close(service->ring_fd);
}
