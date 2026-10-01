# cgroup v2 resource control

Phase 6 adds Linux cgroup v2 enforcement to `run_process`. It is deliberately
an **after-admission** mechanism: it constrains a task which has already been
accepted to run. It is not resource admission, reservation, scheduling, or a
statement about available machine CPU or memory. Those decisions belong to
Phase 7.

## Explicit delegated root

Set `ProcessExecutionOptions::cgroup` to a `CgroupV2Options` with a `root`
that has been explicitly prepared for TaskForge. TaskForge never selects
`/sys/fs/cgroup` by default and never writes an ancestor of `root`. The root
must expose requested `cpu`, `memory`, and/or `pids` controllers. TaskForge may
enable only those requested controllers in `root/cgroup.subtree_control` and
then creates a generated `taskforge-<pid>-<sequence>` child below that root.

The child receives optional `cpu.max` (`quota_us period_us`), `memory.max`, and
`pids.max` values before it is allowed to execute. Zero values are rejected by
the parent before forking. Setup failure is reported as a specific cgroup
`ProcessErrorStage`; no user program is released to execute on an attach or
limit-write failure.

## Attach-before-exec

For a cgroup-enabled execution, the fork child performs the existing
`setpgid(0, 0)` setup and blocks on a raw launch-gate pipe. The parent confirms
the Phase 4 process-group ownership, writes only that direct child PID to the
task cgroup's `cgroup.procs`, then releases the gate. Thus the user program's
first instruction after `execv` already runs in the cgroup. The child-side path
uses only async-signal-safe operations (`setpgid`, `read`, `close`, `fcntl`,
`dup2`, `write`, `execv`, and `_exit`); it does no path lookup or allocation.

Normal descendants inherit the task cgroup automatically. This is independent
of the process group: descendants that leave the task PGID can evade Phase 4
signal cleanup, but ordinarily still remain in the cgroup unless separately
moved by a privileged actor. TaskForge does not use `cgroup.kill`, a `/proc`
crawler, or a subreaper in this phase.

## Diagnostics and cleanup

After direct-child and process-group cleanup, TaskForge reads requested
controller counters into `ProcessResult::cgroup_events`: `cpu.stat`
`nr_throttled`, `memory.events` `oom_kill` and `max`, and `pids.events` `max`. These are
orthogonal to `ProcessOutcome`; for example an OOM-killed task normally remains
`signaled` while reporting `memory_oom_kill`.

It then removes the per-task cgroup. Diagnostic and removal errors are exposed
separately as `cgroup_diagnostic_error` and `cgroup_cleanup_error`, so they do
not overwrite a known exit, timeout, or cancellation result. A delegated root
is required locally; the real kernel integration test skips with return code 77
when `TASKFORGE_CGROUP_TEST_ROOT` is not supplied.
