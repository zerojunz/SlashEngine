#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <numeric>
#include <thread>
#include <vector>

namespace jobs = slash::jobs;
namespace json = slash::json;

TEST_CASE("tasks run on the pool's threads and are waited for") {
    for (const int threads : {0, 1, 3}) {
        CAPTURE(threads);
        jobs::Pool pool(threads);
        CHECK(pool.thread_count() == threads);
        std::atomic<int> sum{0};
        std::vector<jobs::TaskId> ids;
        for (int i = 1; i <= 10; ++i) {
            ids.push_back(pool.add_task([&sum, i] { sum += i; }));
        }
        for (const jobs::TaskId id : ids) {
            pool.wait_for_task_completion(id);
        }
        CHECK(sum == 55);
    }
}

TEST_CASE("with no threads a task runs as it is added") {
    jobs::Pool pool(0);
    int value = 0;
    const jobs::TaskId id = pool.add_task([&] { value = 7; });
    CHECK(value == 7);
    CHECK(pool.is_task_completed(id));
    pool.wait_for_task_completion(id);
}

TEST_CASE("a task no thread has taken runs on the thread that waits for it") {
    jobs::Pool pool(1);
    std::atomic<bool> release{false};
    std::atomic<bool> busy{false};
    const jobs::TaskId blocker = pool.add_task([&] {
        busy = true;
        while (!release) {
            std::this_thread::yield();
        }
    });
    while (!busy) {
        std::this_thread::yield();
    }
    int ran_on = -1;
    const jobs::TaskId queued = pool.add_task([&] { ran_on = pool.thread_index(); });
    CHECK_FALSE(pool.is_task_completed(queued));
    pool.wait_for_task_completion(queued);
    CHECK(ran_on == 0); // this thread, not the pool's busy worker
    release = true;
    pool.wait_for_task_completion(blocker);
}

TEST_CASE("group tasks run a function over every element") {
    for (const int threads : {0, 2, 4}) {
        CAPTURE(threads);
        jobs::Pool pool(threads);
        std::vector<std::size_t> squares(1000, 0);
        const jobs::GroupId id =
            pool.add_group_task([&](std::size_t i) { squares[i] = i * i; }, squares.size());
        pool.wait_for_group_task_completion(id);
        for (std::size_t i = 0; i < squares.size(); ++i) {
            REQUIRE(squares[i] == i * i);
        }
        const jobs::GroupId empty = pool.add_group_task([](std::size_t /*i*/) {}, 0);
        CHECK(pool.is_group_task_completed(empty));
        CHECK(pool.group_processed_element_count(empty) == 0);
        pool.wait_for_group_task_completion(empty);
    }
    jobs::Pool none(0);
    const jobs::GroupId done = none.add_group_task([](std::size_t /*i*/) {}, 5);
    CHECK(none.group_processed_element_count(done) == 5);
    none.wait_for_group_task_completion(done);
}

TEST_CASE("parallel_for nests and numbers the threads running it") {
    jobs::Pool pool(3);
    std::vector<long> totals(8, 0);
    std::vector<std::atomic<int>> seen(4);
    pool.parallel_for(totals.size(), [&](std::size_t outer) {
        std::vector<long> parts(100, 0);
        pool.parallel_for(parts.size(), [&](std::size_t inner) {
            const int index = pool.thread_index();
            REQUIRE(index >= 0);
            REQUIRE(index <= pool.thread_count());
            ++seen[static_cast<std::size_t>(index)];
            parts[inner] = static_cast<long>(outer * inner);
        });
        totals[outer] = std::accumulate(parts.begin(), parts.end(), 0L);
    });
    for (std::size_t outer = 0; outer < totals.size(); ++outer) {
        CHECK(totals[outer] == static_cast<long>(outer * 4950));
    }
    CHECK(std::accumulate(seen.begin(), seen.end(), 0, [](int sum, const std::atomic<int>& n) {
              return sum + n.load();
          }) == 800);
    // A thread outside the pool is number 0.
    CHECK(pool.thread_index() == 0);
}

TEST_CASE("threading.max_threads sets the shared pool's threads") {
    CHECK(jobs::thread_count_for(0) == 0);
    CHECK(jobs::thread_count_for(2) == 2);
    CHECK(jobs::thread_count_for(1000) == jobs::kMaxThreads);
    CHECK(jobs::thread_count_for(-1) ==
          std::min(static_cast<int>(std::thread::hardware_concurrency()), jobs::kMaxThreads));

    const json::Value schema = jobs::settings_schema();
    CHECK(json::validate(json::parse(R"({"max_threads": 4})").value(), schema).empty());
    CHECK_FALSE(json::validate(json::parse(R"({"max_threads": -2})").value(), schema).empty());
    CHECK_FALSE(json::validate(json::parse(R"({"threads": 4})").value(), schema).empty());
    CHECK(jobs::parse_max_threads(json::parse(R"({"max_threads": 4})").value()) == 4);
    CHECK(jobs::parse_max_threads(json::Object{}) == -1);

    // Once the shared pool runs, only its own count takes.
    const int running = jobs::shared().thread_count();
    CHECK(jobs::configure(running));
    CHECK_FALSE(jobs::configure(running == 0 ? 1 : 0));
}
