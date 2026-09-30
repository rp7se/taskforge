# Task lifecycle

Phase 3 provides a thread-safe state machine for one task lifecycle. A new
`TaskStateMachine` always starts in `queued`.

```
queued -> ready -> starting -> running -> success
  |        |         |          |-> failed
  |        |         |          |-> timeout
  |        |         |          `-> cancelled
  |        |         `-> failed
  |        `-> cancelled
  `-> cancelled
```

`success`, `failed`, `timeout`, and `cancelled` are terminal states and have no
outgoing transitions. Timeout is only legal from `running`; cancellation is
legal before running from `queued`, `ready`, or `starting`, as well as from
`running`.
All other state changes are rejected unless they match the graph exactly.

`try_transition(expected, desired)` first validates the graph edge and then
uses an atomic compare-and-set operation. It returns `applied` for the single
successful contender, `invalid_transition` for an edge outside the graph, and
`state_mismatch` when a valid edge loses a race because the observed state no
longer matches `expected`.

From `running`, success, failure, timeout, and cancellation may race. Exactly
one contender can receive `applied`; all other valid contenders receive
`state_mismatch`, and the winning terminal state is immutable.
