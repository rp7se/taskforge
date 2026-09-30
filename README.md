# Taskforge

Taskforge is a C++20 project built with CMake.

## Current status

- Phase 1: Linux Process Executor
- Phase 2: thread-safe lifecycle state transitions
- Phase 3: timeout and C++20 stop-token cancellation for direct child processes

## Build and test

```sh
cmake -S . -B build
cmake --build build
./build/taskforge
ctest --test-dir build --output-on-failure
```
