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

constexpr std::uint64_t mib = 1024U * 1024U;

taskforge::ConcurrentExecutor resource_executor(std::size_t workers, std::size_t queue_capacity,
                                                std::uint64_t cpu, std::uint64_t memory) {
    return taskforge::ConcurrentExecutor({.worker_count = workers,
                                          .queue_capacity = queue_capacity,
                                          .resource_capacity = taskforge::ResourceCapacity{
                                              .cpu_millis = cpu, .memory_bytes = memory}});
}

taskforge::ResourceRequest request(std::uint64_t cpu, std::uint64_t memory) {
    return {.cpu_millis = cpu, .memory_bytes = memory};
}

std::size_t running_count(const std::vector<taskforge::TaskHandle>& handles) {
    return static_cast<std::size_t>(std::count_if(handles.begin(), handles.end(), [](const auto& handle) {
        return handle.state() == taskforge::TaskState::running;
    }));
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

void test_resource_request_validation() {
    auto executor = resource_executor(1, 4, 1000, 128 * mib);
    require(executor.submit(helper({"exit", "0"})).status ==
                taskforge::SubmitStatus::resource_request_required,
            "managed executor accepted legacy submission without request");
    require(executor.submit(helper({"exit", "0"}), {}, request(0, mib)).status ==
                taskforge::SubmitStatus::invalid_resource_request,
            "zero CPU request was not rejected");
    require(executor.submit(helper({"exit", "0"}), {}, request(1, 0)).status ==
                taskforge::SubmitStatus::invalid_resource_request,
            "zero memory request was not rejected");
    require(executor.submit(helper({"exit", "0"}), {}, request(1001, mib)).status ==
                taskforge::SubmitStatus::resource_request_exceeds_capacity,
            "CPU request exceeding capacity was not rejected");
    require(executor.submit(helper({"exit", "0"}), {}, request(1, 129 * mib)).status ==
                taskforge::SubmitStatus::resource_request_exceeds_capacity,
            "memory request exceeding capacity was not rejected");
    require(!executor.resource_snapshot()->reserved.cpu_millis &&
                !executor.resource_snapshot()->reserved.memory_bytes,
            "rejected requests changed reservation accounting");

    bool rejected_zero_capacity = false;
    try {
        [[maybe_unused]] auto invalid = taskforge::ConcurrentExecutor(
            {.worker_count = 1, .queue_capacity = 1,
             .resource_capacity = taskforge::ResourceCapacity{.cpu_millis = 0, .memory_bytes = mib}});
    } catch (const std::invalid_argument&) {
        rejected_zero_capacity = true;
    }
    require(rejected_zero_capacity, "zero configured resource capacity was accepted");
}

void test_basic_resource_admission_and_combined_capacity() {
    auto executor = resource_executor(2, 4, 2000, 256 * mib);
    std::vector<taskforge::TaskHandle> handles;
    handles.push_back(take_accepted(executor.submit(helper({"sleep", "180"}), {}, request(1000, 128 * mib))));
    handles.push_back(take_accepted(executor.submit(helper({"sleep", "180"}), {}, request(1000, 128 * mib))));
    wait_until([&] { return running_count(handles) == 2; }, "full configured budget was not used");
    const auto snapshot = *executor.resource_snapshot();
    require(snapshot.reserved.cpu_millis == 2000 && snapshot.reserved.memory_bytes == 256 * mib,
            "combined resource reservation was not exact");
    for (const auto& handle : handles) require_success(handle, "basic resource-admission task");
}

void test_cpu_memory_and_multi_dimension_admission() {
    {
        auto executor = resource_executor(2, 4, 1000, 256 * mib);
        auto first = take_accepted(executor.submit(helper({"sleep", "140"}), {}, request(700, mib)));
        wait_for_running(first, "CPU guard task did not run");
        auto second = take_accepted(executor.submit(helper({"sleep", "40"}), {}, request(700, mib)));
        std::this_thread::sleep_for(25ms);
        require(second.state() == taskforge::TaskState::queued, "CPU budget allowed over-admission");
        require_success(first, "CPU guard task");
        require_success(second, "CPU serialized task");
    }
    {
        auto executor = resource_executor(2, 4, 2000, 100 * mib);
        auto first = take_accepted(executor.submit(helper({"sleep", "140"}), {}, request(1, 60 * mib)));
        wait_for_running(first, "memory guard task did not run");
        auto second = take_accepted(executor.submit(helper({"sleep", "40"}), {}, request(1, 60 * mib)));
        std::this_thread::sleep_for(25ms);
        require(second.state() == taskforge::TaskState::queued, "memory budget allowed over-admission");
        require_success(first, "memory guard task");
        require_success(second, "memory serialized task");
    }
    {
        auto executor = resource_executor(2, 4, 1000, 100 * mib);
        auto cpu_guard = take_accepted(executor.submit(helper({"sleep", "130"}), {}, request(700, 10 * mib)));
        wait_for_running(cpu_guard, "multi-dimensional CPU guard did not run");
        auto cpu_blocked = take_accepted(executor.submit(helper({"exit", "0"}), {}, request(400, 10 * mib)));
        std::this_thread::sleep_for(20ms);
        require(cpu_blocked.state() == taskforge::TaskState::queued &&
                    executor.resource_snapshot()->reserved.cpu_millis == 700 &&
                    executor.resource_snapshot()->reserved.memory_bytes == 10 * mib,
                "CPU failure created a partial reservation");
        require_success(cpu_guard, "multi-dimensional CPU guard");
        require_success(cpu_blocked, "multi-dimensional CPU blocked task");
    }
    {
        auto executor = resource_executor(2, 4, 1000, 100 * mib);
        auto memory_guard = take_accepted(executor.submit(helper({"sleep", "130"}), {}, request(10, 70 * mib)));
        wait_for_running(memory_guard, "multi-dimensional memory guard did not run");
        auto memory_blocked = take_accepted(executor.submit(helper({"exit", "0"}), {}, request(10, 40 * mib)));
        std::this_thread::sleep_for(20ms);
        require(memory_blocked.state() == taskforge::TaskState::queued &&
                    executor.resource_snapshot()->reserved.cpu_millis == 10 &&
                    executor.resource_snapshot()->reserved.memory_bytes == 70 * mib,
                "memory failure created a partial reservation");
        require_success(memory_guard, "multi-dimensional memory guard");
        require_success(memory_blocked, "multi-dimensional memory blocked task");
    }
}

void test_release_success_failure_timeout_and_cancellation() {
    auto run_release_case = [](taskforge::ProcessSpec first_spec, taskforge::ProcessExecutionOptions options,
                               taskforge::ProcessOutcome expected, std::stop_source* stop_source = nullptr) {
        auto executor = resource_executor(2, 4, 1000, 100 * mib);
        auto first = take_accepted(executor.submit(std::move(first_spec), std::move(options), request(1000, 100 * mib)));
        wait_for_running(first, "release guard task did not run");
        auto second = take_accepted(executor.submit(helper({"exit", "0"}), {}, request(1000, 100 * mib)));
        require(second.state() == taskforge::TaskState::queued, "fully reserved task was admitted early");
        if (stop_source != nullptr) stop_source->request_stop();
        require_ready(first, "release guard task did not complete");
        require(first.get().outcome == expected, "release guard had unexpected outcome");
        require_success(second, "reservation was not released after terminal result");
    };
    run_release_case(helper({"sleep", "80"}), {}, taskforge::ProcessOutcome::exited);
    run_release_case(helper({"exit-after", "80", "7"}), {}, taskforge::ProcessOutcome::exited);
    run_release_case(helper({"sleep", "400"}), {.timeout = 25ms, .termination_grace = 30ms},
                     taskforge::ProcessOutcome::timed_out);
    std::stop_source source;
    run_release_case(helper({"sleep", "400"}), {.stop_token = source.get_token(), .termination_grace = 30ms},
                     taskforge::ProcessOutcome::cancelled, &source);
}

void test_queued_cancellation_fifo_queue_and_shutdown_drain() {
    auto executor = resource_executor(2, 3, 1000, 300 * mib);
    auto first = take_accepted(executor.submit(helper({"sleep", "150"}), {}, request(700, 100 * mib)));
    wait_for_running(first, "FIFO guard did not run");
    std::stop_source stop_source;
    auto cancelled = take_accepted(executor.submit(helper({"stdout"}), {.stop_token = stop_source.get_token()}, request(500, 100 * mib)));
    auto after_cancelled = take_accepted(executor.submit(helper({"exit", "0"}), {}, request(200, 100 * mib)));
    auto queued_tail = take_accepted(executor.submit(helper({"exit", "0"}), {}, request(1, 1)));
    require(executor.submit(helper({"exit", "0"}), {}, request(1, 1)).status == taskforge::SubmitStatus::queue_full,
            "resource-blocked queue did not retain bounded-queue slot");
    std::this_thread::sleep_for(20ms);
    require(cancelled.state() == taskforge::TaskState::queued && after_cancelled.state() == taskforge::TaskState::queued,
            "strict FIFO allowed backfill around blocked head");
    stop_source.request_stop();
    require_success(first, "FIFO guard");
    require_ready(cancelled, "queued cancellation was not drained");
    require(cancelled.get().outcome == taskforge::ProcessOutcome::cancelled,
            "queued cancellation ran instead of completing cancelled");
    require_success(after_cancelled, "task after cancelled queue head");
    require_success(queued_tail, "bounded queue tail task");
    executor.shutdown();
}

void test_cgroup_options_pass_through_resource_executor() {
    auto executor = resource_executor(1, 2, 1000, 100 * mib);
    auto task = take_accepted(executor.submit(
        helper({"exit", "0"}),
        {.cgroup = taskforge::CgroupV2Options{.root = "/definitely/not/a/taskforge-cgroup",
                                               .limits = {.pids_max = 4}}},
        request(1000, 100 * mib)));
    require_ready(task, "cgroup process options did not reach the concurrent executor");
    require(task.get().outcome == taskforge::ProcessOutcome::parent_error,
            "cgroup process option was not passed unchanged to run_process");
}

void test_shutdown_drains_resource_blocked_queue() {
    auto executor = resource_executor(2, 4, 1000, 100 * mib);
    auto first = take_accepted(executor.submit(helper({"sleep", "100"}), {}, request(1000, 100 * mib)));
    wait_for_running(first, "shutdown-drain guard did not run");
    auto second = take_accepted(executor.submit(helper({"sleep", "30"}), {}, request(1000, 100 * mib)));
    auto third = take_accepted(executor.submit(helper({"exit", "0"}), {}, request(1000, 100 * mib)));
    require(second.state() == taskforge::TaskState::queued && third.state() == taskforge::TaskState::queued,
            "shutdown-drain tasks were not resource blocked");
    executor.shutdown();
    require_success(first, "shutdown-drain first task");
    require_success(second, "shutdown-drain second task");
    require_success(third, "shutdown-drain third task");
}

void test_atomic_reservation_race() {
    for (int iteration = 0; iteration < 50; ++iteration) {
        auto executor = resource_executor(4, 8, 1000, 100 * mib);
        std::vector<taskforge::TaskHandle> handles;
        for (int index = 0; index < 6; ++index) {
            handles.push_back(take_accepted(executor.submit(helper({"sleep", "5"}), {}, request(600, 60 * mib))));
        }
        std::size_t max_running = 0;
        while (std::any_of(handles.begin(), handles.end(), [](const auto& handle) {
            return handle.wait_for(0ms) != std::future_status::ready;
        })) {
            max_running = std::max(max_running, running_count(handles));
            const auto snapshot = *executor.resource_snapshot();
            require(snapshot.reserved.cpu_millis <= snapshot.capacity.cpu_millis &&
                        snapshot.reserved.memory_bytes <= snapshot.capacity.memory_bytes,
                    "atomic admission overcommitted configured capacity");
            std::this_thread::sleep_for(1ms);
        }
        require(max_running <= 1, "reservation race admitted multiple 600m/60MiB tasks");
        for (const auto& handle : handles) require_success(handle, "atomic reservation race task");
    }
}

void test_resource_submit_shutdown_race() {
    for (int iteration = 0; iteration < 15; ++iteration) {
        auto executor = resource_executor(2, 8, 1000, 100 * mib);
        std::barrier gate(5);
        std::vector<taskforge::SubmitResult> submissions(4);
        std::vector<std::jthread> producers;
        for (std::size_t index = 0; index < submissions.size(); ++index) {
            producers.emplace_back([&, index] {
                gate.arrive_and_wait();
                submissions[index] = executor.submit(helper({"exit", "0"}), {}, request(1000, 100 * mib));
            });
        }
        gate.arrive_and_wait();
        executor.shutdown();
        for (auto& producer : producers) {
            producer.join();
        }
        for (const auto& submission : submissions) {
            require(submission.status == taskforge::SubmitStatus::accepted ||
                        submission.status == taskforge::SubmitStatus::queue_full ||
                        submission.status == taskforge::SubmitStatus::shutting_down,
                    "resource submit/shutdown race returned invalid status");
            if (submission.accepted()) require_success(*submission.handle, "accepted resource race task");
        }
    }
}

void test_bounded_output_options_pass_through_concurrent_executor() {
    taskforge::ConcurrentExecutor executor(1, 2);
    auto task = take_accepted(executor.submit(
        helper({"dual-write", "100", "100"}),
        {.output_capture_limits = taskforge::OutputCaptureLimits{.stdout_bytes = 11,
                                                                  .stderr_bytes = 7}}));
    require_ready(task, "bounded-output concurrent task did not complete");
    const auto& result = task.get();
    require(result.outcome == taskforge::ProcessOutcome::exited && result.exit_code == 0 &&
                result.stdout_data.size() == 11 && result.stderr_data.size() == 7 &&
                result.stdout_total_bytes == 100 && result.stderr_total_bytes == 100 &&
                result.stdout_truncated && result.stderr_truncated,
            "ConcurrentExecutor did not pass bounded output options to run_process");
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
    test_resource_request_validation();
    test_basic_resource_admission_and_combined_capacity();
    test_cpu_memory_and_multi_dimension_admission();
    test_release_success_failure_timeout_and_cancellation();
    test_queued_cancellation_fifo_queue_and_shutdown_drain();
    test_cgroup_options_pass_through_resource_executor();
    test_shutdown_drains_resource_blocked_queue();
    test_atomic_reservation_race();
    test_resource_submit_shutdown_race();
    test_bounded_output_options_pass_through_concurrent_executor();
    return 0;
}
