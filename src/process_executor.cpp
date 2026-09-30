#include "taskforge/process_executor.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace taskforge {
namespace {

class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    int release() noexcept {
        const int result = fd_;
        fd_ = -1;
        return result;
    }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_;
};

struct Pipe {
    UniqueFd read_end;
    UniqueFd write_end;
};

struct StartupMessage {
    std::uint8_t stage;
    std::int32_t error_number;
};

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
                                         (component_end == std::string::npos ? path.size()
                                                                             : component_end) -
                                             component_start);
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
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
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

[[nodiscard]] bool drain_fd(UniqueFd& fd, std::string& output, ProcessErrorStage stage,
                            ProcessResult& failure) {
    std::array<char, 8192> buffer{};
    while (true) {
        const ssize_t count = ::read(fd.get(), buffer.data(), buffer.size());
        if (count > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            fd.reset();
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        failure = parent_failure(stage, errno);
        return false;
    }
}

[[nodiscard]] bool wait_for_child(pid_t child, int& status, ProcessResult& failure) {
    while (::waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        failure = parent_failure(ProcessErrorStage::parent_waitpid, errno);
        return false;
    }
    return true;
}

}  // namespace

ProcessResult run_process(const ProcessSpec& spec) {
    ProcessResult result{};
    const std::optional<std::string> resolved_executable = resolve_executable(spec, result);
    if (!resolved_executable.has_value()) {
        return result;
    }

    std::vector<char*> argv;
    argv.reserve(spec.arguments.size() + 2);
    argv.push_back(const_cast<char*>(spec.executable.c_str()));
    for (const std::string& argument : spec.arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    const char* const resolved_executable_path = resolved_executable->c_str();
    char* const* const argv_data = argv.data();

    Pipe stdout_pipe;
    Pipe stderr_pipe;
    Pipe startup_pipe;
    if (!make_pipe(stdout_pipe, ProcessErrorStage::create_stdout_pipe, result) ||
        !make_pipe(stderr_pipe, ProcessErrorStage::create_stderr_pipe, result) ||
        !make_pipe(startup_pipe, ProcessErrorStage::create_startup_pipe, result) ||
        !set_nonblocking(stdout_pipe.read_end, result) ||
        !set_nonblocking(stderr_pipe.read_end, result) ||
        !set_nonblocking(startup_pipe.read_end, result)) {
        return result;
    }

    const int stdout_read_fd = stdout_pipe.read_end.get();
    const int stdout_write_fd = stdout_pipe.write_end.get();
    const int stderr_read_fd = stderr_pipe.read_end.get();
    const int stderr_write_fd = stderr_pipe.write_end.get();
    const int startup_read_fd = startup_pipe.read_end.get();
    const int startup_write_fd = startup_pipe.write_end.get();
    const pid_t child = ::fork();
    if (child < 0) {
        return parent_failure(ProcessErrorStage::fork, errno);
    }
    if (child == 0) {
        (void)::close(stdout_read_fd);
        (void)::close(stderr_read_fd);
        (void)::close(startup_read_fd);
        redirect_child_output(stdout_write_fd, STDOUT_FILENO,
                              ProcessErrorStage::child_dup_stdout, startup_write_fd);
        redirect_child_output(stderr_write_fd, STDERR_FILENO,
                              ProcessErrorStage::child_dup_stderr, startup_write_fd);
        ::execv(resolved_executable_path, argv_data);
        child_failure(startup_write_fd, ProcessErrorStage::child_exec);
    }

    stdout_pipe.write_end.reset();
    stderr_pipe.write_end.reset();
    startup_pipe.write_end.reset();

    std::string startup_bytes;
    bool io_ok = true;
    while (stdout_pipe.read_end.valid() || stderr_pipe.read_end.valid() || startup_pipe.read_end.valid()) {
        std::array<pollfd, 3> poll_fds{};
        std::array<int, 3> kinds{};
        nfds_t count = 0;
        const auto add_fd = [&](const UniqueFd& fd, int kind) {
            if (fd.valid()) {
                poll_fds[count] = {.fd = fd.get(), .events = POLLIN, .revents = 0};
                kinds[count] = kind;
                ++count;
            }
        };
        add_fd(stdout_pipe.read_end, 0);
        add_fd(stderr_pipe.read_end, 1);
        add_fd(startup_pipe.read_end, 2);

        int poll_result;
        do {
            poll_result = ::poll(poll_fds.data(), count, -1);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0) {
            result = parent_failure(ProcessErrorStage::parent_poll, errno);
            io_ok = false;
            break;
        }

        for (nfds_t index = 0; index < count; ++index) {
            if (poll_fds[index].revents == 0) {
                continue;
            }
            bool drained = false;
            if (kinds[index] == 0) {
                drained = drain_fd(stdout_pipe.read_end, result.stdout_data,
                                   ProcessErrorStage::parent_read_stdout, result);
            } else if (kinds[index] == 1) {
                drained = drain_fd(stderr_pipe.read_end, result.stderr_data,
                                   ProcessErrorStage::parent_read_stderr, result);
            } else {
                drained = drain_fd(startup_pipe.read_end, startup_bytes,
                                   ProcessErrorStage::parent_read_startup, result);
            }
            if (!drained) {
                io_ok = false;
                break;
            }
        }
        if (!io_ok) {
            break;
        }
    }

    stdout_pipe.read_end.reset();
    stderr_pipe.read_end.reset();
    startup_pipe.read_end.reset();

    int wait_status = 0;
    const bool waited = wait_for_child(child, wait_status, result);
    if (!io_ok || !waited) {
        return result;
    }

    if (!startup_bytes.empty()) {
        if (startup_bytes.size() != sizeof(StartupMessage)) {
            return parent_failure(ProcessErrorStage::startup_protocol, EPROTO);
        }
        StartupMessage message{};
        std::memcpy(&message, startup_bytes.data(), sizeof(message));
        result.outcome = ProcessOutcome::startup_failed;
        result.error = ProcessError{static_cast<ProcessErrorStage>(message.stage),
                                    message.error_number};
        return result;
    }
    if (WIFEXITED(wait_status)) {
        result.outcome = ProcessOutcome::exited;
        result.exit_code = WEXITSTATUS(wait_status);
        return result;
    }
    if (WIFSIGNALED(wait_status)) {
        result.outcome = ProcessOutcome::signaled;
        result.terminating_signal = WTERMSIG(wait_status);
        return result;
    }
    return parent_failure(ProcessErrorStage::parent_waitpid, ECHILD);
}

}  // namespace taskforge
