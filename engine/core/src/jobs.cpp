#include "slash/core/jobs.hpp"

#include "slash/core/assert.hpp"

#include <algorithm>
#include <atomic>
#include <utility>

namespace slash::jobs {

struct Pool::Task {
    std::function<void()> run;
    bool started = false; // both guarded by mutex_
    bool done = false;
};

struct Pool::Group {
    std::function<void(std::size_t)> run;
    std::size_t elements = 0;
    std::atomic<std::size_t> next{0};      // the next element to take
    std::atomic<std::size_t> processed{0}; // elements done
};

namespace {

// Which pool's worker the current thread is, and its number there.
thread_local const Pool* t_pool = nullptr;
thread_local int t_index = 0;

// Where std::thread can't start threads: the web without SharedArrayBuffer.
constexpr bool kHasThreads =
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    false;
#else
    true;
#endif

struct Shared {
    std::mutex mutex;
    int max_threads = -1;
    Pool* pool = nullptr;
};

Shared& shared_state() {
    static Shared state;
    return state;
}

} // namespace

Pool::Pool(int threads) {
    const int count = kHasThreads ? std::clamp(threads, 0, kMaxThreads) : 0;
    threads_.reserve(static_cast<std::size_t>(count));
    for (int i = 1; i <= count; ++i) {
        threads_.emplace_back([this, i] { work(i); });
    }
}

Pool::~Pool() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    queued_.notify_all();
    for (std::thread& thread : threads_) {
        thread.join();
    }
}

int Pool::thread_index() const noexcept {
    return t_pool == this ? t_index : 0;
}

void Pool::work(int index) {
    t_pool = this;
    t_index = index;
    std::unique_lock lock(mutex_);
    while (true) {
        queued_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) {
            return; // stopping, with nothing left to run
        }
        const Item item = std::move(queue_.front());
        queue_.pop_front();
        if (item.task != nullptr) {
            item.task->started = true;
        }
        lock.unlock();
        run(item);
        lock.lock();
    }
}

void Pool::run(const Item& item) {
    if (item.group != nullptr) {
        run_group(*item.group);
        return;
    }
    item.task->run();
    {
        const std::lock_guard lock(mutex_);
        item.task->done = true;
    }
    finished_.notify_all();
}

void Pool::run_group(Group& group) {
    while (true) {
        const std::size_t element = group.next.fetch_add(1);
        if (element >= group.elements) {
            return;
        }
        group.run(element);
        if (group.processed.fetch_add(1) + 1 == group.elements) {
            // Taking the lock means a waiter is either before its check or asleep, so it wakes.
            const std::lock_guard lock(mutex_);
            finished_.notify_all();
        }
    }
}

TaskId Pool::add_task(std::function<void()> task) {
    auto entry = std::make_shared<Task>();
    entry->run = std::move(task);
    std::unique_lock lock(mutex_);
    const TaskId id = next_id_++;
    tasks_.emplace(id, entry);
    if (threads_.empty()) {
        entry->started = true;
        lock.unlock();
        entry->run();
        lock.lock();
        entry->done = true;
        return id;
    }
    queue_.push_back({.task = std::move(entry), .group = nullptr});
    lock.unlock();
    queued_.notify_one();
    return id;
}

bool Pool::is_task_completed(TaskId id) const {
    const std::lock_guard lock(mutex_);
    const auto found = tasks_.find(id);
    SLASH_ASSERT(found != tasks_.end(), "no such task, or it was already waited for");
    return found->second->done;
}

void Pool::wait_for_task_completion(TaskId id) {
    std::unique_lock lock(mutex_);
    const auto found = tasks_.find(id);
    SLASH_ASSERT(found != tasks_.end(), "no such task, or it was already waited for");
    const std::shared_ptr<Task> task = found->second;
    tasks_.erase(found);
    if (!task->started) {
        // Still queued: run it here rather than wait for a thread to take it.
        std::erase_if(queue_, [&](const Item& item) { return item.task == task; });
        task->started = true;
        lock.unlock();
        task->run();
        return;
    }
    finished_.wait(lock, [&] { return task->done; });
}

