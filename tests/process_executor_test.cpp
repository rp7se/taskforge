#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <stop_token>
#include <thread>
#include <vector>

#include "taskforge/process_executor.hpp"

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "test failure: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

taskforge::ProcessResult run_helper(
    std::vector<std::string> arguments,
    const taskforge::ProcessExecutionOptions& options = {}) {
    return taskforge::run_process({.executable = TASKFORGE_PROCESS_TEST_HELPER,
                                   .arguments = std::move(arguments)},
                                  options);
}

taskforge::ProcessExecutionOptions bounded(std::uint64_t stdout_bytes,
                                           std::uint64_t stderr_bytes) {
    return {.output_capture_limits = taskforge::OutputCaptureLimits{
                .stdout_bytes = stdout_bytes, .stderr_bytes = stderr_bytes}};
}

void require_exit(const taskforge::ProcessResult& result, int exit_code);

void require_control_result(const taskforge::ProcessResult& result,
                            taskforge::ProcessOutcome outcome) {
    require(result.outcome == outcome, "unexpected controlled termination outcome");
    require(!result.exit_code.has_value(), "controlled termination has an exit code");
    require(!result.error.has_value(), "controlled termination has an error");
}

std::vector<pid_t> pids_from_output(const std::string& output, const std::string& marker) {
    std::vector<pid_t> pids;
    std::size_t offset = 0;
    while ((offset = output.find(marker, offset)) != std::string::npos) {
        const std::size_t value_start = offset + marker.size();
        const std::size_t value_end = output.find(' ', value_start);
        pids.push_back(static_cast<pid_t>(std::stol(output.substr(value_start, value_end - value_start))));
        offset = value_start;
    }
    return pids;
}

bool process_is_gone(pid_t pid) {
    return ::kill(pid, 0) != 0 && errno == ESRCH;
}

void require_gone(pid_t pid) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (process_is_gone(pid)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(false, "known descendant process remained after cleanup");
}

void test_process_group_cleanup() {
    const auto cooperative = run_helper(
        {"spawn-descendants", "500", "0", "0", "1"},
        {.timeout = std::chrono::milliseconds(30), .stop_token = {}, .termination_grace = std::chrono::milliseconds(80)});
    require_control_result(cooperative, taskforge::ProcessOutcome::timed_out);
    require(cooperative.cleanup_outcome == taskforge::ProcessCleanupOutcome::terminated_during_grace,
            "cooperative process group was not terminated during grace");
    const auto cooperative_descendants = pids_from_output(cooperative.stdout_data, "DESCENDANT_PID=");
    require(cooperative_descendants.size() == 1, "cooperative descendant PID was not captured");
    require_gone(cooperative_descendants.front());

    const auto escalation = run_helper(
        {"spawn-descendants", "500", "0", "1", "1"},
        {.timeout = std::chrono::milliseconds(100), .stop_token = {}, .termination_grace = std::chrono::milliseconds(30)});
    require_control_result(escalation, taskforge::ProcessOutcome::timed_out);
    require(escalation.cleanup_outcome == taskforge::ProcessCleanupOutcome::killed_after_grace,
            "TERM-ignoring descendant was not escalated to SIGKILL: " +
                std::to_string(static_cast<int>(escalation.cleanup_outcome)));
    const auto escalation_descendants = pids_from_output(escalation.stdout_data, "DESCENDANT_PID=");
    require(escalation_descendants.size() == 1, "escalation descendant PID was not captured");
    require_gone(escalation_descendants.front());
}

void test_natural_exit_and_multiple_descendants() {
    const auto natural = run_helper(
        {"spawn-descendants", "500", "1", "0", "1"},
        {.timeout = std::nullopt, .stop_token = {}, .termination_grace = std::chrono::milliseconds(80)});
    require_exit(natural, 0);
    require(natural.cleanup_outcome == taskforge::ProcessCleanupOutcome::terminated_during_grace,
            "natural parent exit did not clean its descendant group");
    require(natural.stderr_data.find("descendant stderr\n") != std::string::npos,
            "descendant output was not captured before cleanup");
    const auto natural_descendants = pids_from_output(natural.stdout_data, "DESCENDANT_PID=");
    require(natural_descendants.size() == 1, "natural-exit descendant PID was not captured");
    require_gone(natural_descendants.front());

    const auto multiple = run_helper(
        {"spawn-descendants", "500", "0", "0", "2"},
        {.timeout = std::chrono::milliseconds(30), .stop_token = {}, .termination_grace = std::chrono::milliseconds(80)});
    require_control_result(multiple, taskforge::ProcessOutcome::timed_out);
    const auto parent = pids_from_output(multiple.stdout_data, "PARENT_PID=");
    const auto descendants = pids_from_output(multiple.stdout_data, "DESCENDANT_PID=");
    require(parent.size() == 1 && descendants.size() == 2, "process-group helper did not report all PIDs");
    require(multiple.stdout_data.find("PARENT_PID=" + std::to_string(parent.front()) + " PGID=" + std::to_string(parent.front())) != std::string::npos,
            "direct child was not a process-group leader");
    for (const pid_t descendant : descendants) {
        require(multiple.stdout_data.find("DESCENDANT_PID=" + std::to_string(descendant) + " PGID=" + std::to_string(parent.front())) != std::string::npos,
                "descendant did not inherit the task process group");
        require_gone(descendant);
    }
}

