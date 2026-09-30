cmake_minimum_required(VERSION 3.20)

project(taskforge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_library(taskforge_lib
    src/taskforge.cpp
    src/process_executor.cpp
    src/task_state.cpp
)

target_include_directories(taskforge_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(taskforge_lib PUBLIC cxx_std_20)
target_compile_options(taskforge_lib PRIVATE -Wall -Wextra -Wpedantic)

add_executable(taskforge src/main.cpp)
target_link_libraries(taskforge PRIVATE taskforge_lib)
target_compile_options(taskforge PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    find_package(Threads REQUIRED)

    add_executable(taskforge_smoke_test tests/smoke_test.cpp)
    target_link_libraries(taskforge_smoke_test PRIVATE taskforge_lib)
    target_compile_options(taskforge_smoke_test PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(taskforge_process_test_helper tests/process_test_helper.cpp)
    target_compile_options(taskforge_process_test_helper PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(taskforge_process_executor_test tests/process_executor_test.cpp)
    target_link_libraries(taskforge_process_executor_test PRIVATE taskforge_lib)
    target_compile_definitions(taskforge_process_executor_test
        PRIVATE TASKFORGE_PROCESS_TEST_HELPER="$<TARGET_FILE:taskforge_process_test_helper>")
    target_compile_options(taskforge_process_executor_test PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(taskforge_task_state_test tests/task_state_test.cpp)
    target_link_libraries(taskforge_task_state_test PRIVATE taskforge_lib Threads::Threads)
    target_compile_options(taskforge_task_state_test PRIVATE -Wall -Wextra -Wpedantic)

    add_test(NAME taskforge.smoke COMMAND taskforge_smoke_test)
    add_test(NAME taskforge.process_executor COMMAND taskforge_process_executor_test)
    add_test(NAME taskforge.task_state COMMAND taskforge_task_state_test)
endif()
