#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "slash/shader_compiler/shader_compiler.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace {

// The tests compile a game's shaders, as the tools do.
[[maybe_unused]] const bool kShaderCompilerInstalled = [] {
    slash::shader_compiler::install();
    return true;
}();

// A test case that runs this long is stuck. The watchdog names it and ends the run, so CI fails
// with the culprit instead of waiting for the job's time limit.
constexpr auto kTestCaseLimit = std::chrono::minutes(3);

class Watchdog : public doctest::IReporter {
public:
    explicit Watchdog(const doctest::ContextOptions& /*options*/) {}

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    Watchdog(Watchdog&&) = delete;
    Watchdog& operator=(Watchdog&&) = delete;

    ~Watchdog() override {
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void test_run_start() override {
        thread_ = std::thread([this] { watch(); });
    }

    void test_case_start(const doctest::TestCaseData& data) override {
        {
            const std::lock_guard lock(mutex_);
            name_ = data.m_name;
            file_ = data.m_file.c_str();
            started_ = std::chrono::steady_clock::now();
            running_ = true;
        }
        changed_.notify_all();
    }

    void test_case_end(const doctest::CurrentTestCaseStats& /*stats*/) override {
        const std::lock_guard lock(mutex_);
        running_ = false;
    }

    // ctest prints a stuck run's output only if it reached the pipe.
    void test_run_end(const doctest::TestRunStats& /*stats*/) override { std::fflush(stdout); }

    void report_query(const doctest::QueryData& /*data*/) override {}
    void test_case_reenter(const doctest::TestCaseData& /*data*/) override {}
    void test_case_exception(const doctest::TestCaseException& /*exception*/) override {}
    void subcase_start(const doctest::SubcaseSignature& /*signature*/) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData& /*data*/) override {}
    void log_message(const doctest::MessageData& /*data*/) override {}
    void test_case_skipped(const doctest::TestCaseData& /*data*/) override {}

private:
    void watch() {
        std::unique_lock lock(mutex_);
        while (!stopping_) {
            if (!running_) {
                changed_.wait(lock);
                continue;
            }
            const auto started = started_;
            const auto deadline = started + kTestCaseLimit;
            if (changed_.wait_until(lock, deadline) == std::cv_status::timeout && running_ &&
                started_ == started && std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr, "test case \"%s\" (%s) ran for over %lld s; stopping\n",
                             name_.c_str(), file_.c_str(),
                             static_cast<long long>(std::chrono::seconds(kTestCaseLimit).count()));
                std::fflush(stderr);
                std::_Exit(EXIT_FAILURE);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::thread thread_;
    std::string name_;
    std::string file_;
    std::chrono::steady_clock::time_point started_;
    bool running_ = false;
    bool stopping_ = false;
};

REGISTER_LISTENER("watchdog", 1, Watchdog);

} // namespace
