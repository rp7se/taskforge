# Taskforge

Taskforge is a C++20 project built with CMake.

## Build and test

```sh
cmake -S . -B build
cmake --build build
./build/taskforge
ctest --test-dir build --output-on-failure
```
