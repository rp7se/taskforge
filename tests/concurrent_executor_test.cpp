#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "taskforge/concurrent_executor.hpp"

namespace {

using namespace std::chrono_literals;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "test failure: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

template <typename Predicate>
void wait_until(Predicate&& predicate, const std::string& message,
                std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return;
        }
        std::this_thread::sleep_for(2ms);
    }
    require(predicate(), message);
}

taskforge::ProcessSpec helper(std::vector<std::string> arguments) {
    return {.executable = TASKFORGE_PROCESS_TEST_HELPER, .arguments = std::move(arguments)};
}

taskforge::TaskHandle take_accepted(taskforge::SubmitResult result) {
    require(result.status == taskforge::SubmitStatus::accepted, "submission was not accepted");
    require(result.handle.has_value(), "accepted submission did not return a handle");
    return std::move(*result.handle);
}

void require_ready(const taskforge::TaskHandle& handle, const std::string& message) {
    require(handle.wait_for(3s) == std::future_status::ready, message);
}

void require_success(const taskforge::TaskHandle& handle, const std::string& message) {
    require_ready(handle, message + " did not complete");
    const auto& result = handle.get();
    require(result.outcome == taskforge::ProcessOutcome::exited && result.exit_code == 0,
            message + " did not exit successfully");
    require(handle.state() == taskforge::TaskState::success, message + " did not map to success");
}

void wait_for_running(const taskforge::TaskHandle& handle, const std::string& message) {
    wait_until([&handle] { return handle.state() == taskforge::TaskState::running; }, message);
}

void test_basic_and_failure_mapping() {
    taskforge::ConcurrentExecutor executor(1, 4);

    auto success = take_accepted(executor.submit(helper({"exit", "0"})));
    require_success(success, "basic task");

    auto failed_exit = take_accepted(executor.submit(helper({"exit", "7"})));
    require_ready(failed_exit, "exit-7 task");
    require(failed_exit.get().outcome == taskforge::ProcessOutcome::exited &&
                failed_exit.get().exit_code == 7,
            "exit-7 result was not preserved");
    require(failed_exit.state() == taskforge::TaskState::failed, "exit-7 did not map to failed");

    auto startup_failure = take_accepted(executor.submit(
        {.executable = "/definitely/not/a/taskforge-executable", .arguments = {}}));
    require_ready(startup_failure, "startup-failure task");
    require(startup_failure.get().outcome == taskforge::ProcessOutcome::startup_failed,
            "startup failure result was not preserved");
    require(startup_failure.state() == taskforge::TaskState::failed,
            "startup failure did not map to failed");
}

void test_timeout_and_running_cancellation() {
    taskforge::ConcurrentExecutor executor(1, 4);

    auto timed_out = take_accepted(executor.submit(
        helper({"sleep", "500"}), {.timeout = 30ms, .stop_token = {}, .termination_grace = 50ms}));
    require_ready(timed_out, "timeout task");
    require(timed_out.get().outcome == taskforge::ProcessOutcome::timed_out,
            "timeout result was not preserved");
    require(timed_out.state() == taskforge::TaskState::timeout, "timeout did not map to timeout");

    std::stop_source stop_source;
    auto cancelled = take_accepted(executor.submit(
        helper({"sleep", "500"}),
        {.timeout = std::nullopt, .stop_token = stop_source.get_token(), .termination_grace = 50ms}));
    wait_for_running(cancelled, "cancellation task never reached running");
    stop_source.request_stop();
    require_ready(cancelled, "running cancellation task");
    require(cancelled.get().outcome == taskforge::ProcessOutcome::cancelled,
            "running cancellation result was not preserved");
    require(cancelled.state() == taskforge::TaskState::cancelled,
            "running cancellation did not map to cancelled");
}

void test_queue_full_and_capacity_recovery() {
    taskforge::ConcurrentExecutor executor(1, 1);
    auto first = take_accepted(executor.submit(helper({"sleep", "350"})));
    wait_for_running(first, "first task never reached running");

    auto second = take_accepted(executor.submit(helper({"sleep", "120"})));
    const auto full = executor.submit(helper({"exit", "0"}));
    require(full.status == taskforge::SubmitStatus::queue_full, "full queue did not reject submission");
    require(!full.handle.has_value(), "queue-full submission returned a handle");

    wait_until([&second] { return second.state() != taskforge::TaskState::queued; },
               "second task was never dequeued", 2s);
    auto third = take_accepted(executor.submit(helper({"exit", "0"})));

    require_success(first, "first queue-capacity task");
    require_success(second, "second queue-capacity task");
    require_success(third, "recovered queue-capacity task");
}

void test_fixed_concurrency() {
    taskforge::ConcurrentExecutor executor(2, 8);
    std::vector<taskforge::TaskHandle> handles;
    for (int index = 0; index < 6; ++index) {
        handles.push_back(take_accepted(executor.submit(helper({"sleep", "350"}))));
    }

    std::size_t maximum_running = 0;
    wait_until(
        [&handles, &maximum_running] {
            std::size_t running = 0;
            for (const auto& handle : handles) {
                if (handle.state() == taskforge::TaskState::running) {
                    ++running;
                }
            }
            maximum_running = std::max(maximum_running, running);
            return running == 2;
        },
        "two workers were never concurrently running", 2s);
    require(maximum_running <= 2, "observed more running tasks than worker count");
    for (const auto& handle : handles) {
        require_success(handle, "fixed-concurrency task");
    }
}

