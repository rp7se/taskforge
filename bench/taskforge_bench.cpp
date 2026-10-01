#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "taskforge/concurrent_executor.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::uint64_t mib = 1024ULL * 1024ULL;

struct Options {
    std::string scenario = "all";
    std::string format = "text";
    std::size_t warmup = 10;
    std::size_t iterations = 100;
    std::size_t tasks = 100;
    std::size_t workers = 4;
};

struct ScenarioResult {
    std::string scenario;
    std::uint64_t elapsed_ms = 0;
    double tasks_per_second = 0.0;
    std::map<std::string, std::string> metrics;
};

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

void require(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

taskforge::ProcessSpec helper(std::vector<std::string> arguments) {
    return {.executable = TASKFORGE_PROCESS_TEST_HELPER, .arguments = std::move(arguments)};
}

taskforge::TaskHandle accepted(taskforge::SubmitResult submission, const std::string& context) {
    require(submission.status == taskforge::SubmitStatus::accepted, context + " was not accepted");
    require(submission.handle.has_value(), context + " did not return a handle");
    return std::move(*submission.handle);
}

void require_exit_zero(const taskforge::ProcessResult& result, const std::string& context) {
    require(result.outcome == taskforge::ProcessOutcome::exited && result.exit_code == 0,
            context + " did not exit zero");
}

std::uint64_t elapsed_ms(Clock::time_point started) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
}

double per_second(std::size_t count, std::uint64_t milliseconds) {
    return milliseconds == 0 ? 0.0 : static_cast<double>(count) * 1000.0 / static_cast<double>(milliseconds);
}

std::uint64_t percentile_us(std::vector<std::uint64_t> samples, double percentile) {
    require(!samples.empty(), "percentile requires samples");
    std::sort(samples.begin(), samples.end());
    const auto index = static_cast<std::size_t>(std::ceil(percentile * samples.size())) - 1;
    return samples.at(index);
}

std::size_t count_running(const std::vector<taskforge::TaskHandle>& handles) {
    return static_cast<std::size_t>(std::count_if(handles.begin(), handles.end(), [](const auto& handle) {
        return handle.state() == taskforge::TaskState::running;
    }));
}

void wait_for_running(const taskforge::TaskHandle& handle, const std::string& context) {
    const auto deadline = Clock::now() + 3s;
    while (Clock::now() < deadline) {
        if (handle.state() == taskforge::TaskState::running) return;
        std::this_thread::sleep_for(1ms);
    }
    require(handle.state() == taskforge::TaskState::running, context + " never reached running");
}

std::size_t observe_max_running(const std::vector<taskforge::TaskHandle>& handles) {
    std::size_t maximum = 0;
    bool pending = true;
    while (pending) {
        maximum = std::max(maximum, count_running(handles));
        pending = std::any_of(handles.begin(), handles.end(), [](const auto& handle) {
            return handle.wait_for(0ms) != std::future_status::ready;
        });
        if (pending) std::this_thread::sleep_for(1ms);
    }
    maximum = std::max(maximum, count_running(handles));
    return maximum;
}

void complete_exit_zero(const std::vector<taskforge::TaskHandle>& handles, const std::string& context) {
    for (const auto& handle : handles) require_exit_zero(handle.get(), context);
}

std::string number(double value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << value;
    return stream.str();
}

ScenarioResult launch(const Options& options) {
    for (std::size_t index = 0; index < options.warmup; ++index) {
        require_exit_zero(taskforge::run_process(helper({"exit", "0"})), "launch warm-up");
    }

    std::vector<std::uint64_t> samples;
    samples.reserve(options.iterations);
    const auto started = Clock::now();
    for (std::size_t index = 0; index < options.iterations; ++index) {
        const auto one_started = Clock::now();
        require_exit_zero(taskforge::run_process(helper({"exit", "0"})), "launch measurement");
        samples.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - one_started).count()));
    }
    ScenarioResult result{"launch", elapsed_ms(started), per_second(options.iterations, elapsed_ms(started))};
    result.metrics["warmup"] = std::to_string(options.warmup);
    result.metrics["iterations"] = std::to_string(options.iterations);
    result.metrics["min_us"] = std::to_string(*std::min_element(samples.begin(), samples.end()));
    result.metrics["p50_us"] = std::to_string(percentile_us(samples, 0.50));
    result.metrics["p95_us"] = std::to_string(percentile_us(samples, 0.95));
    result.metrics["max_us"] = std::to_string(*std::max_element(samples.begin(), samples.end()));
    return result;
}

