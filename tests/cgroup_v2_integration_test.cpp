#include "taskforge/process_executor.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stop_token>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

std::filesystem::path root;

void require(bool value, const char* message) {
    if (!value) {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

taskforge::ProcessSpec helper(std::initializer_list<const char*> arguments) {
    return {.executable = TASKFORGE_PROCESS_TEST_HELPER,
            .arguments = std::vector<std::string>(arguments.begin(), arguments.end())};
}

taskforge::ProcessExecutionOptions options(taskforge::CgroupV2Limits limits) {
    return {.cgroup = taskforge::CgroupV2Options{.root = root, .limits = std::move(limits)}};
}

void require_clean() {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        require(entry.path().filename().string().rfind("taskforge-", 0) != 0,
                "per-task cgroup directory was not removed");
    }
}

void require_events(const taskforge::ProcessResult& result, const char* message) {
    require(result.cgroup_events.has_value() && !result.cgroup_diagnostic_error.has_value(), message);
    require(!result.cgroup_cleanup_error.has_value(), "cgroup cleanup failed");
}

void membership_before_exec() {
    const auto result = taskforge::run_process(helper({"cgroup-membership"}), options({.pids_max = 16}));
    require(result.outcome == taskforge::ProcessOutcome::exited && result.exit_code == 0,
            "membership helper did not exit successfully");
    const std::string prefix = "/sys/fs/cgroup";
    const std::string expected_root = root.string().rfind(prefix, 0) == 0 ? root.string().substr(prefix.size()) : root.string();
    require(result.stdout_data.find("0::" + expected_root + "/taskforge-") != std::string::npos,
            "user program was not attached to the task cgroup before exec");
    require_events(result, "membership diagnostics were unavailable");
    require_clean();
}

void cpu_limit() {
    const auto result = taskforge::run_process(helper({"cpu-burn", "700"}),
                                                options({.cpu = taskforge::CpuMax{10000, 100000}}));
    require(result.outcome == taskforge::ProcessOutcome::exited && result.exit_code == 0,
            "cpu limited task did not exit normally");
    require_events(result, "cpu diagnostics were unavailable");
    require(result.cgroup_events->cpu_nr_throttled.value_or(0) > 0, "cpu.max did not throttle");
    require_clean();
}

void memory_limit() {
    const auto result = taskforge::run_process(helper({"allocate-touch", "134217728"}),
                                                options({.memory_max_bytes = 8ULL * 1024 * 1024}));
    require_events(result, "memory diagnostics were unavailable");
    require(result.cgroup_events->memory_oom_kill.value_or(0) > 0, "memory.max did not report oom_kill");
    require_clean();
}

void pids_limit() {
    const auto result = taskforge::run_process(helper({"fork-hold", "12", "40"}), options({.pids_max = 4}));
    require(result.outcome == taskforge::ProcessOutcome::exited && result.exit_code == 0,
            "pids limited helper did not exit normally");
    require_events(result, "pids diagnostics were unavailable");
    require(result.cgroup_events->pids_max.value_or(0) > 0, "pids.max did not report max event");
    require_clean();
}

void timeout_and_cancellation() {
    const auto timeout = taskforge::run_process(helper({"sleep", "300"}),
                                               {.timeout = 30ms,
                                                .termination_grace = 30ms,
                                                .cgroup = taskforge::CgroupV2Options{.root = root, .limits = {.pids_max = 16}}});
    require(timeout.outcome == taskforge::ProcessOutcome::timed_out, "timeout outcome changed with cgroup");
    require_events(timeout, "timeout cgroup diagnostics were unavailable");
    require_clean();

    std::stop_source source;
    std::jthread canceller([&source] {
        std::this_thread::sleep_for(30ms);
        source.request_stop();
    });
    const auto cancelled = taskforge::run_process(
        helper({"sleep", "300"}), {.stop_token = source.get_token(),
                                     .termination_grace = 30ms,
                                     .cgroup = taskforge::CgroupV2Options{.root = root, .limits = {.pids_max = 16}}});
    require(cancelled.outcome == taskforge::ProcessOutcome::cancelled, "cancellation outcome changed with cgroup");
    require_events(cancelled, "cancellation cgroup diagnostics were unavailable");
    require_clean();
}

}  // namespace

int main() {
    const char* configured_root = std::getenv("TASKFORGE_CGROUP_TEST_ROOT");
    if (configured_root == nullptr || *configured_root == '\0' || !std::filesystem::is_directory(configured_root)) {
        return 77;
    }
    root = configured_root;
    membership_before_exec();
    cpu_limit();
    memory_limit();
    pids_limit();
    timeout_and_cancellation();
}
