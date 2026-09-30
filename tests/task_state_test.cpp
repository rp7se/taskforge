#include <algorithm>
#include <array>
#include <barrier>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "taskforge/task_state.hpp"

namespace {

constexpr int kRaceIterations = 256;
constexpr std::size_t kRaceCompetitors = 8;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "test failure: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

void require_applied(taskforge::TaskStateMachine& machine, taskforge::TaskState expected,
                     taskforge::TaskState desired) {
    require(machine.try_transition(expected, desired) == taskforge::TransitionResult::applied,
            "expected transition was not applied");
}

void advance_to_running(taskforge::TaskStateMachine& machine) {
    require_applied(machine, taskforge::TaskState::queued, taskforge::TaskState::ready);
    require_applied(machine, taskforge::TaskState::ready, taskforge::TaskState::starting);
    require_applied(machine, taskforge::TaskState::starting, taskforge::TaskState::running);
}

void test_initial_state() {
    const taskforge::TaskStateMachine machine;
    require(machine.state() == taskforge::TaskState::queued, "initial state is not queued");
}

void test_normal_lifecycle() {
    taskforge::TaskStateMachine machine;
    advance_to_running(machine);
    require_applied(machine, taskforge::TaskState::running, taskforge::TaskState::success);
    require(machine.state() == taskforge::TaskState::success, "normal lifecycle did not succeed");
}

void test_failure_lifecycles() {
    taskforge::TaskStateMachine startup_failure;
    require_applied(startup_failure, taskforge::TaskState::queued, taskforge::TaskState::ready);
    require_applied(startup_failure, taskforge::TaskState::ready, taskforge::TaskState::starting);
    require_applied(startup_failure, taskforge::TaskState::starting, taskforge::TaskState::failed);

    taskforge::TaskStateMachine runtime_failure;
    advance_to_running(runtime_failure);
    require_applied(runtime_failure, taskforge::TaskState::running, taskforge::TaskState::failed);
}

void test_invalid_and_mismatched_transitions() {
    taskforge::TaskStateMachine machine;
    require(machine.try_transition(taskforge::TaskState::queued, taskforge::TaskState::running) ==
                taskforge::TransitionResult::invalid_transition,
            "queued-to-running was accepted");
    require(machine.state() == taskforge::TaskState::queued, "invalid transition changed state");

    require_applied(machine, taskforge::TaskState::queued, taskforge::TaskState::ready);
    require(machine.try_transition(taskforge::TaskState::queued, taskforge::TaskState::ready) ==
                taskforge::TransitionResult::state_mismatch,
            "stale expected state was not rejected");
    require_applied(machine, taskforge::TaskState::ready, taskforge::TaskState::starting);
    require_applied(machine, taskforge::TaskState::starting, taskforge::TaskState::running);
    require(machine.try_transition(taskforge::TaskState::running, taskforge::TaskState::ready) ==
                taskforge::TransitionResult::invalid_transition,
            "running-to-ready was accepted");
}

void test_terminal_immutability() {
    taskforge::TaskStateMachine success_machine;
    advance_to_running(success_machine);
    require_applied(success_machine, taskforge::TaskState::running, taskforge::TaskState::success);
    require(success_machine.try_transition(taskforge::TaskState::success, taskforge::TaskState::failed) ==
                taskforge::TransitionResult::invalid_transition,
            "success terminal transition was accepted");
    require(success_machine.state() == taskforge::TaskState::success, "success state changed");

    taskforge::TaskStateMachine failed_machine;
    advance_to_running(failed_machine);
    require_applied(failed_machine, taskforge::TaskState::running, taskforge::TaskState::failed);
    require(failed_machine.try_transition(taskforge::TaskState::failed, taskforge::TaskState::running) ==
                taskforge::TransitionResult::invalid_transition,
            "failed terminal transition was accepted");
    require(failed_machine.state() == taskforge::TaskState::failed, "failed state changed");

    taskforge::TaskStateMachine timeout_machine;
    advance_to_running(timeout_machine);
    require_applied(timeout_machine, taskforge::TaskState::running, taskforge::TaskState::timeout);
    require(timeout_machine.try_transition(taskforge::TaskState::timeout, taskforge::TaskState::failed) ==
                taskforge::TransitionResult::invalid_transition,
            "timeout terminal transition was accepted");

    taskforge::TaskStateMachine cancelled_machine;
    require_applied(cancelled_machine, taskforge::TaskState::queued, taskforge::TaskState::cancelled);
    require(cancelled_machine.try_transition(taskforge::TaskState::cancelled,
                                             taskforge::TaskState::success) ==
                taskforge::TransitionResult::invalid_transition,
            "cancelled terminal transition was accepted");
}

void test_cancellation_and_timeout_edges() {
    for (const taskforge::TaskState from : {taskforge::TaskState::queued, taskforge::TaskState::ready,
                                            taskforge::TaskState::starting}) {
        taskforge::TaskStateMachine machine;
        if (from == taskforge::TaskState::ready) {
            require_applied(machine, taskforge::TaskState::queued, taskforge::TaskState::ready);
        } else if (from == taskforge::TaskState::starting) {
            require_applied(machine, taskforge::TaskState::queued, taskforge::TaskState::ready);
            require_applied(machine, taskforge::TaskState::ready, taskforge::TaskState::starting);
        }
        require_applied(machine, from, taskforge::TaskState::cancelled);
    }

    for (const taskforge::TaskState from : {taskforge::TaskState::queued, taskforge::TaskState::ready,
                                            taskforge::TaskState::starting}) {
        taskforge::TaskStateMachine machine;
        if (from == taskforge::TaskState::ready) {
            require_applied(machine, taskforge::TaskState::queued, taskforge::TaskState::ready);
        } else if (from == taskforge::TaskState::starting) {
            require_applied(machine, taskforge::TaskState::queued, taskforge::TaskState::ready);
            require_applied(machine, taskforge::TaskState::ready, taskforge::TaskState::starting);
        }
        require(machine.try_transition(from, taskforge::TaskState::timeout) ==
                    taskforge::TransitionResult::invalid_transition,
                "timeout was accepted before running");
    }
}

void test_concurrent_terminal_race() {
    for (int iteration = 0; iteration < kRaceIterations; ++iteration) {
        taskforge::TaskStateMachine machine;
        advance_to_running(machine);
        std::array<taskforge::TransitionResult, kRaceCompetitors> results{};
        std::barrier start_gate(static_cast<std::ptrdiff_t>(kRaceCompetitors));
        {
            std::vector<std::jthread> threads;
            threads.reserve(kRaceCompetitors);
            for (std::size_t index = 0; index < kRaceCompetitors; ++index) {
                threads.emplace_back([&machine, &results, &start_gate, index] {
                    start_gate.arrive_and_wait();
                    constexpr std::array terminal_states{taskforge::TaskState::success,
                                                         taskforge::TaskState::failed,
                                                         taskforge::TaskState::timeout,
                                                         taskforge::TaskState::cancelled};
                    const taskforge::TaskState desired = terminal_states[index % terminal_states.size()];
                    results[index] = machine.try_transition(taskforge::TaskState::running, desired);
                });
            }
        }

        const auto winner_count = static_cast<std::size_t>(
            std::count(results.begin(), results.end(), taskforge::TransitionResult::applied));
        const auto mismatch_count = static_cast<std::size_t>(
            std::count(results.begin(), results.end(), taskforge::TransitionResult::state_mismatch));
        require(winner_count == 1, "concurrent terminal race had an incorrect winner count");
        require(mismatch_count == kRaceCompetitors - 1,
                "concurrent terminal race did not reject all losers as mismatches");
        require(taskforge::is_terminal(machine.state()), "race did not end in a terminal state");

        const taskforge::TaskState final_state = machine.state();
        require(machine.try_transition(final_state, taskforge::TaskState::failed) ==
                    taskforge::TransitionResult::invalid_transition,
                "terminal state accepted a later transition");
        require(machine.state() == final_state, "terminal state changed after race");
    }
}

}  // namespace

int main() {
    test_initial_state();
    test_normal_lifecycle();
    test_failure_lifecycles();
    test_invalid_and_mismatched_transitions();
    test_terminal_immutability();
    test_cancellation_and_timeout_edges();
    test_concurrent_terminal_race();
    return 0;
}
