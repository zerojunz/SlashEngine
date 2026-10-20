#pragma once

#include "slash/core/json.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// The job system, as Godot's WorkerThreadPool: worker threads that run tasks, and group tasks
// that run one function over many elements. Worlds run their threaded systems on it
// (slash/world/world.hpp), and systems split their own work over it with parallel_for.
namespace slash::jobs {

using TaskId = std::uint64_t;
using GroupId = std::uint64_t;

// The most worker threads a pool has. Box2D numbers the threads that step a world below 64, and
// the thread that waits for a task takes part too.
inline constexpr int kMaxThreads = 63;

class Pool {
public:
    // A pool with this many worker threads, at most kMaxThreads. With none, every task runs on
    // the thread that adds it, as Godot's pool does where there are no threads (the web).
    explicit Pool(int threads);
    // Runs the tasks still queued, then stops the threads.
    ~Pool();

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    Pool(Pool&&) = delete;
    Pool& operator=(Pool&&) = delete;

    [[nodiscard]] int thread_count() const noexcept { return static_cast<int>(threads_.size()); }

    // Runs a task on a worker thread. Every task is waited for once with
    // wait_for_task_completion, as in Godot.
    TaskId add_task(std::function<void()> task);
    [[nodiscard]] bool is_task_completed(TaskId id) const;
    // Waits for the task to finish; one still queued runs on this thread instead.
    void wait_for_task_completion(TaskId id);

    // Runs task(i) for every i below elements, spread over tasks_needed worker threads (-1: all
    // of them). Every group is waited for once with wait_for_group_task_completion.
    GroupId add_group_task(std::function<void(std::size_t)> task, std::size_t elements,
                           int tasks_needed = -1);
    [[nodiscard]] bool is_group_task_completed(GroupId id) const;
    [[nodiscard]] std::size_t group_processed_element_count(GroupId id) const;
    // Waits for every element to be done, doing the ones no thread has started on this thread.
    void wait_for_group_task_completion(GroupId id);

    // Runs task(i) for every i below elements on the workers and this thread, and returns when
    // all are done. Which thread runs which element varies, so each element's work must be its
    // own.
    void parallel_for(std::size_t elements, const std::function<void(std::size_t)>& task);

    // The calling thread's number in this pool: 1 to thread_count() for its worker threads, 0 for
    // any other thread. No two threads running this pool's work at once share one, so work can
    // keep scratch space per thread (Box2D does).
    [[nodiscard]] int thread_index() const noexcept;

private:
    struct Task;
    struct Group;
    // A queued task, or one of a group's runners, which take its elements until none are left.
    struct Item {
        std::shared_ptr<Task> task;
        std::shared_ptr<Group> group;
    };

    void work(int index);
    void run(const Item& item);
    void run_group(Group& group);

    std::vector<std::thread> threads_;
    mutable std::mutex mutex_;
    std::condition_variable queued_;
    std::condition_variable finished_;
    std::deque<Item> queue_;
    std::unordered_map<TaskId, std::shared_ptr<Task>> tasks_;
    std::unordered_map<GroupId, std::shared_ptr<Group>> groups_;
    std::uint64_t next_id_ = 1;
    bool stopping_ = false;
};

// The worker threads a max_threads setting makes: -1 for one per CPU core, as Godot's
// threading/worker_pool/max_threads, and none on the web without threads.
[[nodiscard]] int thread_count_for(int max_threads);

// The pool worlds use unless given another: made on first use, with the thread count configure
// set.
Pool& shared();
// Sets the shared pool's max_threads, as project.json's threading.max_threads. It takes effect
// only before the pool is first used, as Godot's setting takes a restart; returns whether it did.
bool configure(int max_threads);

// JSON Schema for the threading object in project.json.
[[nodiscard]] json::Value settings_schema();
// The max_threads of a threading object that passed the schema; -1 if it has none.
[[nodiscard]] int parse_max_threads(const json::Value& threading);

} // namespace slash::jobs