ScenarioResult scaling(const Options& options) {
    const std::vector<std::size_t> workers = [] {
        std::vector<std::size_t> values{1, 2, 4};
        if (std::thread::hardware_concurrency() >= 8) values.push_back(8);
        return values;
    }();
    const std::size_t tasks = std::max<std::size_t>(options.tasks, 4);
    ScenarioResult result{"worker_scaling", 0, 0.0};
    result.metrics["tasks"] = std::to_string(tasks);
    result.metrics["sleep_ms"] = "25";
    for (const auto worker_count : workers) {
        taskforge::ConcurrentExecutor executor(worker_count, tasks + worker_count + 1);
        std::vector<taskforge::TaskHandle> handles;
        handles.reserve(tasks);
        const auto started = Clock::now();
        for (std::size_t index = 0; index < tasks; ++index) {
            handles.push_back(accepted(executor.submit(helper({"sleep", "25"})), "scaling task"));
        }
        complete_exit_zero(handles, "scaling task");
        const auto milliseconds = elapsed_ms(started);
        result.metrics["worker_" + std::to_string(worker_count) + "_elapsed_ms"] = std::to_string(milliseconds);
        result.metrics["worker_" + std::to_string(worker_count) + "_tasks_per_second"] = number(per_second(tasks, milliseconds));
    }
    return result;
}

ScenarioResult concurrency(const Options& options) {
    const std::size_t workers = std::max<std::size_t>(2, options.workers);
    taskforge::ConcurrentExecutor executor(workers, workers * 3);
    std::vector<taskforge::TaskHandle> handles;
    for (std::size_t index = 0; index < workers * 2; ++index) {
        handles.push_back(accepted(executor.submit(helper({"sleep", "100"})), "concurrency task"));
    }
    const auto started = Clock::now();
    const auto maximum = observe_max_running(handles);
    complete_exit_zero(handles, "concurrency task");
    require(maximum <= workers, "worker concurrency exceeded configured workers");
    require(maximum == workers, "workload did not reach configured worker concurrency");
    ScenarioResult result{"worker_concurrency", elapsed_ms(started), per_second(handles.size(), elapsed_ms(started))};
    result.metrics["configured_workers"] = std::to_string(workers);
    result.metrics["observed_max_running"] = std::to_string(maximum);
    result.metrics["invariant"] = "true";
    return result;
}

ScenarioResult resource(const Options& options) {
    const std::size_t tasks = std::max<std::size_t>(options.tasks, 4);
    taskforge::ConcurrentExecutor executor({.worker_count = 4,
                                            .queue_capacity = tasks + 4,
                                            .resource_capacity = taskforge::ResourceCapacity{
                                                .cpu_millis = 2000, .memory_bytes = 512 * mib}});
    std::vector<taskforge::TaskHandle> handles;
    const auto managed_started = Clock::now();
    for (std::size_t index = 0; index < tasks; ++index) {
        handles.push_back(accepted(executor.submit(helper({"sleep", "80"}), {},
                                                  {.cpu_millis = 1000, .memory_bytes = mib}),
                                  "resource task"));
    }
    const auto maximum = observe_max_running(handles);
    complete_exit_zero(handles, "resource task");
    const auto managed_elapsed = elapsed_ms(managed_started);
    const auto snapshot = executor.resource_snapshot();
    require(snapshot.has_value(), "resource snapshot unavailable");
    require(snapshot->reserved.cpu_millis == 0 && snapshot->reserved.memory_bytes == 0,
            "resource reservations remained after completion");
    require(maximum <= 2, "resource admission exceeded two logical CPU reservations");

    taskforge::ConcurrentExecutor unbounded(4, tasks + 4);
    std::vector<taskforge::TaskHandle> control;
    const auto control_started = Clock::now();
    for (std::size_t index = 0; index < tasks; ++index) {
        control.push_back(accepted(unbounded.submit(helper({"sleep", "80"})), "resource control task"));
    }
    const auto control_maximum = observe_max_running(control);
    complete_exit_zero(control, "resource control task");
    const auto control_elapsed = elapsed_ms(control_started);
    require(control_maximum == 4, "resource-disabled control did not reach four workers");

    ScenarioResult result{"resource_admission", managed_elapsed, per_second(tasks, managed_elapsed)};
    result.metrics["worker_count"] = "4";
    result.metrics["resource_cpu_capacity_millis"] = "2000";
    result.metrics["task_cpu_request_millis"] = "1000";
    result.metrics["observed_max_running"] = std::to_string(maximum);
    result.metrics["resource_disabled_observed_max_running"] = std::to_string(control_maximum);
    result.metrics["resource_disabled_elapsed_ms"] = std::to_string(control_elapsed);
    result.metrics["resource_disabled_tasks_per_second"] = number(per_second(tasks, control_elapsed));
    result.metrics["reserved_after_completion_cpu_millis"] = "0";
    result.metrics["reserved_after_completion_memory_bytes"] = "0";
    return result;
}

