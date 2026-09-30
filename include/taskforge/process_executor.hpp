#pragma once

#include <optional>
#include <string>
#include <vector>

namespace taskforge {

struct ProcessSpec {
    std::string executable;
    std::vector<std::string> arguments;
};

enum class ProcessOutcome {
    exited,
    signaled,
    startup_failed,
    parent_error,
};

enum class ProcessErrorStage {
    create_stdout_pipe,
    create_stderr_pipe,
    create_startup_pipe,
    configure_pipe,
    resolve_executable,
    fork,
    child_dup_stdout,
    child_dup_stderr,
    child_exec,
    parent_poll,
    parent_read_stdout,
    parent_read_stderr,
    parent_read_startup,
    parent_waitpid,
    startup_protocol,
};

struct ProcessError {
    ProcessErrorStage stage;
    int system_error;
};

struct ProcessResult {
    std::string stdout_data;
    std::string stderr_data;
    ProcessOutcome outcome;
    std::optional<int> exit_code;
    std::optional<int> terminating_signal;
    std::optional<ProcessError> error;
};

[[nodiscard]] ProcessResult run_process(const ProcessSpec& spec);

}  // namespace taskforge
