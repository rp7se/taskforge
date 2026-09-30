#pragma once

#include <atomic>

namespace taskforge {

enum class TaskState {
    queued,
    ready,
    starting,
    running,
    success,
    failed,
    timeout,
    cancelled,
};

enum class TransitionResult {
    applied,
    invalid_transition,
    state_mismatch,
};

[[nodiscard]] constexpr bool is_legal_transition(TaskState expected, TaskState desired) noexcept {
    switch (expected) {
        case TaskState::queued:
            return desired == TaskState::ready || desired == TaskState::cancelled;
        case TaskState::ready:
            return desired == TaskState::starting || desired == TaskState::cancelled;
        case TaskState::starting:
            return desired == TaskState::running || desired == TaskState::failed ||
                   desired == TaskState::cancelled;
        case TaskState::running:
            return desired == TaskState::success || desired == TaskState::failed ||
                   desired == TaskState::timeout || desired == TaskState::cancelled;
        case TaskState::success:
        case TaskState::failed:
        case TaskState::timeout:
        case TaskState::cancelled:
            return false;
    }
    return false;
}

[[nodiscard]] constexpr bool is_terminal(TaskState state) noexcept {
    return state == TaskState::success || state == TaskState::failed ||
           state == TaskState::timeout || state == TaskState::cancelled;
}

class TaskStateMachine {
public:
    TaskStateMachine() noexcept;

    [[nodiscard]] TaskState state() const noexcept;
    [[nodiscard]] TransitionResult try_transition(TaskState expected, TaskState desired) noexcept;

private:
    std::atomic<TaskState> state_;
};

}  // namespace taskforge