ScenarioResult memory_admission(const Options& options) {
    const std::size_t tasks = std::max<std::size_t>(options.tasks, 4);
    taskforge::ConcurrentExecutor executor({.worker_count = 3,
                                            .queue_capacity = tasks + 3,
                                            .resource_capacity = taskforge::ResourceCapacity{
                                                .cpu_millis = 3000, .memory_bytes = 256 * mib}});
    std::vector<taskforge::TaskHandle> handles;
    const auto started = Clock::now();
    for (std::size_t index = 0; index < tasks; ++index) {
        handles.push_back(accepted(executor.submit(helper({"sleep", "80"}), {},
                                                  {.cpu_millis = 1000, .memory_bytes = 128 * mib}),
                                  "memory resource task"));
    }
    const auto maximum = observe_max_running(handles);
    complete_exit_zero(handles, "memory resource task");
    const auto snapshot = executor.resource_snapshot();
    require(snapshot.has_value() && snapshot->reserved.cpu_millis == 0 && snapshot->reserved.memory_bytes == 0,
            "memory admission leaked a reservation");
    require(maximum <= 2, "memory admission exceeded two logical reservations");
    ScenarioResult result{"memory_admission", elapsed_ms(started), per_second(tasks, elapsed_ms(started))};
    result.metrics["worker_count"] = "3";
    result.metrics["memory_capacity_bytes"] = std::to_string(256 * mib);
    result.metrics["task_memory_request_bytes"] = std::to_string(128 * mib);
    result.metrics["observed_max_running"] = std::to_string(maximum);
    return result;
}

ScenarioResult backpressure(const Options&) {
    taskforge::ConcurrentExecutor executor(2, 8);
    std::vector<taskforge::TaskHandle> accepted_handles;
    accepted_handles.push_back(accepted(executor.submit(helper({"sleep", "200"})), "backpressure guard"));
    accepted_handles.push_back(accepted(executor.submit(helper({"sleep", "200"})), "backpressure guard"));
    wait_for_running(accepted_handles[0], "first backpressure guard");
    wait_for_running(accepted_handles[1], "second backpressure guard");
    std::size_t queue_full = 0;
    const auto started = Clock::now();
    for (std::size_t index = 0; index < 32; ++index) {
        auto submission = executor.submit(helper({"exit", "0"}));
        if (submission.status == taskforge::SubmitStatus::accepted) {
            accepted_handles.push_back(std::move(*submission.handle));
        } else if (submission.status == taskforge::SubmitStatus::queue_full) {
            ++queue_full;
        } else {
            fail("backpressure submission returned unexpected status");
        }
    }
    complete_exit_zero(accepted_handles, "backpressure accepted task");
    require(queue_full > 0, "backpressure benchmark did not produce queue_full");
    ScenarioResult result{"bounded_queue_backpressure", elapsed_ms(started),
                          per_second(accepted_handles.size(), elapsed_ms(started))};
    result.metrics["worker_count"] = "2";
    result.metrics["queue_capacity"] = "8";
    result.metrics["burst_submissions"] = "32";
    result.metrics["accepted"] = std::to_string(accepted_handles.size());
    result.metrics["queue_full"] = std::to_string(queue_full);
    result.metrics["completed"] = std::to_string(accepted_handles.size());
    return result;
}

