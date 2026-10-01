#include "taskforge/cgroup_v2.hpp"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace taskforge {
namespace {

std::atomic<std::uint64_t> next_cgroup_id{1};

ProcessResult failure(ProcessErrorStage stage, int error_number) {
    ProcessResult result{};
    result.outcome = ProcessOutcome::parent_error;
    result.error = ProcessError{stage, error_number};
    return result;
}

bool write_text(const std::filesystem::path& path, std::string_view value, int& error_number) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        error_number = errno;
        return false;
    }
    std::size_t offset = 0;
    while (offset < value.size()) {
        const ssize_t written = ::write(fd, value.data() + offset, value.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        error_number = written < 0 ? errno : EIO;
        (void)::close(fd);
        return false;
    }
    if (::close(fd) != 0) {
        error_number = errno;
        return false;
    }
    return true;
}

bool read_text(const std::filesystem::path& path, std::string& value, int& error_number) {
    std::ifstream input(path);
    if (!input) {
        error_number = errno == 0 ? ENOENT : errno;
        return false;
    }
    value.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad()) {
        error_number = EIO;
        return false;
    }
    return true;
}

bool has_token(std::string_view haystack, std::string_view needle) {
    std::istringstream stream{std::string(haystack)};
    std::string token;
    while (stream >> token) {
        if (token == needle) {
            return true;
        }
    }
    return false;
}

bool requested(const CgroupV2Limits& limits, std::string_view controller) {
    return (controller == "cpu" && limits.cpu.has_value()) ||
           (controller == "memory" && limits.memory_max_bytes.has_value()) ||
           (controller == "pids" && limits.pids_max.has_value());
}

