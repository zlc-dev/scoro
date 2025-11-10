#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <stddef.h>
#include <unistd.h>

typedef void(*io_cb)(int res, void* arg);

enum io_uring_mode {
    IORING_MODE_DEFAULT = 0x00000001,
    IORING_MODE_SQPOLL = 0x00000002,
};

struct io_uring_service;

#define IO_URING_SERVICE_SIZE 96
#define IO_URING_SERVICE_ALIGNMENT 8

int io_uring_service_init(struct io_uring_service *service, unsigned entries, enum io_uring_mode mode);

int io_uring_service_submit(struct io_uring_service *service, int fd, int op, void *buf, unsigned len, off_t off, io_cb cb, void* arg);

void io_uring_service_start(struct io_uring_service *service);

void io_uring_service_stop(struct io_uring_service *service);

void io_uring_service_finish(struct io_uring_service *service);

#ifdef __cplusplus
}
#endif
