# Sanitizer hardening

TaskForge maintains two optional, mutually exclusive sanitizer configurations.
They are off by default and do not change normal Debug or Release builds.

## ASan and UBSan

AddressSanitizer (ASan) detects memory-safety defects such as use-after-free,
buffer overflows, invalid frees, and leaks. UndefinedBehaviorSanitizer (UBSan)
detects runtime undefined behavior such as invalid shifts and signed overflow.
Configure, build, and run every non-privileged registered test with:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DTASKFORGE_ENABLE_ASAN_UBSAN=ON
cmake --build build-asan --parallel
ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build-asan --output-on-failure \
  -E '^taskforge.cgroup_v2_integration$'
```

## TSan

ThreadSanitizer (TSan) detects data races in the parent process's shared
state. It is a separate build because TSan must never be linked with ASan.
The focused concurrent-state tests are:

```sh
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DTASKFORGE_ENABLE_TSAN=ON
cmake --build build-tsan --parallel
TSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir build-tsan --output-on-failure \
  -R '^(taskforge.task_state|taskforge.concurrent_executor)$'
```

The CMake configuration rejects enabling both
`TASKFORGE_ENABLE_ASAN_UBSAN` and `TASKFORGE_ENABLE_TSAN`. Sanitizer modes
require a GNU or Clang compiler with the selected sanitizer flag. On Linux, the
TSan configuration also builds non-PIE executables to avoid sanitizer runtime
shadow-memory mapping conflicts.

## Cgroup and fork/exec scope

The privileged cgroup v2 kernel integration test deliberately remains in the
normal CI build. Sanitizer jobs run only non-privileged tests; they do not add a
sudo-based cgroup gate.

TaskForge uses multi-threading together with fork and exec. Sanitizer
instrumentation can add runtime hooks to the post-fork child path, so a passing
ASan/UBSan or TSan build is not a proof of post-fork async-signal-safety and
does not alter that design conclusion. A sanitizer-runtime-specific failure
must be distinguished from a TaskForge defect before changing code or adding
any suppression.

## WSL TSan runtime limitation

On the current WSL2 environment, GCC's libtsan can intermittently terminate
during runtime initialization with `ThreadSanitizer: unexpected memory
mapping`, including in the minimal TaskState test that does not use
ProcessExecutor or fork. The TSan executables are built as non-PIE and the
failure contains no TaskForge production stack or data-race report. Therefore
local WSL TSan is not the authoritative race gate. The GitHub Actions Linux
TSan job runs the TaskState and ConcurrentExecutor tests with
`TSAN_OPTIONS=halt_on_error=1` and is the required authoritative gate.

## When a sanitizer reports a defect

Stop the hardening transaction: record the exact test, sanitizer error, stack,
source location, and minimal reproduction. Do not hide reports by disabling
halting, leak detection, or adding suppressions. Production fixes belong in a
dedicated sanitizer-fix transaction.