void test_cancel_cleans_descendant() {
    std::stop_source source;
    std::jthread canceller([&source] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        source.request_stop();
    });
    const auto result = run_helper(
        {"spawn-descendants", "500", "0", "0", "1"},
        {.timeout = std::nullopt, .stop_token = source.get_token(), .termination_grace = std::chrono::milliseconds(80)});
    require_control_result(result, taskforge::ProcessOutcome::cancelled);
    const auto descendants = pids_from_output(result.stdout_data, "DESCENDANT_PID=");
    require(descendants.size() == 1, "cancelled descendant PID was not captured");
    require_gone(descendants.front());
}

void test_timeout() {
    const auto result = run_helper({"sleep", "500"},
                                   {.timeout = std::chrono::milliseconds(30), .stop_token = {}});
    require_control_result(result, taskforge::ProcessOutcome::timed_out);
}

void test_manual_cancellation() {
    std::stop_source source;
    std::jthread canceller([&source] {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        source.request_stop();
    });
    const auto started = std::chrono::steady_clock::now();
    const auto result = run_helper({"sleep", "500"},
                                   {.timeout = std::nullopt, .stop_token = source.get_token()});
    require_control_result(result, taskforge::ProcessOutcome::cancelled);
    require(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(300),
            "cancellation did not return promptly");
}

void test_prestart_controls() {
    std::stop_source source;
    source.request_stop();
    const auto cancelled = taskforge::run_process(
        {.executable = "/definitely/not/a/taskforge-executable", .arguments = {}},
        {.timeout = std::nullopt, .stop_token = source.get_token()});
    require(cancelled.outcome == taskforge::ProcessOutcome::cancelled,
            "already-cancelled execution resolved or started a child");

    const auto timeout = taskforge::run_process(
        {.executable = "/definitely/not/a/taskforge-executable", .arguments = {}},
        {.timeout = std::chrono::milliseconds::zero(), .stop_token = {}});
    require(timeout.outcome == taskforge::ProcessOutcome::timed_out,
            "immediate timeout resolved or started a child");
}

void test_control_races() {
    for (int iteration = 0; iteration < 32; ++iteration) {
        std::stop_source source;
        std::jthread canceller([&source] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            source.request_stop();
        });
        const auto result = run_helper({"exit-after", "20", "0"},
                                       {.timeout = std::nullopt, .stop_token = source.get_token()});
        require(result.outcome == taskforge::ProcessOutcome::exited ||
                    result.outcome == taskforge::ProcessOutcome::cancelled,
                "cancel versus natural-exit race had an invalid outcome");
    }

    for (int iteration = 0; iteration < 32; ++iteration) {
        const auto result = run_helper({"exit-after", "20", "0"},
                                       {.timeout = std::chrono::milliseconds(20), .stop_token = {}});
        require(result.outcome == taskforge::ProcessOutcome::exited ||
                    result.outcome == taskforge::ProcessOutcome::timed_out,
                "timeout versus natural-exit race had an invalid outcome");
    }
}

void test_output_before_timeout() {
    const auto result = run_helper({"print-then-sleep", "500"},
                                   {.timeout = std::chrono::milliseconds(30), .stop_token = {}});
    require_control_result(result, taskforge::ProcessOutcome::timed_out);
    require(result.stdout_data == "before timeout stdout\n", "stdout before timeout was lost");
    require(result.stderr_data == "before timeout stderr\n", "stderr before timeout was lost");
}

