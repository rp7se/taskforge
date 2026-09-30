#include "taskforge/concurrent_executor.hpp"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace taskforge {

namespace detail {

struct TaskControl {
    TaskControl(std::uint64_t task_id, ProcessSpec process_spec,
                ProcessExecutionOptions execution_options)
        : id(task_id), spec(std::move(process_spec)), options(std::move(execution_options)),
          completion(promise.get_future().share()) {}

    const std::uint64_t id;
    const ProcessSpec spec;
    const ProcessExecutionOptions options;
    TaskStateMachine state_machine;
    std::promise<ProcessResult> promise;
    std::shared_future<ProcessResult> completion;
};

}  // namespace detail

struct ConcurrentExecutor::Impl {
    explicit Impl(std::size_t capacity) : queue_capacity(capacity) {}

    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<std::shared_ptr<detail::TaskControl>> queue;
    const std::size_t queue_capacity;
    bool accepting = true;

    std::mutex shutdown_mutex;
    std::vector<std::jthread> workers;
};

namespace {

std::atomic<std::uint64_t> next_task_id{1};

ProcessResult cancelled_before_run_result() {
    return {.stdout_data = {},
            .stderr_data = {},
            .outcome = ProcessOutcome::cancelled,
            .exit_code = std::nullopt,
            .terminating_signal = std::nullopt,
            .error = std::nullopt,
            .cleanup_outcome = ProcessCleanupOutcome::not_needed};
}

ProcessResult invariant_failure_result() {
    return {.stdout_data = {},
            .stderr_data = {},
            .outcome = ProcessOutcome::parent_error,
            .exit_code = std::nullopt,
            .terminating_signal = std::nullopt,
            .error = ProcessError{.stage = ProcessErrorStage::parent_waitpid, .system_error = EPROTO},
            .cleanup_outcome = ProcessCleanupOutcome::not_needed};
}

TaskState terminal_state_for(const ProcessResult& result) noexcept {
    switch (result.outcome) {
        case ProcessOutcome::exited:
            return result.exit_code == 0 ? TaskState::success : TaskState::failed;
        case ProcessOutcome::timed_out:
            return TaskState::timeout;
        case ProcessOutcome::cancelled:
            return TaskState::cancelled;
        case ProcessOutcome::signaled:
        case ProcessOutcome::startup_failed:
        case ProcessOutcome::parent_error:
            return TaskState::failed;
    }
    return TaskState::failed;
}

void set_completion(const std::shared_ptr<detail::TaskControl>& control, ProcessResult result) {
    control->promise.set_value(std::move(result));
}

void complete_invariant_failure(const std::shared_ptr<detail::TaskControl>& control) {
    for (;;) {
        const TaskState state = control->state_machine.state();
        if (is_terminal(state)) {
            set_completion(control, invariant_failure_result());
            return;
        }

        TaskState desired = TaskState::failed;
        if (state == TaskState::queued) {
            desired = TaskState::ready;
        } else if (state == TaskState::ready) {
            desired = TaskState::starting;
        }

        if (control->state_machine.try_transition(state, desired) == TransitionResult::applied) {
            if (desired == TaskState::failed) {
                set_completion(control, invariant_failure_result());
                return;
            }
        }
    }
}

bool transition_or_complete_invariant_failure(const std::shared_ptr<detail::TaskControl>& control,
                                              TaskState expected, TaskState desired) {
    if (control->state_machine.try_transition(expected, desired) == TransitionResult::applied) {
        return true;
    }
    complete_invariant_failure(control);
    return false;
}

void complete_cancelled_before_run(const std::shared_ptr<detail::TaskControl>& control,
                                   TaskState state) {
    if (!transition_or_complete_invariant_failure(control, state, TaskState::cancelled)) {
        return;
    }
    set_completion(control, cancelled_before_run_result());
}

}  // namespace

TaskHandle::TaskHandle(std::shared_ptr<detail::TaskControl> control) noexcept
    : control_(std::move(control)) {}