GroupId Pool::add_group_task(std::function<void(std::size_t)> task, std::size_t elements,
                             int tasks_needed) {
    auto group = std::make_shared<Group>();
    group->run = std::move(task);
    group->elements = elements;
    const std::size_t runners =
        std::min({tasks_needed < 0 ? threads_.size() : static_cast<std::size_t>(tasks_needed),
                  threads_.size(), elements});
    std::unique_lock lock(mutex_);
    const GroupId id = next_id_++;
    groups_.emplace(id, group);
    if (runners == 0) {
        lock.unlock();
        run_group(*group);
        return id;
    }
    for (std::size_t i = 0; i < runners; ++i) {
        queue_.push_back({.task = nullptr, .group = group});
    }
    lock.unlock();
    if (runners == 1) {
        queued_.notify_one();
    } else {
        queued_.notify_all();
    }
    return id;
}

bool Pool::is_group_task_completed(GroupId id) const {
    const std::lock_guard lock(mutex_);
    const auto found = groups_.find(id);
    SLASH_ASSERT(found != groups_.end(), "no such group task, or it was already waited for");
    return found->second->processed.load() == found->second->elements;
}

std::size_t Pool::group_processed_element_count(GroupId id) const {
    const std::lock_guard lock(mutex_);
    const auto found = groups_.find(id);
    SLASH_ASSERT(found != groups_.end(), "no such group task, or it was already waited for");
    return found->second->processed.load();
}

void Pool::wait_for_group_task_completion(GroupId id) {
    std::unique_lock lock(mutex_);
    const auto found = groups_.find(id);
    SLASH_ASSERT(found != groups_.end(), "no such group task, or it was already waited for");
    const std::shared_ptr<Group> group = found->second;
    groups_.erase(found);
    lock.unlock();
    run_group(*group);
    lock.lock();
    // Runners no thread took have nothing left to do.
    std::erase_if(queue_, [&](const Item& item) { return item.group == group; });
    finished_.wait(lock, [&] { return group->processed.load() == group->elements; });
}

void Pool::parallel_for(std::size_t elements, const std::function<void(std::size_t)>& task) {
    if (threads_.empty() || elements < 2) {
        for (std::size_t i = 0; i < elements; ++i) {
            task(i);
        }
        return;
    }
    // This thread takes elements too, so one runner fewer than elements is enough.
    const GroupId id =
        add_group_task(task, elements, static_cast<int>(std::min(threads_.size(), elements - 1)));
    wait_for_group_task_completion(id);
}

int thread_count_for(int max_threads) {
    if (!kHasThreads) {
        return 0;
    }
    if (max_threads < 0) {
        max_threads = static_cast<int>(std::thread::hardware_concurrency()); // 0 if unknown
    }
    return std::clamp(max_threads, 0, kMaxThreads);
}

Pool& shared() {
    Shared& state = shared_state();
    const std::lock_guard lock(state.mutex);
    if (state.pool == nullptr) {
        // Never destroyed: its threads wait for work until the process ends, so nothing
        // destroyed at exit can stop a world stepping on them.
        state.pool = new Pool(thread_count_for(state.max_threads)); // NOLINT
    }
    return *state.pool;
}

bool configure(int max_threads) {
    Shared& state = shared_state();
    const std::lock_guard lock(state.mutex);
    if (state.pool != nullptr) {
        return state.pool->thread_count() == thread_count_for(max_threads);
    }
    state.max_threads = max_threads;
    return true;
}

json::Value settings_schema() {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] = "The job system, as Godot's threading/worker_pool settings.";
    json::Value& max_threads = schema["properties"]["max_threads"];
    max_threads["type"] = "integer";
    max_threads["minimum"] = -1;
    max_threads["maximum"] = kMaxThreads;
    max_threads["default"] = -1;
    max_threads["description"] =
        "Worker threads that threaded systems, particles and physics run on beside the main "
        "thread: -1 for one per CPU core, 0 to run everything on the main thread. Results are "
        "the same with any number.";
    return schema;
}

int parse_max_threads(const json::Value& threading) {
    const json::Value* max_threads = threading.find("max_threads");
    return max_threads != nullptr ? static_cast<int>(max_threads->as_int()) : -1;
}

} // namespace slash::jobs
