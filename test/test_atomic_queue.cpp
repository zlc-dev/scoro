#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <print>
#include <vector>
#include <thread>
#include "container/queue.hpp"

const size_t N_THREAD = 200;
const size_t N_LOOP = 50000;

bool arr[N_THREAD][N_LOOP];

struct Test {

    inline static std::atomic_long count = 0;

    int x, y;

    Test(int x, int y) noexcept: x(x), y(y) { count.fetch_add(1); }

    Test(Test&& oth) noexcept: x(oth.x), y(oth.y) { count.fetch_add(1); }

    ~Test() { count.fetch_sub(1); }

};

int main() {

    std::println("{}", Test::count.load());
    {
        std::vector<std::thread> ths;
        nct::AtomicQueue<Test> que;
        std::atomic_int producers = N_THREAD;

        for(int i = 0; i < N_THREAD; ++i) {
            ths.emplace_back([&, i]() {
                for(int j = 0; j < N_LOOP; ++j) {
                    if(!que.enqueue(i, j)) {
                        return;
                    }
                }
                if (producers.fetch_sub(1) == 1) que.stop();
            });
        }

        ths.emplace_back([&]() {
            for(;;) {
                auto e = que.wait_dequeue();
                if (!e) {
                    return;
                }
                auto [x, y] = std::move(e.value());
                arr[x][y] = true;
            }
        });
        
        for(int i = 0; i < ths.size(); ++i) {
            ths[i].join();
        }
        std::println("empty: {}", que.maybe_empty());
        std::println("last: {}", Test::count.load());
    }

    std::println("{}", Test::count.load());

    for(int i = 0; i < N_THREAD; ++i) {
        for(int j = 0; j < N_LOOP; ++j) {
            assert(arr[i][j]);
        }
    }

    return 0;
}
