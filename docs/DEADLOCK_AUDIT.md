# Deadlock, lock-reentrancy and shutdown audit

This document records a deliberate inspection of the concurrency design, made by
reading the code rather than by relying on tests to find the problems. Every
item in the required audit list is addressed explicitly.

## Lock inventory

| Lock | Owner | Guards | Held across I/O | Held across callbacks | Recursive |
| --- | --- | --- | --- | --- | --- |
| `Fabric::mutex_` | `ncf::Fabric` | policy, topology, domains, publishers, fences, evidence window, intents, explanations, stats | yes (durable commit) | no | no |
| `CoordinatorNode::registry_mutex_` | `ncf::CoordinatorNode` | live channel registry, connection-thread table, last accept error | no | no | no |
| `CoordinatorNode::report_mutex_` | `ncf::CoordinatorNode` | the JSONL report file | yes (fprintf) | no | no |
| `ChildProcess::Impl::mutex` | `ncf::ChildProcess` | captured stdout/stderr buffers | no | no | no |

There is exactly one lock per object. No lock is ever taken while another lock of
the same object is held, and no two locks from different objects are ever held at
the same time, so there is no lock ordering to get wrong and no cycle to form.

## Recorded findings and resolutions

### 1. Read-lock to write-lock re-entry on the same lock

**Not present.** There is no reader/writer lock anywhere in the runtime.
`Fabric::mutex_` is a plain `std::mutex`; every access, read or write, takes it
exclusively. There is therefore no upgrade path that can self-deadlock.

### 2. Write lock held across callbacks or code that reacquires the lock

**Found and fixed.** `Fabric::unregister_publisher` took `mutex_` and then called
the public `Fabric::fence_publisher`, which takes `mutex_` again. On a
non-recursive mutex that is an immediate self-deadlock whenever the unregister
path also fences. Fixed by extracting `fence_publisher_locked`, which documents
that it requires the lock to be held, and having the public entry points take the
lock exactly once. The same helper is used by `advance_epoch`, which already held
the lock.

### 3. Mutex re-entry through callbacks

**Not present.** No user-supplied callback is ever invoked while a lock is held.
The evaluation path copies its inputs, runs the pure evaluator, and only then
re-enters the lock to commit. `InterventionPlanner::plan` is a pure function of
its arguments. Nothing in the runtime can call back into `Fabric` from inside a
locked region.

### 4. Event emission while internal locks are held

**Not present.** The coordinator writes report lines under `report_mutex_` only,
never under the fabric lock and never under `registry_mutex_`. Report writing
happens after the fabric call has returned and released its lock.

### 5. Worker shutdown while holding locks workers need

**Not present.** `CoordinatorNode::stop` and `run` cancel pending channel I/O and
join worker threads with **no** lock held. `reap_finished_threads` collects
finished threads under `registry_mutex_`, releases the lock, and only then joins.

### 6. Joining a thread while holding state required by that thread

**Found and designed out.** Connection threads call `remove_connection`, which
takes `registry_mutex_`. The accept loop reaps them by taking `registry_mutex_`
briefly to move finished threads out of the table, releasing the lock, and
joining afterwards. A worker therefore never needs a lock that the joiner is
holding. Each worker sets its completion flag **after** its last registry access,
so a `true` flag guarantees the worker can no longer contend for the lock.

### 7. Reversed lock ordering on cancellation paths

**Not present.** `cancel_all_connections` takes a snapshot of the channel
pointers under `registry_mutex_`, releases the lock, and only then calls
`cancel_pending()` on each channel. A channel's cancellation wakes a connection
thread that immediately wants `registry_mutex_`, so cancelling while holding the
lock would be a re-entry hazard. It is deliberately not done.

### 8. Progress callbacks that re-enter mutable state

**Not present.** There are no progress callbacks. The only periodic activity is
the coordinator evaluation sweep, which calls the ordinary public fabric entry
points and holds no lock of its own.

### 9. Shutdown paths that prevent work from completing while waiting for it

**Not present.** Shutdown does not block new work behind a lock the workers need:
it sets an atomic stop flag (which the listener poll observes within one poll
interval), cancels pending reads so blocked workers wake, then joins. Workers
observe the stop flag at every loop iteration and exit promptly. A worker
currently inside a fabric call finishes that call and then exits; the fabric lock
is not held by the joiner.

### 10. Nested resource acquisition with inconsistent global ordering

**Not present.** The only nested acquisition in the system is
`ChildProcess::stop_capture`, which joins reader threads that take only
`ChildProcess::Impl::mutex` to append to their buffers, while `stop_capture`
itself holds no lock. The fabric mutex and the child-process mutex are never held
simultaneously.

## Shutdown and cancellation contract

* Cancellation is real. `Fabric::evaluate_cancellable` and
  `plan_cancellable` check the token before the evaluation and again at the
  commit boundary. A cancelled call returns `ErrCode::Cancelled` and mutates no
  authoritative state; a cancelled evaluation is proven not to increment the
  evaluation counter or create a domain state (see
  `fabric_cancellation_is_real` and
  `concurrency_cancellation_is_observed_from_another_thread`).
* `Fabric::close` stops accepting work, flushes the journal and is idempotent.
  Every mutating entry point returns `ErrCode::Closed` afterwards.
* `CoordinatorNode::stop` cancels pending channel I/O, joins every worker,
  closes the report file and closes the fabric. It is safe to call twice and it
  is never called concurrently from two threads by the shipped tools.
* A surviving worker cannot resurrect a completed shutdown: the stop flag is
  checked at the top of every loop and the listener reports `Closed` once its
  handle is released.