void test_bounded_capture_boundaries_and_zero() {
    {
        const auto result = run_helper({"write-bytes", "stdout", "7"}, bounded(16, 16));
        require_exit(result, 0);
        require(result.stdout_data.size() == 7 && result.stdout_total_bytes == 7 &&
                    !result.stdout_truncated,
                "small stdout under limit was not retained exactly");
    }
    {
        const auto result = run_helper({"write-bytes", "stderr", "7"}, bounded(16, 16));
        require_exit(result, 0);
        require(result.stderr_data.size() == 7 && result.stderr_total_bytes == 7 &&
                    !result.stderr_truncated,
                "small stderr under limit was not retained exactly");
    }
    {
        const auto exact_stdout = run_helper({"write-bytes", "stdout", "13"}, bounded(13, 1));
        const auto plus_stdout = run_helper({"write-bytes", "stdout", "14"}, bounded(13, 1));
        const auto exact_stderr = run_helper({"write-bytes", "stderr", "9"}, bounded(1, 9));
        const auto plus_stderr = run_helper({"write-bytes", "stderr", "10"}, bounded(1, 9));
        require(exact_stdout.stdout_data.size() == 13 && exact_stdout.stdout_total_bytes == 13 &&
                    !exact_stdout.stdout_truncated,
                "exact stdout boundary was marked truncated");
        require(plus_stdout.stdout_data.size() == 13 && plus_stdout.stdout_total_bytes == 14 &&
                    plus_stdout.stdout_truncated,
                "stdout limit-plus-one did not retain prefix and diagnose truncation");
        require(exact_stderr.stderr_data.size() == 9 && exact_stderr.stderr_total_bytes == 9 &&
                    !exact_stderr.stderr_truncated,
                "exact stderr boundary was marked truncated");
        require(plus_stderr.stderr_data.size() == 9 && plus_stderr.stderr_total_bytes == 10 &&
                    plus_stderr.stderr_truncated,
                "stderr limit-plus-one did not retain prefix and diagnose truncation");
    }
    {
        const auto result = run_helper({"dual-write", "31", "29"}, bounded(0, 0));
        require_exit(result, 0);
        require(result.stdout_data.empty() && result.stderr_data.empty() &&
                    result.stdout_total_bytes == 31 && result.stderr_total_bytes == 29 &&
                    result.stdout_truncated && result.stderr_truncated,
                "zero capture limits did not drain and account for both streams");
    }
}

void test_independent_large_and_binary_capture() {
    constexpr std::uint64_t mib = 1024U * 1024U;
    {
        const auto result = run_helper({"dual-write", "200", "200"}, bounded(64, 17));
        require_exit(result, 0);
        require(result.stdout_data.size() == 64 && result.stderr_data.size() == 17 &&
                    result.stdout_total_bytes == 200 && result.stderr_total_bytes == 200 &&
                    result.stdout_truncated && result.stderr_truncated,
                "stdout and stderr limits were not independent");
    }
    {
        const auto result = run_helper({"write-bytes", "stdout", "8388608"}, bounded(64 * 1024, 64 * 1024));
        require_exit(result, 0);
        require(result.stdout_data.size() == 64 * 1024 && result.stdout_total_bytes == 8 * mib &&
                    result.stdout_truncated,
                "large stdout was not fully drained with bounded prefix capture");
    }
    {
        const auto result = run_helper({"write-bytes", "stderr", "8388608"}, bounded(64 * 1024, 64 * 1024));
        require_exit(result, 0);
        require(result.stderr_data.size() == 64 * 1024 && result.stderr_total_bytes == 8 * mib &&
                    result.stderr_truncated,
                "large stderr was not fully drained with bounded prefix capture");
    }
    {
        const auto result = run_helper({"dual-write", "8388608", "8388608"}, bounded(32 * 1024, 16 * 1024));
        require_exit(result, 0);
        require(result.stdout_data.size() == 32 * 1024 && result.stderr_data.size() == 16 * 1024 &&
                    result.stdout_total_bytes == 8 * mib && result.stderr_total_bytes == 8 * mib &&
                    result.stdout_truncated && result.stderr_truncated,
                "large dual-stream output deadlocked or exceeded bounded capture");
    }
    {
        const auto result = run_helper({"binary-stdout", "17"}, bounded(10, 1));
        require_exit(result, 0);
        require(result.stdout_data.size() == 10 && result.stdout_total_bytes == 17 &&
                    result.stdout_truncated && result.stdout_data[1] == '\0' &&
                    result.stdout_data[5] == '\0',
                "binary NUL output was not captured as bytes");
    }
}

