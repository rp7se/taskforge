#pragma once

#include <chrono>
#include <filesystem>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace taskforge {

struct ProcessSpec {
    std::string executable;
    std::vector<std::string> arguments;
};

// cpu.max uses microseconds for both quota and period.
struct CpuMax {
    std::uint64_t quota_us;
    std::uint64_t period_us;
};

struct CgroupV2Limits {
    std::optional<CpuMax> cpu;
    std::optional<std::uint64_t> memory_max_bytes;
    std::optional<std::uint64_t> pids_max;
};

// root is an explicitly delegated cgroup v2 subtree.  TaskForge never uses
// /sys/fs/cgroup implicitly and never changes root's parent.
struct CgroupV2Options {
    std::filesystem::path root;
    CgroupV2Limits limits;
};

enum class ProcessOutcome {
    exited,
    signaled,
    startup_failed,
    parent_error,
    timed_out,
    cancelled,
};

enum class ProcessErrorStage {
    create_stdout_pipe,
    create_stderr_pipe,
    create_startup_pipe,
    create_launch_gate,
    configure_pipe,
    resolve_executable,
    fork,
    child_dup_stdout,
    child_dup_stderr,
    child_process_group_setup,
    child_exec,
    parent_poll,
    parent_read_stdout,
    parent_read_stderr,
    parent_read_startup,
    parent_kill,
    parent_process_group_setup,
    parent_group_signal,
    parent_waitpid,
    startup_protocol,
    cgroup_root_validation,
    cgroup_controller_setup,
    cgroup_create,
    cgroup_limit_write,
    cgroup_attach,
    cgroup_diagnostics,
    cgroup_cleanup,
};

enum class ProcessCleanupOutcome {
    not_needed,
    terminated_during_grace,
    killed_after_grace,
    failed,
};

struct ProcessError {
    ProcessErrorStage stage;
    int system_error;
};

struct CgroupResourceEvents {
    std::optional<std::uint64_t> cpu_nr_throttled;
    std::optional<std::uint64_t> memory_oom_kill;
    std::optional<std::uint64_t> memory_max;
    std::optional<std::uint64_t> pids_max;
};

struct ProcessResult {
    std::string stdout_data;
    std::string stderr_data;
    ProcessOutcome outcome;
    std::optional<int> exit_code;
    std::optional<int> terminating_signal;
    std::optional<ProcessError> error;
    ProcessCleanupOutcome cleanup_outcome = ProcessCleanupOutcome::not_needed;
    std::optional<CgroupResourceEvents> cgroup_events;
    // Diagnostics and directory removal are orthogonal to the process outcome.
    std::optional<ProcessError> cgroup_diagnostic_error;
    std::optional<ProcessError> cgroup_cleanup_error;
};

struct ProcessExecutionOptions {
    std::optional<std::chrono::milliseconds> timeout;
    std::stop_token stop_token;
    std::chrono::milliseconds termination_grace{100};
    std::optional<CgroupV2Options> cgroup;
};

[[nodiscard]] ProcessResult run_process(const ProcessSpec& spec,
                                        const ProcessExecutionOptions& options = {});

}  // namespace taskforge
