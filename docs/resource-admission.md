# Resource-Aware Admission

Phase 7 adds optional, explicit user-space resource admission to
`ConcurrentExecutor`. It is configured, not discovered: Taskforge does not
sample host CPUs, free memory, `/proc/meminfo`, load average, or any other
dynamic host resource signal.

```cpp
ConcurrentExecutor executor({
    .worker_count = 2,
    .queue_capacity = 16,
    .resource_capacity = ResourceCapacity{
        .cpu_millis = 2000,
        .memory_bytes = 256 * 1024 * 1024,
    },
});

auto submitted = executor.submit(spec, options, ResourceRequest{
    .cpu_millis = 500,
    .memory_bytes = 128 * 1024 * 1024,
});
```

`cpu_millis` is a logical configured admission budget: 1000 means one logical
CPU worth of this executor's budget. It is not host utilization, `cpu.weight`,
or a CPU priority. `memory_bytes` is likewise a declared reservation budget.
Both configured capacity dimensions and every managed request must be greater
than zero. A request larger than either total capacity is rejected before it is
enqueued with `resource_request_exceeds_capacity`; a zero-valued dimension is
rejected with `invalid_resource_request`.

## Admission, queueing, and release

With resource admission enabled, every submission must supply a
`ResourceRequest`; the legacy overload is rejected with
`resource_request_required`. With admission disabled, the existing
`ConcurrentExecutor(worker_count, queue_capacity)` API retains its Phase 5
behavior.

`accepted` means the task has entered the one bounded FIFO waiting queue and
is owned by the executor. It does not mean it has been resource-admitted. A
resource-blocked accepted task remains `queued` and continues to occupy that
same bounded queue slot; there is no second resource-waiting queue.

Under the queue mutex, a worker atomically checks the queue front against both
available dimensions, reserves CPU and memory together, and pops that task.
It never holds this mutex while changing task state or running the process.
Release is exception-safe and happens exactly once before the task completion
promise becomes ready, for every process outcome including failures, timeouts,
and cancellation. A release notifies waiting workers.

Admission is strict FIFO. Only the front task is considered. If it does not
fit, workers wait on a condition variable; they do not scan for a smaller later
task. This deterministic no-backfill policy prevents starvation from bypassing
the head, but can temporarily leave capacity unused (head-of-line blocking).

Drain shutdown first stops acceptance, then retains all accepted,
resource-blocked work. Workers wait for running tasks to release reservations
and continue FIFO admission until the bounded queue drains. The submit-time
capacity check guarantees an accepted queue head can eventually fit total
capacity.

`resource_snapshot()` provides a consistent read-only capacity/reserved/
available snapshot for diagnostics and tests when admission is enabled.

## Relationship to cgroup v2

Admission reservations decide whether a task may start now. `CgroupV2Limits`
in `ProcessExecutionOptions::cgroup` decide what the kernel enforces after a
task starts. They are intentionally orthogonal. For example,
`ResourceRequest{500, 128MiB}` accounts for executor reservation, while
`cpu.max` and `memory.max` in `CgroupV2Limits` are kernel controls. Callers
should choose values that are sensible for their policy, but Phase 7 does not
convert requests to cgroup limits or overwrite caller-provided limits.

This phase deliberately adds no priority scheduling, preemption, backfill,
overcommit, retries, DAGs, host-resource sampling, or distributed scheduling.
