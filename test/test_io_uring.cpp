#include <alloca.h>
#include <atomic>
#include <cassert>
#include <fcntl.h>
#include <filesystem>
#include <print>
#include <string.h>
#include <thread>
#include <unistd.h>

#include "io/io_uring.h"
#include "sys/file.h"

int main(int argc, const char* argv[]) {

    std::filesystem::path filepath { argc > 1 ? argv[1] : "." };
    filepath.append("test.txt");

    char read_buf[128];
    memset(read_buf, 0, sizeof(read_buf));

    alignas(IO_URING_SERVICE_ALIGNMENT) char service_buf[IO_URING_SERVICE_SIZE];
    struct io_uring_service* service = reinterpret_cast<struct io_uring_service*>(&service_buf);

    io_uring_service_init(service, 1, IORING_MODE_SQPOLL);

    std::thread task {[&]() {
        io_uring_service_start(service);
        io_uring_service_finish(service);
    }};

    int fd = open(filepath.c_str(), O_CREAT | O_WRONLY, 0666);
    std::atomic_int flag = 10;
    for (int i = 0; i < 10; ++i) {
        io_uring_service_submit(
            service, fd, 
            IORING_OP_WRITE, (void*)"hello, world\n", 
            13, i*13, 
            [](int res, void* arg) {
                std::println("write res: {}", res);
                std::atomic_int& flag = *(std::atomic_int*)arg;
                if (flag.fetch_sub(1) == 1)
                    flag.notify_all();
            }, 
            &flag
        );
    }
    while(flag.load() != 0) {}
    close(fd);

    flag.store(1);
    fd = open(filepath.c_str(), O_RDONLY);
    io_uring_service_submit(
        service, fd, 
        IORING_OP_READ, read_buf, 
        13, 0, 
        [](int res, void* arg) {
            std::println("read res: {}", res);
            std::atomic_int& flag = *(std::atomic_int*)arg;
            if (flag.fetch_sub(1) == 1)
                flag.notify_all();   
        },
        &flag
    );
    while(flag.load() != 0) {}
    close(fd);

    assert(strcmp(read_buf, "hello, world\n") == 0);

    io_uring_service_stop(service);
    task.join();
}
