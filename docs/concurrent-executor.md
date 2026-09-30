# Concurrent Executor and Backpressure

`ConcurrentExecutor` runs existing `run_process()` work through a fixed-size worker pool. Its
constructor takes a positive `worker_count` and a positive `queue_capacity`.

## Submission and queueing

`submit(ProcessSpec, ProcessExecutionOptions)` is thread-safe and non-blocking. It returns one
of these statuses:

- `accepted`: ownership of the task has transferred to the executor and a `TaskHandle` is
  returned.
- `queue_full`: no task was accepted and no handle is returned.
- `shutting_down`: no task was accepted and no handle is returned.

The waiting queue is FIFO and bounded. Its capacity counts only tasks still waiting in the queue;
tasks already dequeued by workers, including running tasks, do not consume a queue slot. There is
no secondary pending queue. Concurrent submitters serialize the accepting check and enqueue under
the same queue mutex, so the queue never exceeds its configured capacity.

An accepted task remains owned by the executor until it has completed, even if every caller drops
its `TaskHandle`. Task IDs are monotonically generated 64-bit values for the current process.
`TaskHandle` exposes the ID, current `TaskState`, and a blocking `get()` for the `ProcessResult`.
It does not expose the mutable state machine.

## Lifecycle and cancellation

A worker maps an accepted task through `queued -> ready -> starting -> running` before it enters
`run_process()`. A normal exit code of zero maps to `success`; nonzero exits, signals, startup
failures, and parent errors map to `failed`; process timeout maps to `timeout`; and cancellation
maps to `cancelled`.

If a task's stop token is already requested when a worker dequeues it, the worker transitions it
directly from `queued` to `cancelled` and does not call `run_process()`. Cancellation observed at
`ready` or `starting` is handled equivalently. Once `running`, cancellation is delegated to the
existing ProcessExecutor. The executor does not scan or remove waiting tasks: a cancelled queued
task can continue occupying a queue slot until a worker dequeues it.

## Shutdown

`shutdown()` performs drain shutdown. It first rejects all future submissions, then workers finish
their current work and drain every accepted waiting task before exiting. It joins workers without
holding the queue mutex. It is idempotent, and the destructor calls it safely.

## Limitations

This phase intentionally has no blocking submit, priority scheduling, resource-aware admission,
CPU or memory reservations, cgroup admission, retry, DAG execution, persistent task registry,
dynamic worker resizing, work stealing, or distributed execution.