ScenarioResult output_capture(const Options&) {
    constexpr std::uint64_t bytes = 16 * mib;
    constexpr std::uint64_t retained = 64 * 1024;
    const auto started = Clock::now();
    const auto result = taskforge::run_process(
        helper({"dual-write", std::to_string(bytes), std::to_string(bytes)}),
        {.output_capture_limits = taskforge::OutputCaptureLimits{.stdout_bytes = retained, .stderr_bytes = retained}});
    require_exit_zero(result, "output benchmark");
    require(result.stdout_total_bytes == bytes && result.stderr_total_bytes == bytes,
            "output benchmark did not drain requested bytes");
    require(result.stdout_data.size() <= retained && result.stderr_data.size() <= retained &&
                result.stdout_truncated && result.stderr_truncated,
            "output benchmark retained output beyond its configured limit");
    ScenarioResult report{"bounded_output_capture", elapsed_ms(started), 0.0};
    report.metrics["stdout_total_bytes"] = std::to_string(result.stdout_total_bytes);
    report.metrics["stderr_total_bytes"] = std::to_string(result.stderr_total_bytes);
    report.metrics["stdout_retained_bytes"] = std::to_string(result.stdout_data.size());
    report.metrics["stderr_retained_bytes"] = std::to_string(result.stderr_data.size());
    report.metrics["stdout_truncated"] = "true";
    report.metrics["stderr_truncated"] = "true";
    return report;
}

ScenarioResult stress(const Options& options) {
    const std::size_t tasks = std::max<std::size_t>(options.tasks, 300);
    taskforge::ConcurrentExecutor executor(4, tasks + 4);
    std::vector<taskforge::TaskHandle> handles;
    std::vector<std::stop_source> stops;
    handles.reserve(tasks);
    stops.reserve(tasks / 12 + 1);
    const auto started = Clock::now();
    for (std::size_t index = 0; index < tasks; ++index) {
        switch (index % 12) {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
            case 6:
            case 7:
                handles.push_back(accepted(executor.submit(helper({"exit", "0"})), "stress success task"));
                break;
            case 8:
                handles.push_back(accepted(executor.submit(helper({"exit", "7"})), "stress failure task"));
                break;
            case 9:
                handles.push_back(accepted(executor.submit(helper({"sleep", "200"}),
                                                           {.timeout = 10ms, .termination_grace = 10ms}),
                                           "stress timeout task"));
                break;
            case 10:
                handles.push_back(accepted(executor.submit(
                    {.executable = "/definitely/not/a/taskforge-executable", .arguments = {}}),
                                           "stress startup failure task"));
                break;
            case 11:
                stops.emplace_back();
                handles.push_back(accepted(executor.submit(helper({"sleep", "200"}),
                                                           {.stop_token = stops.back().get_token(), .termination_grace = 10ms}),
                                           "stress cancellation task"));
                stops.back().request_stop();
                break;
        }
    }

    std::set<std::uint64_t> unique_ids;
    std::size_t success = 0;
    std::size_t failed = 0;
    std::size_t timed_out = 0;
    std::size_t cancelled = 0;
    for (const auto& handle : handles) {
        unique_ids.insert(handle.id());
        const auto& result = handle.get();
        switch (result.outcome) {
            case taskforge::ProcessOutcome::exited:
                result.exit_code == 0 ? ++success : ++failed;
                break;
            case taskforge::ProcessOutcome::startup_failed:
            case taskforge::ProcessOutcome::parent_error:
            case taskforge::ProcessOutcome::signaled:
                ++failed;
                break;
            case taskforge::ProcessOutcome::timed_out:
                ++timed_out;
                break;
            case taskforge::ProcessOutcome::cancelled:
                ++cancelled;
                break;
        }
    }
    require(unique_ids.size() == handles.size(), "stress accepted task IDs were not unique");
    require(success + failed + timed_out + cancelled == handles.size(), "stress completion accounting mismatch");
    ScenarioResult report{"mixed_stress", elapsed_ms(started), per_second(handles.size(), elapsed_ms(started))};
    report.metrics["submitted"] = std::to_string(tasks);
    report.metrics["accepted"] = std::to_string(handles.size());
    report.metrics["completed"] = std::to_string(handles.size());
    report.metrics["success"] = std::to_string(success);
    report.metrics["failed"] = std::to_string(failed);
    report.metrics["timeout"] = std::to_string(timed_out);
    report.metrics["cancelled"] = std::to_string(cancelled);
    report.metrics["accepted_id_count"] = std::to_string(handles.size());
    report.metrics["unique_id_count"] = std::to_string(unique_ids.size());
    return report;
}

