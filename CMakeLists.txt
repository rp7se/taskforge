cmake_minimum_required(VERSION 3.20)

project(taskforge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TASKFORGE_ENABLE_ASAN_UBSAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(TASKFORGE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(TASKFORGE_ENABLE_ASAN_UBSAN AND TASKFORGE_ENABLE_TSAN)
    message(FATAL_ERROR
        "TASKFORGE_ENABLE_ASAN_UBSAN and TASKFORGE_ENABLE_TSAN cannot both be ON")
endif()

add_library(taskforge_sanitizers INTERFACE)

if(TASKFORGE_ENABLE_ASAN_UBSAN OR TASKFORGE_ENABLE_TSAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR
            "TaskForge sanitizers require a GNU or Clang C++ compiler; found ${CMAKE_CXX_COMPILER_ID}")
    endif()

    include(CheckCXXCompilerFlag)

    if(TASKFORGE_ENABLE_ASAN_UBSAN)
        set(_taskforge_saved_required_link_options "${CMAKE_REQUIRED_LINK_OPTIONS}")
        set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
        check_cxx_compiler_flag("-fsanitize=address,undefined"
            TASKFORGE_COMPILER_SUPPORTS_ASAN_UBSAN)
        set(CMAKE_REQUIRED_LINK_OPTIONS "${_taskforge_saved_required_link_options}")
        if(NOT TASKFORGE_COMPILER_SUPPORTS_ASAN_UBSAN)
            message(FATAL_ERROR
                "TASKFORGE_ENABLE_ASAN_UBSAN requires compiler support for -fsanitize=address,undefined")
        endif()

        target_compile_options(taskforge_sanitizers INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all)
        target_link_options(taskforge_sanitizers INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all)
    endif()

    if(TASKFORGE_ENABLE_TSAN)
        set(_taskforge_saved_required_link_options "${CMAKE_REQUIRED_LINK_OPTIONS}")
        set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=thread")
        check_cxx_compiler_flag("-fsanitize=thread" TASKFORGE_COMPILER_SUPPORTS_TSAN)
        set(CMAKE_REQUIRED_LINK_OPTIONS "${_taskforge_saved_required_link_options}")
        if(NOT TASKFORGE_COMPILER_SUPPORTS_TSAN)
            message(FATAL_ERROR
                "TASKFORGE_ENABLE_TSAN requires compiler support for -fsanitize=thread")
        endif()

        target_compile_options(taskforge_sanitizers INTERFACE
            -fsanitize=thread
            -fno-omit-frame-pointer
            -fno-pie)
        target_link_options(taskforge_sanitizers INTERFACE
            -fsanitize=thread
            -fno-omit-frame-pointer
            -no-pie)
    endif()
endif()

include(CTest)
find_package(Threads REQUIRED)

add_library(taskforge_lib
    src/taskforge.cpp
    src/process_executor.cpp
    src/cgroup_v2.cpp
    src/task_state.cpp
    src/concurrent_executor.cpp
)

target_include_directories(taskforge_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(taskforge_lib PUBLIC cxx_std_20)
target_link_libraries(taskforge_lib PUBLIC Threads::Threads taskforge_sanitizers)
target_compile_options(taskforge_lib PRIVATE -Wall -Wextra -Wpedantic)

add_executable(taskforge src/main.cpp)
target_link_libraries(taskforge PRIVATE taskforge_lib)
target_compile_options(taskforge PRIVATE -Wall -Wextra -Wpedantic)

add_executable(taskforge_process_test_helper tests/process_test_helper.cpp)
target_link_libraries(taskforge_process_test_helper PRIVATE taskforge_sanitizers)
target_compile_options(taskforge_process_test_helper PRIVATE -Wall -Wextra -Wpedantic)

add_executable(taskforge_bench bench/taskforge_bench.cpp)
target_link_libraries(taskforge_bench PRIVATE taskforge_lib Threads::Threads)
target_compile_definitions(taskforge_bench
    PRIVATE TASKFORGE_PROCESS_TEST_HELPER="$<TARGET_FILE:taskforge_process_test_helper>")
target_compile_options(taskforge_bench PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_executable(taskforge_smoke_test tests/smoke_test.cpp)
    target_link_libraries(taskforge_smoke_test PRIVATE taskforge_lib)
    target_compile_options(taskforge_smoke_test PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(taskforge_process_executor_test tests/process_executor_test.cpp)
    target_link_libraries(taskforge_process_executor_test PRIVATE taskforge_lib)
    target_compile_definitions(taskforge_process_executor_test
        PRIVATE TASKFORGE_PROCESS_TEST_HELPER="$<TARGET_FILE:taskforge_process_test_helper>")
    target_compile_options(taskforge_process_executor_test PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(taskforge_task_state_test tests/task_state_test.cpp)
    target_link_libraries(taskforge_task_state_test PRIVATE taskforge_lib Threads::Threads)
    target_compile_options(taskforge_task_state_test PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(taskforge_concurrent_executor_test tests/concurrent_executor_test.cpp)
    target_link_libraries(taskforge_concurrent_executor_test PRIVATE taskforge_lib Threads::Threads)
    target_compile_definitions(taskforge_concurrent_executor_test
        PRIVATE TASKFORGE_PROCESS_TEST_HELPER="$<TARGET_FILE:taskforge_process_test_helper>")
    target_compile_options(taskforge_concurrent_executor_test PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(taskforge_cgroup_v2_test tests/cgroup_v2_test.cpp)
    target_link_libraries(taskforge_cgroup_v2_test PRIVATE taskforge_lib)
    target_compile_options(taskforge_cgroup_v2_test PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(taskforge_cgroup_v2_integration_test tests/cgroup_v2_integration_test.cpp)
    target_link_libraries(taskforge_cgroup_v2_integration_test PRIVATE taskforge_lib Threads::Threads)
    target_compile_definitions(taskforge_cgroup_v2_integration_test
        PRIVATE TASKFORGE_PROCESS_TEST_HELPER="$<TARGET_FILE:taskforge_process_test_helper>")
    target_compile_options(taskforge_cgroup_v2_integration_test PRIVATE -Wall -Wextra -Wpedantic)

    add_test(NAME taskforge.smoke COMMAND taskforge_smoke_test)
    add_test(NAME taskforge.process_executor COMMAND taskforge_process_executor_test)
    add_test(NAME taskforge.task_state COMMAND taskforge_task_state_test)
    add_test(NAME taskforge.concurrent_executor COMMAND taskforge_concurrent_executor_test)
    add_test(NAME taskforge.cgroup_v2 COMMAND taskforge_cgroup_v2_test)
    add_test(NAME taskforge.cgroup_v2_integration COMMAND taskforge_cgroup_v2_integration_test)
    add_test(NAME taskforge.benchmark_launch_smoke
             COMMAND taskforge_bench --scenario launch --warmup 1 --iterations 3 --format json)
    add_test(NAME taskforge.benchmark_resource_smoke
             COMMAND taskforge_bench --scenario resource --tasks 4 --format json)
    set_tests_properties(taskforge.cgroup_v2_integration PROPERTIES SKIP_RETURN_CODE 77)
endif()
