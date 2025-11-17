#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <print>
#include <vector>
#include <thread>
#include "container/queue.hpp"

const size_t N_THREAD = 100;
const size_t N_LOOP = 10000;

bool arr[N_THREAD][N_LOOP];

int main() {

    std::vector<std::thread> ths;
    AtomicQueue<std::pair<int, int>> que;

    for(int i = 0; i < N_THREAD; ++i) {
        ths.emplace_back([&, i]() {
            for(int j = 0; j < N_LOOP; ++j) {
                que.push(i, j);
            }
        });
    }

    for(int i = 0; i < N_THREAD; ++i) {
        ths.emplace_back([&]() {
            for(int j = 0; j < N_LOOP; ++j) {
                for(;;) {
                    auto e = que.pop();
                    if (!e) continue;
                    auto [x, y] = e.value();
                    if (x < 0 || x >= N_THREAD || y < 0 || y >= N_LOOP) {
                        std::println("{} {}", x, y);
                        exit(-1);
                    }
                    arr[x][y] = true;
                    break;
                }
            }
        });
    }

    for(int i = 0; i < N_THREAD * 2; ++i) {
        ths[i].join();
    }

    for(int i = 0; i < N_THREAD; ++i) {
        for(int j = 0; j < N_LOOP; ++j) {
            assert(arr[i][j]);
        }
    }

    return 0;
}
