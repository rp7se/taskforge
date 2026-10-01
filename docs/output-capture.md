# Bounded Output Capture

`run_process()` always drains child stdout and stderr pipes. Without a capture
bound, retaining an unbounded child log in `ProcessResult` can grow TaskForge's
parent-process memory indefinitely. Phase 8 adds an optional in-memory payload
bound without changing pipe draining.

```cpp
ProcessExecutionOptions options{
    .output_capture_limits = OutputCaptureLimits{
        .stdout_bytes = 64 * 1024,
        .stderr_bytes = 16 * 1024,
    },
};
```

`output_capture_limits == nullopt` is the legacy mode and retains all output.
When limits are supplied, stdout and stderr are independent limits. A zero
limit retains no payload for that stream, but it still drains every byte,
counts it, and marks the stream truncated if any byte was produced.

## Capture and accounting semantics

Capture uses prefix semantics: a bounded stream retains its first N bytes, not
a ring buffer or the last N bytes. `stdout_data.size()` and `stderr_data.size()`
never exceed their configured limits. `stdout_total_bytes` and
`stderr_total_bytes` count every successful pipe read, including bytes discarded
after the retained prefix reaches its limit. The counters use saturating
`uint64_t` accounting and never wrap.

`stdout_truncated` or `stderr_truncated` is set only when that stream produced
more bytes than its retained bound. Truncation is diagnostic only: it neither
kills a child nor changes its process outcome, exit code, timeout, cancellation,
or process-tree cleanup behavior.

Reaching a capture limit never closes or stops polling a pipe. TaskForge keeps
reading until the existing execution lifecycle closes the stream, preventing a
child from blocking on a full stdout or stderr pipe. Capture is byte-oriented:
binary bytes, including NUL, are retained and counted exactly.

## cgroup relationship and memory claim

Output capture is separate from cgroup v2 enforcement. A child cgroup's
`memory.max` does not limit TaskForge's parent-side retained logs, and output
limits do not alter `cpu.max`, `memory.max`, cgroup attach ordering, or startup
diagnostics.

Configured limits bound retained in-memory stdout/stderr payload per stream.
They are not a hard limit on total TaskForge process memory: implementation
buffers, task metadata, and other allocations remain outside this model.

This phase intentionally adds no log files, rotation, compression, remote
upload, tail API, callbacks, persistent logs, DAGs, retries, or scheduling.
