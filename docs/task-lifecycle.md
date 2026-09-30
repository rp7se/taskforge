# Task lifecycle

Phase 2 provides a thread-safe state machine for one task lifecycle. A new
`TaskStateMachine` always starts in `queued`.

```
queued -> ready -> starting -> running -> success
                         |             `-> failed
                         `-> failed
```

`success` and `failed` are terminal states and have no outgoing transitions.
All other state changes are rejected unless they match the graph exactly.

`try_transition(expected, desired)` first validates the graph edge and then
uses an atomic compare-and-set operation. It returns `applied` for the single
successful contender, `invalid_transition` for an edge outside the graph, and
`state_mismatch` when a valid edge loses a race because the observed state no
longer matches `expected`.

This phase does not implement TIMEOUT or CANCELLED states. Those extensions
remain future work.
