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

After timeout or cancellation wins, Taskforge sends `SIGKILL` only to the owned
direct child PID, then waits for and reaps it. `timed_out` and `cancelled` are
high-level outcomes rather than ordinary `signaled` results; the actual signal
is retained as diagnostic data. Output already available from stdout and stderr
is drained before the remaining pipes are closed, so inherited descriptors in a
surviving descendant cannot block return.

This phase does not clean up descendants, use process groups, or implement
graceful `SIGTERM` escalation or cgroups.
