# Taskforge

Taskforge is a C++20 project built with CMake.

## Current status

- Phase 1: Linux Process Executor
- Phase 2: thread-safe lifecycle state transitions

## Build and test

```sh
cmake -S . -B build
cmake --build build
./build/taskforge
ctest --test-dir build --output-on-failure
```
