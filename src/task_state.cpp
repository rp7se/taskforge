#include "taskforge/task_state.hpp"

namespace taskforge {

TaskStateMachine::TaskStateMachine() noexcept : state_(TaskState::queued) {}

TaskState TaskStateMachine::state() const noexcept {
    return state_.load();
}

TransitionResult TaskStateMachine::try_transition(TaskState expected, TaskState desired) noexcept {
    if (!is_legal_transition(expected, desired)) {
        return TransitionResult::invalid_transition;
    }

    TaskState observed = expected;
    if (state_.compare_exchange_strong(observed, desired)) {
        return TransitionResult::applied;
    }
    return TransitionResult::state_mismatch;
}

}  // namespace taskforge
