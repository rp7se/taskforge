#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
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

taskforge::ProcessResult run_helper(std::vector<std::string> arguments) {
    return taskforge::run_process({.executable = TASKFORGE_PROCESS_TEST_HELPER,
                                   .arguments = std::move(arguments)});
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

    const auto exit_127_result = run_helper({"exit", "127"});
    require_exit(exit_127_result, 127);

    const auto both_result = run_helper({"both"});
    require_exit(both_result, 0);
    require(both_result.stdout_data.size() == 128U * 1024U, "stdout drain was incomplete");
    require(both_result.stderr_data.size() == 128U * 1024U, "stderr drain was incomplete");

    return 0;
}
