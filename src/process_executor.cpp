#include "taskforge/process_executor.hpp"
#include "taskforge/cgroup_v2.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace taskforge {
namespace {

constexpr int kControlPollMilliseconds = 20;
constexpr auto kKillCleanupWindow = std::chrono::milliseconds(1000);

class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_;
};

struct Pipe { UniqueFd read_end; UniqueFd write_end; };
struct StartupMessage { std::uint8_t stage; std::int32_t error_number; };

enum class ControlDecision { none, timed_out, cancelled };
enum class CleanupPhase { inactive, grace, kill, complete, failed };

[[nodiscard]] ProcessResult parent_failure(ProcessErrorStage stage, int error_number) {
    ProcessResult result{};
    result.outcome = ProcessOutcome::parent_error;
    result.error = ProcessError{stage, error_number};
    return result;
}

[[nodiscard]] bool make_pipe(Pipe& pipe, ProcessErrorStage stage, ProcessResult& failure) {
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        failure = parent_failure(stage, errno);
        return false;
    }
    pipe.read_end.reset(fds[0]);
    pipe.write_end.reset(fds[1]);
    return true;
}

[[nodiscard]] bool set_nonblocking(const UniqueFd& fd, ProcessResult& failure) {
    const int flags = ::fcntl(fd.get(), F_GETFL);
    if (flags < 0 || ::fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK) != 0) {
        failure = parent_failure(ProcessErrorStage::configure_pipe, errno);
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<std::string> resolve_executable(const ProcessSpec& spec,
                                                            ProcessResult& failure) {
    if (spec.executable.empty()) {
        failure = parent_failure(ProcessErrorStage::resolve_executable, ENOENT);
        return std::nullopt;
    }
    if (spec.executable.find('/') != std::string::npos) {
        return spec.executable;
    }
    const char* path_environment = std::getenv("PATH");
    if (path_environment == nullptr) {
        failure = parent_failure(ProcessErrorStage::resolve_executable, ENOENT);
        return std::nullopt;
    }
    const std::string path(path_environment);
    int resolution_error = ENOENT;
    std::size_t component_start = 0;
    while (true) {
        const std::size_t component_end = path.find(':', component_start);
        const std::string_view component(path.data() + component_start,
                                         (component_end == std::string::npos ? path.size() : component_end) - component_start);
        std::string candidate = component.empty() ? "." : std::string(component);
        candidate += '/';
        candidate += spec.executable;
        if (::access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
        if (errno == EACCES) {
            resolution_error = EACCES;
        }
        if (component_end == std::string::npos) {
            break;
        }
        component_start = component_end + 1;
    }
    failure = parent_failure(ProcessErrorStage::resolve_executable, resolution_error);
    return std::nullopt;
}

[[noreturn]] void child_failure(int startup_write_fd, ProcessErrorStage stage) {
    const StartupMessage message{static_cast<std::uint8_t>(stage), errno};
    const auto* bytes = reinterpret_cast<const char*>(&message);
    std::size_t written = 0;
    while (written < sizeof(message)) {
        const ssize_t result = ::write(startup_write_fd, bytes + written, sizeof(message) - written);
        if (result > 0) { written += static_cast<std::size_t>(result); continue; }
        if (result < 0 && errno == EINTR) { continue; }
        break;
    }
    _exit(127);
}

void redirect_child_output(int source_fd, int destination_fd, ProcessErrorStage stage,
                           int startup_write_fd) {
    if (source_fd == destination_fd) {
        const int descriptor_flags = ::fcntl(source_fd, F_GETFD);
        if (descriptor_flags < 0 ||
            ::fcntl(source_fd, F_SETFD, descriptor_flags & ~FD_CLOEXEC) != 0) {
            child_failure(startup_write_fd, stage);
        }
        return;
    }
    if (::dup2(source_fd, destination_fd) < 0) {
        child_failure(startup_write_fd, stage);
    }
    (void)::close(source_fd);
}

void wait_for_launch_gate(int launch_read_fd, int startup_write_fd) {
    char token = 0;
    while (true) {
        const ssize_t count = ::read(launch_read_fd, &token, 1);
        if (count == 1 && token == 'G') {
            (void)::close(launch_read_fd);
            return;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        child_failure(startup_write_fd, ProcessErrorStage::cgroup_attach);
    }
}

[[nodiscard]] bool release_launch_gate(UniqueFd& launch_write, ProcessResult& failure) {
    char token = 'G';
    while (true) {
        const ssize_t count = ::write(launch_write.get(), &token, 1);
        if (count == 1) {
            launch_write.reset();
            return true;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        failure = parent_failure(ProcessErrorStage::cgroup_attach, count < 0 ? errno : EIO);
        launch_write.reset();
        return false;
    }
}

void append_captured_bytes(std::string& output, std::uint64_t& total_bytes, bool& truncated,
                           std::optional<std::uint64_t> limit, const char* bytes,
                           std::size_t count) {
    const auto count_u64 = static_cast<std::uint64_t>(count);
    if (std::numeric_limits<std::uint64_t>::max() - total_bytes < count_u64) {
        total_bytes = std::numeric_limits<std::uint64_t>::max();
    } else {
        total_bytes += count_u64;
    }

    if (!limit.has_value()) {
        output.append(bytes, count);
        return;
    }

    const auto captured = static_cast<std::uint64_t>(output.size());
    std::size_t append_count = 0;
    if (captured < *limit) {
        const auto remaining = *limit - captured;
        append_count = static_cast<std::size_t>(std::min<std::uint64_t>(count_u64, remaining));
        output.append(bytes, append_count);
    }
    if (append_count < count) {
        truncated = true;
    }
}

[[nodiscard]] bool drain_fd(UniqueFd& fd, std::string& output, std::uint64_t& total_bytes,
                            bool& truncated, std::optional<std::uint64_t> capture_limit,
                            ProcessErrorStage stage, ProcessResult& failure) {
    std::array<char, 8192> buffer{};
    while (true) {
        const ssize_t count = ::read(fd.get(), buffer.data(), buffer.size());
        if (count > 0) {
            append_captured_bytes(output, total_bytes, truncated, capture_limit, buffer.data(),
                                  static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) { fd.reset(); return true; }
        if (errno == EINTR) { continue; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) { return true; }
        failure = parent_failure(stage, errno);
        return false;
    }
}

[[nodiscard]] bool observe_child(pid_t child, int& status, bool& reaped, ProcessResult& failure) {
    while (true) {
        const pid_t observed = ::waitpid(child, &status, WNOHANG);
        if (observed == 0) { return true; }
        if (observed == child) { reaped = true; return true; }
        if (errno == EINTR) { continue; }
        failure = parent_failure(ProcessErrorStage::parent_waitpid, errno);
        return false;
    }
}

[[nodiscard]] bool wait_for_child(pid_t child, int& status, ProcessResult& failure) {
    while (::waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) { continue; }
        failure = parent_failure(ProcessErrorStage::parent_waitpid, errno);
        return false;
    }
    return true;
}

[[nodiscard]] bool group_exists(bool process_group_confirmed, pid_t pgid,
                                ProcessResult& failure) {
    if (!process_group_confirmed || pgid <= 1 || pgid == ::getpgrp()) {
        failure = parent_failure(ProcessErrorStage::parent_group_signal, EINVAL);
        return false;
    }
    if (::kill(-pgid, 0) == 0) { return true; }
    if (errno == ESRCH) { return false; }
    failure = parent_failure(ProcessErrorStage::parent_group_signal, errno);
    return false;
}

[[nodiscard]] bool signal_group(bool process_group_confirmed, pid_t pgid, int signal,
                                ProcessResult& failure) {
    if (!process_group_confirmed || pgid <= 1 || pgid == ::getpgrp()) {
        failure = parent_failure(ProcessErrorStage::parent_group_signal, EINVAL);
        return false;
    }
    if (::kill(-pgid, signal) == 0 || errno == ESRCH) { return true; }
    failure = parent_failure(ProcessErrorStage::parent_group_signal, errno);
    return false;
}

void drain_available(UniqueFd& stdout_fd, UniqueFd& stderr_fd, UniqueFd& startup_fd,
                     ProcessResult& result, std::string& startup_bytes,
                     const ProcessExecutionOptions& options) {
    ProcessResult ignored{};
    const auto stdout_limit = options.output_capture_limits
                                  ? std::optional(options.output_capture_limits->stdout_bytes)
                                  : std::nullopt;
    const auto stderr_limit = options.output_capture_limits
                                  ? std::optional(options.output_capture_limits->stderr_bytes)
                                  : std::nullopt;
    std::uint64_t ignored_total = 0;
    bool ignored_truncated = false;
    (void)drain_fd(stdout_fd, result.stdout_data, result.stdout_total_bytes, result.stdout_truncated,
                   stdout_limit, ProcessErrorStage::parent_read_stdout, ignored);
    (void)drain_fd(stderr_fd, result.stderr_data, result.stderr_total_bytes, result.stderr_truncated,
                   stderr_limit, ProcessErrorStage::parent_read_stderr, ignored);
    (void)drain_fd(startup_fd, startup_bytes, ignored_total, ignored_truncated, std::nullopt,
                   ProcessErrorStage::parent_read_startup, ignored);
}

}  // namespace

ProcessResult run_process(const ProcessSpec& spec, const ProcessExecutionOptions& options) {
    ProcessResult result{};
    if (options.stop_token.stop_requested()) { result.outcome = ProcessOutcome::cancelled; return result; }
    if (options.timeout && *options.timeout <= std::chrono::milliseconds::zero()) { result.outcome = ProcessOutcome::timed_out; return result; }
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = options.timeout ? std::optional(started + *options.timeout)
                                          : std::optional<std::chrono::steady_clock::time_point>{};
    const auto executable = resolve_executable(spec, result);
    if (!executable) { return result; }
    std::vector<char*> argv;
    argv.reserve(spec.arguments.size() + 2);
    argv.push_back(const_cast<char*>(spec.executable.c_str()));
    for (const std::string& argument : spec.arguments) { argv.push_back(const_cast<char*>(argument.c_str())); }
    argv.push_back(nullptr);
    if (options.stop_token.stop_requested()) { result.outcome = ProcessOutcome::cancelled; return result; }
    if (deadline && std::chrono::steady_clock::now() >= *deadline) { result.outcome = ProcessOutcome::timed_out; return result; }

    std::optional<CgroupV2Task> cgroup_task;
    bool cgroup_attached = false;
    const auto finish = [&](ProcessResult final) {
        if (cgroup_task) {
            if (cgroup_attached) {
                cgroup_task->collect_events(final);
            }
            cgroup_task->cleanup(final);
        }
        return final;
    };
    if (options.cgroup) {
        cgroup_task = CgroupV2Task::create(*options.cgroup, result);
        if (!cgroup_task) {
            return result;
        }
    }
    if (options.stop_token.stop_requested()) { result.outcome = ProcessOutcome::cancelled; return finish(result); }
    if (deadline && std::chrono::steady_clock::now() >= *deadline) { result.outcome = ProcessOutcome::timed_out; return finish(result); }

    Pipe stdout_pipe, stderr_pipe, startup_pipe, launch_gate;
    if (!make_pipe(stdout_pipe, ProcessErrorStage::create_stdout_pipe, result) ||
        !make_pipe(stderr_pipe, ProcessErrorStage::create_stderr_pipe, result) ||
        !make_pipe(startup_pipe, ProcessErrorStage::create_startup_pipe, result) ||
        (cgroup_task && !make_pipe(launch_gate, ProcessErrorStage::create_launch_gate, result)) ||
        !set_nonblocking(stdout_pipe.read_end, result) || !set_nonblocking(stderr_pipe.read_end, result) ||
        !set_nonblocking(startup_pipe.read_end, result)) { return finish(result); }
    const int stdout_read = stdout_pipe.read_end.get(), stdout_write = stdout_pipe.write_end.get();
    const int stderr_read = stderr_pipe.read_end.get(), stderr_write = stderr_pipe.write_end.get();
    const int startup_read = startup_pipe.read_end.get(), startup_write = startup_pipe.write_end.get();
    const int launch_read = launch_gate.read_end.get(), launch_write = launch_gate.write_end.get();
    const pid_t child = ::fork();
    if (child < 0) { return finish(parent_failure(ProcessErrorStage::fork, errno)); }
    if (child == 0) {
        (void)::close(stdout_read); (void)::close(stderr_read); (void)::close(startup_read);
        if (cgroup_task) (void)::close(launch_write);
        if (::setpgid(0, 0) != 0) { child_failure(startup_write, ProcessErrorStage::child_process_group_setup); }
        if (cgroup_task) wait_for_launch_gate(launch_read, startup_write);
        redirect_child_output(stdout_write, STDOUT_FILENO, ProcessErrorStage::child_dup_stdout, startup_write);
        redirect_child_output(stderr_write, STDERR_FILENO, ProcessErrorStage::child_dup_stderr, startup_write);
        ::execv(executable->c_str(), argv.data());
        child_failure(startup_write, ProcessErrorStage::child_exec);
    }
    if (cgroup_task) launch_gate.read_end.reset();
    const pid_t task_pgid = child;
    bool process_group_confirmed = false;
    if (::setpgid(child, child) != 0) {
        const int setup_error = errno;
        pid_t observed_pgid;
        do {
            observed_pgid = ::getpgid(child);
        } while (observed_pgid == -1 && errno == EINTR);
        if (observed_pgid == child) {
            process_group_confirmed = true;
        } else {
            int ignored_status = 0;
            pid_t observed_child;
            do {
                observed_child = ::waitpid(child, &ignored_status, WNOHANG);
            } while (observed_child == -1 && errno == EINTR);
            if (observed_child == 0) {
                launch_gate.write_end.reset();
                (void)::kill(child, SIGKILL);
                (void)wait_for_child(child, ignored_status, result);
            }
            return finish(parent_failure(ProcessErrorStage::parent_process_group_setup, setup_error));
        }
    } else {
        process_group_confirmed = true;
    }
    if (cgroup_task) {
        if (!cgroup_task->attach(child, result)) {
            const ProcessResult attach_failure = result;
            launch_gate.write_end.reset();
            (void)::kill(child, SIGKILL);
            int ignored_status = 0;
            (void)wait_for_child(child, ignored_status, result);
            return finish(attach_failure);
        }
        cgroup_attached = true;
        if (!release_launch_gate(launch_gate.write_end, result)) {
            const ProcessResult gate_failure = result;
            (void)::kill(child, SIGKILL);
            int ignored_status = 0;
            (void)wait_for_child(child, ignored_status, result);
            return finish(gate_failure);
        }
    }
    stdout_pipe.write_end.reset(); stderr_pipe.write_end.reset(); startup_pipe.write_end.reset();

    std::string startup_bytes;
    const auto stdout_limit = options.output_capture_limits
                                  ? std::optional(options.output_capture_limits->stdout_bytes)
                                  : std::nullopt;
    const auto stderr_limit = options.output_capture_limits
                                  ? std::optional(options.output_capture_limits->stderr_bytes)
                                  : std::nullopt;
    std::uint64_t startup_total_bytes = 0;
    bool startup_truncated = false;
    bool reaped = false, io_ok = true;
    int wait_status = 0;
    ControlDecision control = ControlDecision::none;
    CleanupPhase cleanup = CleanupPhase::inactive;
    auto grace_deadline = started;
    auto kill_deadline = started;
    bool close_after_drain = false;

    while (!reaped || stdout_pipe.read_end.valid() || stderr_pipe.read_end.valid() || startup_pipe.read_end.valid() ||
           cleanup == CleanupPhase::grace || cleanup == CleanupPhase::kill) {
        if (!reaped && !observe_child(child, wait_status, reaped, result)) { io_ok = false; break; }

        if (cleanup == CleanupPhase::inactive) {
            if (!reaped && options.stop_token.stop_requested()) { control = ControlDecision::cancelled; }
            else if (!reaped && deadline && std::chrono::steady_clock::now() >= *deadline) { control = ControlDecision::timed_out; }
            ProcessResult group_probe{};
            const bool group_live = group_exists(process_group_confirmed, task_pgid, group_probe);
            if (group_probe.error) { result = group_probe; io_ok = false; break; }
            if (control != ControlDecision::none || (reaped && group_live)) {
                if (group_live && !signal_group(process_group_confirmed, task_pgid, SIGTERM, result)) { io_ok = false; break; }
                if (group_live) {
                    cleanup = CleanupPhase::grace;
                    grace_deadline = std::chrono::steady_clock::now() + std::max(options.termination_grace, std::chrono::milliseconds::zero());
                } else if (reaped) {
                    cleanup = CleanupPhase::complete;
                }
            }
        } else if (cleanup == CleanupPhase::grace) {
            ProcessResult group_probe{};
            const bool group_live = group_exists(process_group_confirmed, task_pgid, group_probe);
            if (group_probe.error) { result = group_probe; io_ok = false; break; }
            if (!group_live) { cleanup = CleanupPhase::complete; result.cleanup_outcome = ProcessCleanupOutcome::terminated_during_grace; close_after_drain = true; }
            else if (std::chrono::steady_clock::now() >= grace_deadline) {
                if (!signal_group(process_group_confirmed, task_pgid, SIGKILL, result)) { io_ok = false; break; }
                cleanup = CleanupPhase::kill;
                kill_deadline = std::chrono::steady_clock::now() + kKillCleanupWindow;
            }
        } else if (cleanup == CleanupPhase::kill) {
            ProcessResult group_probe{};
            const bool group_live = group_exists(process_group_confirmed, task_pgid, group_probe);
            if (group_probe.error) { result = group_probe; io_ok = false; break; }
            if (!group_live) { cleanup = CleanupPhase::complete; result.cleanup_outcome = ProcessCleanupOutcome::killed_after_grace; close_after_drain = true; }
            else if (std::chrono::steady_clock::now() >= kill_deadline) { cleanup = CleanupPhase::failed; result.cleanup_outcome = ProcessCleanupOutcome::failed; close_after_drain = true; }
        }

        std::array<pollfd, 3> fds{}; std::array<int, 3> kinds{}; nfds_t count = 0;
        const auto add = [&](const UniqueFd& fd, int kind) { if (fd.valid()) { fds[count] = {.fd = fd.get(), .events = POLLIN, .revents = 0}; kinds[count++] = kind; } };
        add(stdout_pipe.read_end, 0); add(stderr_pipe.read_end, 1); add(startup_pipe.read_end, 2);
        int wait_ms = kControlPollMilliseconds;
        const auto now = std::chrono::steady_clock::now();
        const auto active_deadline = cleanup == CleanupPhase::grace ? grace_deadline : cleanup == CleanupPhase::kill ? kill_deadline : deadline.value_or(now + std::chrono::milliseconds(wait_ms));
        if (active_deadline > now) wait_ms = static_cast<int>(std::min<std::int64_t>(wait_ms, std::chrono::duration_cast<std::chrono::milliseconds>(active_deadline - now).count()));
        int poll_result;
        do { poll_result = ::poll(fds.data(), count, std::max(0, wait_ms)); } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0) { result = parent_failure(ProcessErrorStage::parent_poll, errno); io_ok = false; break; }
        for (nfds_t index = 0; index < count; ++index) {
            if (fds[index].revents == 0) continue;
            bool drained = kinds[index] == 0 ? drain_fd(stdout_pipe.read_end, result.stdout_data,
                                                         result.stdout_total_bytes, result.stdout_truncated,
                                                         stdout_limit, ProcessErrorStage::parent_read_stdout, result)
                         : kinds[index] == 1 ? drain_fd(stderr_pipe.read_end, result.stderr_data,
                                                         result.stderr_total_bytes, result.stderr_truncated,
                                                         stderr_limit, ProcessErrorStage::parent_read_stderr, result)
                                           : drain_fd(startup_pipe.read_end, startup_bytes, startup_total_bytes,
                                                      startup_truncated, std::nullopt,
                                                      ProcessErrorStage::parent_read_startup, result);
            if (!drained) { io_ok = false; break; }
        }
        if (!io_ok) break;
        if (close_after_drain && reaped) {
            drain_available(stdout_pipe.read_end, stderr_pipe.read_end, startup_pipe.read_end, result,
                            startup_bytes, options);
            stdout_pipe.read_end.reset(); stderr_pipe.read_end.reset(); startup_pipe.read_end.reset();
        }
    }

    if (!reaped) {
        if (!wait_for_child(child, wait_status, result)) return finish(result);
        reaped = true;
    }
    if (!io_ok) return finish(result);
    if (control == ControlDecision::timed_out) { result.outcome = ProcessOutcome::timed_out; if (WIFSIGNALED(wait_status)) result.terminating_signal = WTERMSIG(wait_status); return finish(result); }
    if (control == ControlDecision::cancelled) { result.outcome = ProcessOutcome::cancelled; if (WIFSIGNALED(wait_status)) result.terminating_signal = WTERMSIG(wait_status); return finish(result); }
    if (!startup_bytes.empty()) {
        if (startup_bytes.size() != sizeof(StartupMessage)) return finish(parent_failure(ProcessErrorStage::startup_protocol, EPROTO));
        StartupMessage message{}; std::memcpy(&message, startup_bytes.data(), sizeof(message));
        result.outcome = ProcessOutcome::startup_failed;
        result.error = ProcessError{static_cast<ProcessErrorStage>(message.stage), message.error_number};
        return finish(result);
    }
    if (WIFEXITED(wait_status)) { result.outcome = ProcessOutcome::exited; result.exit_code = WEXITSTATUS(wait_status); return finish(result); }
    if (WIFSIGNALED(wait_status)) { result.outcome = ProcessOutcome::signaled; result.terminating_signal = WTERMSIG(wait_status); return finish(result); }
    return finish(parent_failure(ProcessErrorStage::parent_waitpid, ECHILD));
}

}  // namespace taskforge