std::uint64_t TaskHandle::id() const noexcept {
    return control_->id;
}

TaskState TaskHandle::state() const noexcept {
    return control_->state_machine.state();
}

const ProcessResult& TaskHandle::get() const {
    return control_->completion.get();
}

std::future_status TaskHandle::wait_for(std::chrono::milliseconds timeout) const {
    return control_->completion.wait_for(timeout);
}

ConcurrentExecutor::ConcurrentExecutor(std::size_t worker_count, std::size_t queue_capacity)
    : impl_(std::make_unique<Impl>(queue_capacity)) {
    if (worker_count == 0) {
        throw std::invalid_argument("worker_count must be at least one");
    }
    if (queue_capacity == 0) {
        throw std::invalid_argument("queue_capacity must be at least one");
    }

    impl_->workers.reserve(worker_count);
    try {
        for (std::size_t index = 0; index < worker_count; ++index) {
            impl_->workers.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        {
            std::lock_guard lock(impl_->queue_mutex);
            impl_->accepting = false;
        }
        impl_->queue_cv.notify_all();
        throw;
    }
}

ConcurrentExecutor::~ConcurrentExecutor() {
    shutdown();
}

SubmitResult ConcurrentExecutor::submit(ProcessSpec spec, ProcessExecutionOptions options) {
    std::lock_guard lock(impl_->queue_mutex);
    if (!impl_->accepting) {
        return {.status = SubmitStatus::shutting_down, .handle = std::nullopt};
    }
    if (impl_->queue.size() >= impl_->queue_capacity) {
        return {.status = SubmitStatus::queue_full, .handle = std::nullopt};
    }

    auto control = std::make_shared<detail::TaskControl>(
        next_task_id.fetch_add(1, std::memory_order_relaxed), std::move(spec), std::move(options));
    impl_->queue.push_back(control);
    impl_->queue_cv.notify_one();
    return {.status = SubmitStatus::accepted, .handle = TaskHandle(std::move(control))};
}

void ConcurrentExecutor::shutdown() {
    std::lock_guard shutdown_lock(impl_->shutdown_mutex);
    {
        std::lock_guard queue_lock(impl_->queue_mutex);
        impl_->accepting = false;
    }
    impl_->queue_cv.notify_all();
    for (std::jthread& worker : impl_->workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ConcurrentExecutor::worker_loop() {
    for (;;) {
        std::shared_ptr<detail::TaskControl> control;
        {
            std::unique_lock lock(impl_->queue_mutex);
            impl_->queue_cv.wait(lock, [this] { return !impl_->queue.empty() || !impl_->accepting; });
            if (impl_->queue.empty()) {
                return;
            }
            control = std::move(impl_->queue.front());
            impl_->queue.pop_front();
        }

        try {
            if (control->options.stop_token.stop_requested()) {
                complete_cancelled_before_run(control, TaskState::queued);
                continue;
            }
            if (!transition_or_complete_invariant_failure(control, TaskState::queued, TaskState::ready)) {
                continue;
            }
            if (control->options.stop_token.stop_requested()) {
                complete_cancelled_before_run(control, TaskState::ready);
                continue;
            }
            if (!transition_or_complete_invariant_failure(control, TaskState::ready, TaskState::starting)) {
                continue;
            }
            if (control->options.stop_token.stop_requested()) {
                complete_cancelled_before_run(control, TaskState::starting);
                continue;
            }
            if (!transition_or_complete_invariant_failure(control, TaskState::starting, TaskState::running)) {
                continue;
            }

            ProcessResult result = run_process(control->spec, control->options);
            if (!transition_or_complete_invariant_failure(control, TaskState::running,
                                                          terminal_state_for(result))) {
                continue;
            }
            set_completion(control, std::move(result));
        } catch (...) {
            complete_invariant_failure(control);
        }
    }
}

}  // namespace taskforge