std::optional<std::uint64_t> counter(std::string_view text, std::string_view key) {
    std::istringstream stream{std::string(text)};
    std::string name;
    std::uint64_t value = 0;
    while (stream >> name >> value) {
        if (name == key) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> number(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t\n");
    if (first == std::string_view::npos) return std::nullopt;
    const std::size_t last = text.find_last_not_of(" \t\n");
    std::uint64_t value = 0;
    const char* begin = text.data() + first;
    const char* end = text.data() + last + 1;
    const auto [parsed, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && parsed == end ? std::optional(value) : std::nullopt;
}

bool valid_limits(const CgroupV2Limits& limits, ProcessResult& result) {
    if ((limits.cpu && (limits.cpu->quota_us == 0 || limits.cpu->period_us == 0)) ||
        (limits.memory_max_bytes && *limits.memory_max_bytes == 0) ||
        (limits.pids_max && *limits.pids_max == 0)) {
        result = failure(ProcessErrorStage::cgroup_root_validation, EINVAL);
        return false;
    }
    return true;
}

}  // namespace

std::string serialize_cpu_max(CpuMax value) {
    return std::to_string(value.quota_us) + " " + std::to_string(value.period_us);
}

CgroupV2Task::CgroupV2Task(std::filesystem::path path, CgroupV2Limits limits) noexcept
    : path_(std::move(path)), limits_(std::move(limits)) {}

CgroupV2Task::CgroupV2Task(CgroupV2Task&& other) noexcept
    : path_(std::move(other.path_)), limits_(std::move(other.limits_)), cleaned_(other.cleaned_) {
    other.cleaned_ = true;
}

CgroupV2Task& CgroupV2Task::operator=(CgroupV2Task&& other) noexcept {
    if (this != &other) {
        path_ = std::move(other.path_);
        limits_ = std::move(other.limits_);
        cleaned_ = other.cleaned_;
        other.cleaned_ = true;
    }
    return *this;
}

CgroupV2Task::~CgroupV2Task() = default;

std::optional<CgroupV2Task> CgroupV2Task::create(const CgroupV2Options& options,
                                                   ProcessResult& result) {
    if (options.root.empty() || !valid_limits(options.limits, result)) {
        if (!result.error) {
            result = failure(ProcessErrorStage::cgroup_root_validation, EINVAL);
        }
        return std::nullopt;
    }
    std::error_code fs_error;
    if (!std::filesystem::is_directory(options.root, fs_error)) {
        result = failure(ProcessErrorStage::cgroup_root_validation,
                         fs_error ? fs_error.value() : ENOTDIR);
        return std::nullopt;
    }
    std::string controllers;
    int error_number = 0;
    if (!read_text(options.root / "cgroup.controllers", controllers, error_number)) {
        result = failure(ProcessErrorStage::cgroup_root_validation, error_number);
        return std::nullopt;
    }
    for (const char* controller : {"cpu", "memory", "pids"}) {
        if (requested(options.limits, controller) && !has_token(controllers, controller)) {
            result = failure(ProcessErrorStage::cgroup_controller_setup, ENODEV);
            return std::nullopt;
        }
    }
    std::string enabled;
    if (!read_text(options.root / "cgroup.subtree_control", enabled, error_number)) {
        result = failure(ProcessErrorStage::cgroup_controller_setup, error_number);
        return std::nullopt;
    }
    std::string enable;
    for (const char* controller : {"cpu", "memory", "pids"}) {
        if (requested(options.limits, controller) && !has_token(enabled, controller)) {
            if (!enable.empty()) enable += ' ';
            enable += '+';
            enable += controller;
        }
    }
    if (!enable.empty() && !write_text(options.root / "cgroup.subtree_control", enable, error_number)) {
        result = failure(ProcessErrorStage::cgroup_controller_setup, error_number);
        return std::nullopt;
    }

    const auto id = next_cgroup_id.fetch_add(1, std::memory_order_relaxed);
    const auto task_path = options.root / ("taskforge-" + std::to_string(::getpid()) + "-" + std::to_string(id));
    if (::mkdir(task_path.c_str(), 0755) != 0) {
        result = failure(ProcessErrorStage::cgroup_create, errno);
        return std::nullopt;
    }
    CgroupV2Task task(task_path, options.limits);
    const auto limit_failure = [&] {
        ProcessResult cleanup_result{};
        task.cleanup(cleanup_result);
        return std::nullopt;
    };
    if (options.limits.cpu) {
        const std::string text = serialize_cpu_max(*options.limits.cpu);
        if (!write_text(task_path / "cpu.max", text, error_number)) {
            result = failure(ProcessErrorStage::cgroup_limit_write, error_number);
            return limit_failure();
        }
    }
    if (options.limits.memory_max_bytes &&
        !write_text(task_path / "memory.max", std::to_string(*options.limits.memory_max_bytes), error_number)) {
        result = failure(ProcessErrorStage::cgroup_limit_write, error_number);
        return limit_failure();
    }
    if (options.limits.pids_max &&
        !write_text(task_path / "pids.max", std::to_string(*options.limits.pids_max), error_number)) {
        result = failure(ProcessErrorStage::cgroup_limit_write, error_number);
        return limit_failure();
    }
    return task;
}

bool CgroupV2Task::attach(pid_t pid, ProcessResult& result) const {
    int error_number = 0;
    if (!write_text(path_ / "cgroup.procs", std::to_string(pid), error_number)) {
        result = failure(ProcessErrorStage::cgroup_attach, error_number);
        return false;
    }
    return true;
}

void CgroupV2Task::collect_events(ProcessResult& result) const {
    CgroupResourceEvents events{};
    bool any = false;
    int error_number = 0;
    std::string text;
    const auto read_counter = [&](const char* file, const char* key, std::optional<std::uint64_t>& target) {
        if (!read_text(path_ / file, text, error_number)) return false;
        target = counter(text, key);
        return target.has_value();
    };
    if (limits_.cpu) { any = true; if (!read_counter("cpu.stat", "nr_throttled", events.cpu_nr_throttled)) goto diagnostic_failure; }
    if (limits_.memory_max_bytes) {
        any = true;
        if (!read_counter("memory.events", "oom_kill", events.memory_oom_kill) ||
            !read_counter("memory.events", "max", events.memory_max_events)) goto diagnostic_failure;
        if (!read_text(path_ / "memory.max", text, error_number) ||
            !(events.memory_max_bytes = number(text)).has_value()) goto diagnostic_failure;
    }
    if (limits_.pids_max) { any = true; if (!read_counter("pids.events", "max", events.pids_max)) goto diagnostic_failure; }
    if (any) result.cgroup_events = events;
    return;
diagnostic_failure:
    result.cgroup_diagnostic_error = ProcessError{ProcessErrorStage::cgroup_diagnostics, error_number == 0 ? EPROTO : error_number};
}

void CgroupV2Task::cleanup(ProcessResult& result) noexcept {
    if (cleaned_) return;
    cleaned_ = true;
    if (::rmdir(path_.c_str()) != 0) {
        result.cgroup_cleanup_error = ProcessError{ProcessErrorStage::cgroup_cleanup, errno};
    }
}

}  // namespace taskforge