void test_timeout_and_cancellation_with_bounded_output() {
    {
        auto options = bounded(1024, 1024);
        options.timeout = std::chrono::milliseconds(100);
        options.termination_grace = std::chrono::milliseconds(30);
        const auto result = run_helper({"write-then-sleep", "262144", "262144", "500"}, options);
        require_control_result(result, taskforge::ProcessOutcome::timed_out);
        require(result.stdout_data.size() <= 1024 && result.stderr_data.size() <= 1024 &&
                    result.stdout_total_bytes >= result.stdout_data.size() &&
                    result.stderr_total_bytes >= result.stderr_data.size() &&
                    result.stdout_truncated && result.stderr_truncated,
                "timeout did not preserve bounded truncated capture");
    }
    {
        std::stop_source source;
        std::jthread canceller([&source] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            source.request_stop();
        });
        auto options = bounded(1024, 1024);
        options.stop_token = source.get_token();
        options.termination_grace = std::chrono::milliseconds(30);
        const auto result = run_helper({"write-then-sleep", "262144", "262144", "500"}, options);
        require_control_result(result, taskforge::ProcessOutcome::cancelled);
        require(result.stdout_data.size() <= 1024 && result.stderr_data.size() <= 1024 &&
                    result.stdout_total_bytes >= result.stdout_data.size() &&
                    result.stderr_total_bytes >= result.stderr_data.size() &&
                    result.stdout_truncated && result.stderr_truncated,
                "cancellation did not preserve bounded truncated capture");
    }
}

void require_exit(const taskforge::ProcessResult& result, int exit_code) {
    require(result.outcome == taskforge::ProcessOutcome::exited, "expected normal exit");
    require(result.exit_code == exit_code, "unexpected exit code");
    require(!result.terminating_signal.has_value(), "normal exit has a signal");
    require(!result.error.has_value(), "normal exit has an error");
}

}  // namespace

int main() {
    const auto stdout_result = run_helper({"stdout"});
    require_exit(stdout_result, 0);
    require(stdout_result.stdout_data == "hello stdout\n", "stdout was not captured");
    require(stdout_result.stderr_data.empty(), "stdout case wrote stderr");

    const auto stderr_result = run_helper({"stderr"});
    require_exit(stderr_result, 0);
    require(stderr_result.stderr_data == "hello stderr\n", "stderr was not captured");
    require(stderr_result.stdout_data.empty(), "stderr case wrote stdout");

    const auto argv_result = run_helper({"argv", "first", "two words", "--literal"});
    require_exit(argv_result, 0);
    require(argv_result.stdout_data == "first|two words|--literal\n", "argv was not preserved");

    const auto nonzero_result = run_helper({"exit", "7"});
    require_exit(nonzero_result, 7);

    const auto path_result = taskforge::run_process({.executable = "true", .arguments = {}});
    require_exit(path_result, 0);

    const auto signaled_result = run_helper({"signal"});
    require(signaled_result.outcome == taskforge::ProcessOutcome::signaled,
            "signal termination was not reported");
    require(signaled_result.terminating_signal == SIGTERM, "wrong terminating signal");
    require(!signaled_result.exit_code.has_value(), "signal termination has an exit code");

    const auto missing_result = taskforge::run_process(
        {.executable = "/definitely/not/a/taskforge-executable", .arguments = {}});
    require(missing_result.outcome == taskforge::ProcessOutcome::startup_failed,
            "missing executable was not a startup failure");
    require(missing_result.error.has_value(), "missing executable has no error");
    require(missing_result.error->stage == taskforge::ProcessErrorStage::child_exec,
            "missing executable failed at the wrong stage");
    require(missing_result.error->system_error == ENOENT, "missing executable has wrong errno");

    const auto bounded_missing_result = taskforge::run_process(
        {.executable = "/definitely/not/a/taskforge-executable", .arguments = {}}, bounded(0, 0));
    require(bounded_missing_result.outcome == taskforge::ProcessOutcome::startup_failed &&
                bounded_missing_result.error.has_value() &&
                bounded_missing_result.error->stage == taskforge::ProcessErrorStage::child_exec,
            "zero output limits altered startup failure diagnostics");

    const auto exit_127_result = run_helper({"exit", "127"});
    require_exit(exit_127_result, 127);
    const auto bounded_exit_127_result = run_helper({"exit", "127"}, bounded(0, 0));
    require_exit(bounded_exit_127_result, 127);

    const auto both_result = run_helper({"both"});
    require_exit(both_result, 0);
    require(both_result.stdout_data.size() == 128U * 1024U, "stdout drain was incomplete");
    require(both_result.stderr_data.size() == 128U * 1024U, "stderr drain was incomplete");
    require(both_result.stdout_total_bytes == 128U * 1024U &&
                both_result.stderr_total_bytes == 128U * 1024U &&
                !both_result.stdout_truncated && !both_result.stderr_truncated,
            "legacy unlimited capture accounting changed");

    test_timeout();
    test_manual_cancellation();
    test_prestart_controls();
    test_control_races();
    test_output_before_timeout();
    test_process_group_cleanup();
    test_natural_exit_and_multiple_descendants();
    test_cancel_cleans_descendant();
    test_bounded_capture_boundaries_and_zero();
    test_independent_large_and_binary_capture();
    test_timeout_and_cancellation_with_bounded_output();

    return 0;
}