void test_concurrent_producers_and_no_task_loss() {
    constexpr std::size_t producer_count = 8;
    constexpr int race_iterations = 50;
    for (int iteration = 0; iteration < race_iterations; ++iteration) {
        taskforge::ConcurrentExecutor executor(2, 3);
        auto first = take_accepted(executor.submit(helper({"sleep", "80"})));
        auto second = take_accepted(executor.submit(helper({"sleep", "80"})));
        wait_for_running(first, "first producer guard task did not run");
        wait_for_running(second, "second producer guard task did not run");

        std::barrier gate(static_cast<std::ptrdiff_t>(producer_count));
        std::vector<taskforge::SubmitResult> submissions(producer_count);
        std::vector<std::jthread> producers;
        producers.reserve(producer_count);
        for (std::size_t index = 0; index < producer_count; ++index) {
            producers.emplace_back([&executor, &gate, &submissions, index] {
                gate.arrive_and_wait();
                submissions[index] = executor.submit(helper({"exit", "0"}));
            });
        }
        producers.clear();

        std::vector<taskforge::TaskHandle> accepted;
        for (auto& submission : submissions) {
            if (submission.status == taskforge::SubmitStatus::accepted) {
                require(submission.handle.has_value(), "accepted producer task has no handle");
                accepted.push_back(std::move(*submission.handle));
            } else {
                require(submission.status == taskforge::SubmitStatus::queue_full,
                        "producer submission had an unexpected status");
                require(!submission.handle.has_value(), "rejected producer task has a handle");
            }
        }
        require(accepted.size() == 3,
                "concurrent producers did not accept exactly queue capacity");

        std::set<std::uint64_t> ids;
        ids.insert(first.id());
        ids.insert(second.id());
        for (const auto& handle : accepted) {
            ids.insert(handle.id());
            require_success(handle, "accepted producer task");
        }
        require_success(first, "first producer guard task");
        require_success(second, "second producer guard task");
        require(ids.size() == accepted.size() + 2, "accepted task IDs were not unique");
    }
}

void test_no_task_loss_and_shutdown_drain() {
    taskforge::ConcurrentExecutor executor(2, 16);
    std::vector<taskforge::TaskHandle> handles;
    for (int index = 0; index < 10; ++index) {
        handles.push_back(take_accepted(executor.submit(helper({"sleep", "40"}))));
    }
    executor.shutdown();
    executor.shutdown();

    std::set<std::uint64_t> ids;
    for (const auto& handle : handles) {
        require_ready(handle, "drained accepted task");
        require_success(handle, "drained accepted task");
        ids.insert(handle.id());
    }
    require(ids.size() == handles.size(), "drain test observed duplicate IDs");

    const auto rejected = executor.submit(helper({"exit", "0"}));
    require(rejected.status == taskforge::SubmitStatus::shutting_down,
            "shutdown did not reject new submission");
    require(!rejected.handle.has_value(), "shutdown rejection returned a handle");
}

void test_queued_cancellation() {
    taskforge::ConcurrentExecutor executor(1, 1);
    auto first = take_accepted(executor.submit(helper({"sleep", "300"})));
    wait_for_running(first, "queued-cancellation guard task did not run");

    std::stop_source stop_source;
    auto queued = take_accepted(executor.submit(
        helper({"stdout"}), {.timeout = std::nullopt, .stop_token = stop_source.get_token()}));
    require(queued.state() == taskforge::TaskState::queued,
            "queued-cancellation task did not remain waiting behind worker");
    stop_source.request_stop();
    const auto still_full = executor.submit(helper({"exit", "0"}));
    require(still_full.status == taskforge::SubmitStatus::queue_full,
            "cancelled queued task did not continue occupying its queue slot");

    require_success(first, "queued-cancellation guard task");
    require_ready(queued, "queued cancellation task");
    require(queued.get().outcome == taskforge::ProcessOutcome::cancelled,
            "queued cancellation did not produce cancelled result");
    require(queued.get().stdout_data.empty(), "queued cancellation invoked the stdout helper");
    require(queued.state() == taskforge::TaskState::cancelled,
            "queued cancellation did not map to cancelled");
}

void test_shutdown_submit_race() {
    constexpr std::size_t producer_count = 8;
    constexpr int race_iterations = 50;
    for (int iteration = 0; iteration < race_iterations; ++iteration) {
        taskforge::ConcurrentExecutor executor(2, 12);
        std::barrier gate(static_cast<std::ptrdiff_t>(producer_count + 1));
        std::vector<taskforge::SubmitResult> submissions(producer_count);
        std::vector<std::jthread> producers;
        producers.reserve(producer_count);
        for (std::size_t index = 0; index < producer_count; ++index) {
            producers.emplace_back([&executor, &gate, &submissions, index] {
                gate.arrive_and_wait();
                submissions[index] = executor.submit(helper({"exit", "0"}));
            });
        }
        gate.arrive_and_wait();
        executor.shutdown();
        producers.clear();

        for (auto& submission : submissions) {
            require(submission.status == taskforge::SubmitStatus::accepted ||
                        submission.status == taskforge::SubmitStatus::queue_full ||
                        submission.status == taskforge::SubmitStatus::shutting_down,
                    "shutdown race returned an invalid status");
            if (submission.status == taskforge::SubmitStatus::accepted) {
                require(submission.handle.has_value(), "accepted shutdown-race task has no handle");
                require_ready(*submission.handle, "accepted shutdown-race task");
                require_success(*submission.handle, "accepted shutdown-race task");
            } else {
                require(!submission.handle.has_value(), "rejected shutdown-race task has a handle");
            }
        }
    }
}

}  // namespace

int main() {
    test_basic_and_failure_mapping();
    test_timeout_and_running_cancellation();
    test_queue_full_and_capacity_recovery();
    test_fixed_concurrency();
    test_concurrent_producers_and_no_task_loss();
    test_no_task_loss_and_shutdown_drain();
    test_queued_cancellation();
    test_shutdown_submit_race();
    return 0;
}
