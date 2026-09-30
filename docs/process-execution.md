# Process execution control

`run_process` accepts `ProcessExecutionOptions`, containing an optional timeout
and a C++20 `std::stop_token`. Timeout uses a `std::chrono::steady_clock`
deadline computed when execution begins.

An already-requested stop token returns `cancelled` before PATH resolution or
fork. A zero or negative timeout returns `timed_out` before starting a child.

While a child is running, each monitor iteration first observes it with
`waitpid(..., WNOHANG)`. A naturally observed and reaped process keeps its
natural result. Otherwise cancellation is checked before the timeout deadline,
so simultaneous observable control requests resolve to `cancelled`.

Each successful invocation owns a dedicated Linux process group: the child
calls `setpgid(0, 0)` before `execv`, and the parent also calls
`setpgid(child_pid, child_pid)` then verifies the group to close the fork/exec
race. Ordinary descendants inherit that PGID.

After timeout or cancellation wins, Taskforge sends `SIGTERM` to the owned
process group, drains output while waiting for the configured grace period, and
escalates with group `SIGKILL` if necessary. The direct child is always waited
for and reaped. `timed_out` and `cancelled` remain high-level outcomes rather
than ordinary `signaled` results; `ProcessCleanupOutcome` independently records
whether cleanup was unnecessary, completed during grace, required SIGKILL, or
failed.

If the direct child exits naturally but descendants remain in its process
group, Taskforge applies the same cleanup while retaining the direct child's
natural exit result. Once group cleanup has completed, remaining pipe ends are
closed after available output is drained, giving bounded return even for an
unknown escaped writer.

Cleanup covers descendants that remain in the task-owned PGID. A process which
deliberately leaves it with `setsid` or another `setpgid` is not guaranteed
cleanup. Cgroup containment is not implemented and is the future stronger
ownership boundary.