void print_text(const std::vector<ScenarioResult>& results) {
    for (const auto& result : results) {
        std::cout << "scenario=" << result.scenario << " elapsed_ms=" << result.elapsed_ms
                  << " tasks_per_second=" << std::fixed << std::setprecision(2) << result.tasks_per_second << '\n';
        for (const auto& [key, value] : result.metrics) std::cout << "  " << key << '=' << value << '\n';
    }
}

void print_json(const std::vector<ScenarioResult>& results) {
    std::cout << "{\"clock\":\"steady_clock\",\"results\":[";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        if (index != 0) std::cout << ',';
        std::cout << "{\"scenario\":\"" << result.scenario << "\",\"elapsed_ms\":" << result.elapsed_ms
                  << ",\"tasks_per_second\":" << std::fixed << std::setprecision(2) << result.tasks_per_second
                  << ",\"metrics\":{";
        bool first = true;
        for (const auto& [key, value] : result.metrics) {
            if (!first) std::cout << ',';
            first = false;
            std::cout << '"' << key << "\":" << value;
        }
        std::cout << "}}";
    }
    std::cout << "]}\n";
}

void usage(std::ostream& output) {
    output << "Usage: taskforge_bench [--scenario all|launch|scaling|concurrency|resource|memory|backpressure|output|stress] "
              "[--format text|json] [--warmup N] [--iterations N] [--tasks N] [--workers N]\n";
}

std::size_t parse_count(std::string_view value, const char* name) {
    try {
        const auto parsed = std::stoull(std::string(value));
        require(parsed > 0 && parsed <= static_cast<unsigned long long>(SIZE_MAX),
                std::string(name) + " must be a positive integer");
        return static_cast<std::size_t>(parsed);
    } catch (const std::exception&) {
        fail(std::string("invalid ") + name + ": " + std::string(value));
    }
}

Options parse_options(int argc, char* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help") {
            usage(std::cout);
            std::exit(0);
        }
        if (index + 1 == argc) fail("missing value for " + std::string(argument));
        const std::string_view value = argv[++index];
        if (argument == "--scenario") options.scenario = value;
        else if (argument == "--format") options.format = value;
        else if (argument == "--warmup") options.warmup = parse_count(value, "warmup");
        else if (argument == "--iterations") options.iterations = parse_count(value, "iterations");
        else if (argument == "--tasks") options.tasks = parse_count(value, "tasks");
        else if (argument == "--workers") options.workers = parse_count(value, "workers");
        else fail("unknown argument: " + std::string(argument));
    }
    const std::vector<std::string> scenarios{"all", "launch", "scaling", "concurrency", "resource", "memory", "backpressure", "output", "stress"};
    require(std::find(scenarios.begin(), scenarios.end(), options.scenario) != scenarios.end(), "unknown scenario");
    require(options.format == "text" || options.format == "json", "format must be text or json");
    return options;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const auto options = parse_options(argc, argv);
        std::vector<ScenarioResult> results;
        const auto selected = [&options](std::string_view name) { return options.scenario == "all" || options.scenario == name; };
        if (selected("launch")) results.push_back(launch(options));
        if (selected("scaling")) results.push_back(scaling(options));
        if (selected("concurrency")) results.push_back(concurrency(options));
        if (selected("resource")) results.push_back(resource(options));
        if (selected("memory")) results.push_back(memory_admission(options));
        if (selected("backpressure")) results.push_back(backpressure(options));
        if (selected("output")) results.push_back(output_capture(options));
        if (selected("stress")) results.push_back(stress(options));
        if (options.format == "json") print_json(results);
        else print_text(results);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "taskforge_bench: " << error.what() << '\n';
        return 2;
    }
}
