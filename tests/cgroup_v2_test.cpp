#include "taskforge/cgroup_v2.hpp"
#include "taskforge/process_executor.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

void require(bool value, const char* message) {
    if (!value) {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

void require_stage(const taskforge::ProcessResult& result, taskforge::ProcessErrorStage stage,
                   const char* message) {
    require(result.outcome == taskforge::ProcessOutcome::parent_error && result.error.has_value() &&
                result.error->stage == stage,
            message);
}

void test_serialization() {
    require(taskforge::serialize_cpu_max({.quota_us = 20000, .period_us = 100000}) == "20000 100000",
            "cpu.max serialization was incorrect");
}

void test_invalid_limits_prevent_launch() {
    const auto result = taskforge::run_process(
        {.executable = "/bin/echo", .arguments = {"must-not-run"}},
        {.cgroup = taskforge::CgroupV2Options{.root = "/definitely/not/a/cgroup",
                                              .limits = {.cpu = taskforge::CpuMax{0, 100000}}}});
    require_stage(result, taskforge::ProcessErrorStage::cgroup_root_validation,
                  "invalid cpu limit was not rejected before fork");
    require(result.stdout_data.empty(), "invalid cgroup configuration executed the child");
}

void test_missing_root() {
    taskforge::ProcessResult result{};
    const auto task = taskforge::CgroupV2Task::create(
        {.root = "/definitely/not/a/taskforge-cgroup", .limits = {.pids_max = 4}}, result);
    require(!task.has_value(), "missing root was accepted");
    require_stage(result, taskforge::ProcessErrorStage::cgroup_root_validation,
                  "missing root did not identify root validation");
}

void test_missing_requested_controller() {
    const auto root = std::filesystem::temp_directory_path() / "taskforge-cgroup-v2-unit-root";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);
    {
        std::ofstream(root / "cgroup.controllers") << "cpu\n";
        std::ofstream(root / "cgroup.subtree_control") << "\n";
    }
    taskforge::ProcessResult result{};
    const auto task = taskforge::CgroupV2Task::create(
        {.root = root, .limits = {.memory_max_bytes = 4096}}, result);
    require(!task.has_value(), "missing requested controller was accepted");
    require_stage(result, taskforge::ProcessErrorStage::cgroup_controller_setup,
                  "missing controller did not identify controller setup");
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    test_serialization();
    test_invalid_limits_prevent_launch();
    test_missing_root();
    test_missing_requested_controller();
}
