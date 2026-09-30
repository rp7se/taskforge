cmake_minimum_required(VERSION 3.20)

project(taskforge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_library(taskforge_lib
    src/taskforge.cpp
)

target_include_directories(taskforge_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(taskforge_lib PUBLIC cxx_std_20)

add_executable(taskforge src/main.cpp)
target_link_libraries(taskforge PRIVATE taskforge_lib)

if(BUILD_TESTING)
    add_executable(taskforge_smoke_test tests/smoke_test.cpp)
    target_link_libraries(taskforge_smoke_test PRIVATE taskforge_lib)

    add_test(NAME taskforge.smoke COMMAND taskforge_smoke_test)
endif()
