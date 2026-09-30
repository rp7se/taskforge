#pragma once

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>

#include "taskforge/process_executor.hpp"
#include "taskforge/task_state.hpp"

namespace taskforge {

namespace detail {
struct TaskControl;
}

class ConcurrentExecutor;

enum class SubmitStatus {
    accepted,
    queue_full,
    shutting_down,
};

class TaskHandle {
public:
    [[nodiscard]] std::uint64_t id() const noexcept;
    [[nodiscard]] TaskState state() const noexcept;
    [[nodiscard]] const ProcessResult& get() const;
    [[nodiscard]] std::future_status wait_for(std::chrono::milliseconds timeout) const;

private:
    explicit TaskHandle(std::shared_ptr<detail::TaskControl> control) noexcept;

    std::shared_ptr<detail::TaskControl> control_;

    friend class ConcurrentExecutor;
};

struct SubmitResult {
    SubmitStatus status = SubmitStatus::shutting_down;
    std::optional<TaskHandle> handle;

    [[nodiscard]] bool accepted() const noexcept { return status == SubmitStatus::accepted; }
};

class ConcurrentExecutor {
public:
    ConcurrentExecutor(std::size_t worker_count, std::size_t queue_capacity);
    ~ConcurrentExecutor();

    ConcurrentExecutor(const ConcurrentExecutor&) = delete;
    ConcurrentExecutor& operator=(const ConcurrentExecutor&) = delete;
    ConcurrentExecutor(ConcurrentExecutor&&) = delete;
    ConcurrentExecutor& operator=(ConcurrentExecutor&&) = delete;

    [[nodiscard]] SubmitResult submit(ProcessSpec spec,
                                      ProcessExecutionOptions options = {});

    void shutdown();

private:
    void worker_loop();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace taskforge
