#pragma once

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>

#include "taskforge/process_executor.hpp"
#include "taskforge/resource_admission.hpp"
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
    resource_request_required,
    invalid_resource_request,
    resource_request_exceeds_capacity,
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

struct ConcurrentExecutorConfig {
    std::size_t worker_count;
    std::size_t queue_capacity;
    std::optional<ResourceCapacity> resource_capacity;
};

class ConcurrentExecutor {
public:
    ConcurrentExecutor(std::size_t worker_count, std::size_t queue_capacity);
    explicit ConcurrentExecutor(ConcurrentExecutorConfig config);
    ~ConcurrentExecutor();

    ConcurrentExecutor(const ConcurrentExecutor&) = delete;
    ConcurrentExecutor& operator=(const ConcurrentExecutor&) = delete;
    ConcurrentExecutor(ConcurrentExecutor&&) = delete;
    ConcurrentExecutor& operator=(ConcurrentExecutor&&) = delete;

    [[nodiscard]] SubmitResult submit(ProcessSpec spec,
                                      ProcessExecutionOptions options = {});
    [[nodiscard]] SubmitResult submit(ProcessSpec spec, ProcessExecutionOptions options,
                                      ResourceRequest request);

    // Returns std::nullopt when resource admission is disabled.
    [[nodiscard]] std::optional<ResourceSnapshot> resource_snapshot() const;

    void shutdown();

private:
    [[nodiscard]] SubmitResult submit_impl(ProcessSpec spec, ProcessExecutionOptions options,
                                           std::optional<ResourceRequest> request);
    void worker_loop();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace taskforge
