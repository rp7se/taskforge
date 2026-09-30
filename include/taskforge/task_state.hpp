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
};

enum class TransitionResult {
    applied,
    invalid_transition,
    state_mismatch,
};

[[nodiscard]] constexpr bool is_legal_transition(TaskState expected, TaskState desired) noexcept {
    switch (expected) {
        case TaskState::queued:
            return desired == TaskState::ready;
        case TaskState::ready:
            return desired == TaskState::starting;
        case TaskState::starting:
            return desired == TaskState::running || desired == TaskState::failed;
        case TaskState::running:
            return desired == TaskState::success || desired == TaskState::failed;
        case TaskState::success:
        case TaskState::failed:
            return false;
    }
    return false;
}

[[nodiscard]] constexpr bool is_terminal(TaskState state) noexcept {
    return state == TaskState::success || state == TaskState::failed;
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
