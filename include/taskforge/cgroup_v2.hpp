#pragma once

#include <filesystem>
#include <optional>
#include <sys/types.h>

#include "taskforge/process_executor.hpp"

namespace taskforge {

[[nodiscard]] std::string serialize_cpu_max(CpuMax value);

// Parent-side cgroup v2 setup.  It deliberately owns only a generated child
// below the caller-provided delegated root.
class CgroupV2Task {
public:
    CgroupV2Task(CgroupV2Task&&) noexcept;
    CgroupV2Task& operator=(CgroupV2Task&&) noexcept;
    ~CgroupV2Task();

    CgroupV2Task(const CgroupV2Task&) = delete;
    CgroupV2Task& operator=(const CgroupV2Task&) = delete;

    [[nodiscard]] static std::optional<CgroupV2Task> create(const CgroupV2Options& options,
                                                              ProcessResult& failure);
    [[nodiscard]] bool attach(pid_t pid, ProcessResult& failure) const;
    void collect_events(ProcessResult& result) const;
    void cleanup(ProcessResult& result) noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    CgroupV2Task(std::filesystem::path path, CgroupV2Limits limits) noexcept;

    std::filesystem::path path_;
    CgroupV2Limits limits_;
    bool cleaned_ = false;
};

}  // namespace taskforge
