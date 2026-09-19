# Network Congestion Fabric

Open-source, vendor-neutral C++20 runtime for generation-bound network-wide
congestion state, intervention authority, propagation, and recovery intent across
governed fabric resources.

**Version 1.0.0.** Everything described here is implemented and covered by the
test suite in this repository. Nothing here is planned, partial or aspirational.

## The question this runtime answers

Given authoritative queue, buffer, rate, path, capacity, loss, latency,
utilization and topology evidence: what congestion state exists now across the
fabric, where is it propagating, which interventions are authorized, and when
must that state or intervention be demoted, fenced, revalidated or retired as
stale?

## Systems boundary

Network Congestion Fabric owns **network-wide congestion state and governed
control intent**. It emits bounded, authority-checked, evidence-bound *requests*
toward adjacent runtimes that own enforcement. It never performs the action
itself.

**Owned here**

* congestion state for a governed domain, bound to an exact evidence vector;
* the severity ladder and its explicit hysteresis;
* intervention authority and the suppression reasons for refused intent;
* propagation reasoning across links and paths;
* recovery intent and eligibility;
* freshness, fencing, epoch and generation semantics;
* durable policy, congestion history, intervention lineage, provenance, fences
  and epoch.

**Deliberately NOT owned here**

queue implementation, buffer allocation, per-flow rate enforcement, path
computation or path legality, traffic admission, traffic-engineering allocation,
microburst detection, elephant-flow governance, incast mitigation, hotspot
governance, pacing, backpressure propagation, congestion recovery sequencing, and
physical switch or NIC programming. Topology hop membership is validated, but
path adjacency and path legality are *not* re-derived, because that is the path
runtime's job.

## State model

```
Severity:  CLEAR  <  WATCH  <  CONGESTED  <  SEVERE
State:     CLEAR | WATCH | CONGESTED | SEVERE | RECOVERING   (authoritative)
           UNKNOWN | STALE | CONFLICT                        (non-authoritative)
```

Four rules follow directly from the model and are enforced by the evaluator:

* **High utilization is not automatically congestion.** A single metric can raise
  a domain to WATCH, which carries no corrective intervention. CONGESTED and
  SEVERE require corroboration: by default at least two distinct metric rules
  must independently classify at or above the level
  (`min_agreeing_rules_for_congested`, `min_agreeing_rules_for_severe`).
* **Queue occupancy and loss are not automatically congestion** for the same
  reason: each is one rule among several, subject to the same gate.
* **Congestion state is not intervention.** A state authorizes intent; it does
  not perform it. Each intent carries the exact evidence snapshot, evaluation,
  epoch, authority generation and policy fingerprint that justified it.
* **Intervention intent is not applied effect.** The fabric has no notion of
  whether an adjacent runtime honoured a request, and never claims one.

## Invariants

* No intervention relies on stale evidence. Every authorization is gated on
  `evidence_within_freshness` at the planning tick.
* UNKNOWN never authorizes corrective action. UNKNOWN, STALE and CONFLICT are
  non-authoritative and suppress every corrective intent with a named reason.
* Severity transitions are deterministic. The evaluator is a pure function of
  `(request, prior state, context)`; replaying the same inputs reproduces the
  same state sequence, proven by property tests.
* Hysteresis is explicit. Escalation and de-escalation sample counts, minimum
  dwell, the downgrade margin and the recovery requirements live only in the
  policy; no implicit smoothing exists anywhere else.
* Recovery cannot begin from one improved sample unless the policy explicitly
  permits it (`recovery.allow_single_sample`, off by default). Leaving an
  indeterminate state after a previously congested level is treated as a recovery
  and obeys the same requirement.
* Intervention generations are invalidated by authority change until revalidated.
  A policy, topology or epoch change clears outstanding intent and flags affected
  domains as requiring revalidation.
* Contradictory evidence is never silently merged. Disagreement beyond the
  configured tolerance produces CONFLICT (the default) or a *reported*,
  explicitly resolved selection; the disagreement is always listed.
* Missing evidence never becomes positive authority. Incomplete required coverage
  floors the domain at WATCH in both directions, so it can never be reported as
  healthy.

## Architecture

```
include/ncf/core/        strong identities, checked arithmetic, CRC-32C, Result,
                         logical time, cancellation, hard bounds
include/ncf/model/       metrics, evidence, topology, policy, authority, state,
                         intervention, explanation, fence
include/ncf/eval/        deterministic evaluator, hysteresis engine,
                         propagation engine, intervention planner
include/ncf/durability/  binary codec, CRC-framed journal, atomic snapshot,
                         crash-safe store
include/ncf/transport/   framed protocol, streaming decoder, sequence window,
                         control payload codecs
include/ncf/ipc/         real named-pipe endpoints, real child processes
include/ncf/fabric/      the coordinator: ingest, evaluate, plan, fence, epoch
include/ncf/runtime/     coordinator node and publisher client
include/ncf/cli/         strict argument parsing
```

Evaluation is a pure function. `Fabric` is the only mutable component and holds
exactly one mutex, held across no callbacks.

## Durability

Durable state lives in a directory containing `ncf.journal` and `ncf.snapshot`
(created on first checkpoint).

Commit protocol for every mutation:

1. validate the payload against the hard bounds;
2. bind the mutation to the current epoch and authority generation;
3. append an **Intent** record and flush to stable storage;
4. append the **Payload** record and flush;
5. append a **Commit** record carrying the payload CRC and flush;
6. only now is the mutation applied in memory and reported as durable.

A crash between (4) and (5) leaves an *unfinished attempt*. Replay reports it, and
it is never applied; the ambiguity is written into the recovered document so the
next boot still knows about it.

Other durability properties, all covered by tests:

* every record header and payload is CRC-32C checked, and a record that fails is
  refused rather than interpreted;
* a torn tail is reported as `TruncatedTail` with the exact byte count, is cut
  on the next writable open, and is never mistaken for a complete journal;
* `Store::checkpoint` writes a new snapshot, reads it back, applies every record
  into a scratch document and only then rebases the journal — a compaction that
  cannot be recovered is refused before the journal is discarded;
* a snapshot without a valid footer is refused in full;
* restart advances the coordinator epoch, demotes every restored authoritative
  state to STALE, and marks it as requiring revalidation. Telemetry freshness,
  publisher authority, leases and hardware effect are **never** restored as
  current;
* fences are durable and survive restart, so a fenced incarnation cannot come back
  to life by reconnecting;
* a store opened read-only never truncates or rebases anything.

## Distributed operation

The runtime proves distributed behaviour with real operating-system processes and
real framed transport, not with in-process simulation:

* `ncf_node coordinator` creates a real named-pipe endpoint (Windows) or
  `AF_UNIX` stream socket (POSIX) and serves one thread per connection;
* `ncf_node publisher` is a separate process that connects, handshakes, streams
  framed evidence and reads acknowledgements;
* every frame carries a version, type, flags, payload length, header CRC, payload
  CRC, epoch, boot, publisher, connection, sequence and nonce;
* the streaming decoder is fail-stop: a stream that desynchronises is closed, not
  guessed at;
* a peer that connects and vanishes before the connection completes is reported
  as a benign, retryable event and the listener replaces the spent instance, so
  one abandoned connection cannot wedge the endpoint;
* an unregistered or non-handshaked stream is refused, and a malformed stream is
  counted and closed without affecting any other connection.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Requirements: CMake 3.20 or newer and a C++20 compiler. Verified on Windows with
MSVC 19.44 (Visual Studio 2022, x64) at `/W4 /WX /permissive-`. The build is
clean at `/W4 /WX` in both Release and Debug.

Options: `NCF_BUILD_TESTS`, `NCF_BUILD_TOOLS`, `NCF_BUILD_BENCH`,
`NCF_WARNINGS_AS_ERRORS`, `NCF_ENABLE_ASAN`, `NCF_ENABLE_STATIC_ANALYSIS`.

## Install and consume

```
cmake --install build --config Release --prefix <prefix>
cmake -S examples/consumer -B consumer-build -DCMAKE_PREFIX_PATH=<prefix>
cmake --build consumer-build --config Release
```

`examples/consumer` is an independent project that consumes the exported
`ncf::ncf` target through `find_package(ncf 1.0 CONFIG REQUIRED)`. It prints
the resulting state, the authorized interventions and the full explanation, and
exits zero only if the state and the plan are what the recorded evidence implies.

## Tools

```
ncf_node coordinator --endpoint NAME --state-dir DIR --report FILE \
                     [--resources N] [--paths N] [--eval-interval-ms N] \
                     [--stop-file PATH] [--run-ms N] [--deny CAPABILITY,...]
ncf_node publisher   --endpoint NAME --publisher HEX --boot HEX --domain HEX \
                     --resources a,b,c --samples N [--interval-ms N] [...]
ncf_node selfcheck
ncf_ctl inspect --state-dir DIR
ncf_ctl verify  --state-dir DIR
ncf_ctl version
ncf_bench [--evaluations N] [--out FILE]
```

`ncf_ctl verify` exits non-zero on any integrity failure;
`ncf_ctl inspect` prints the durable document, including per-domain
`requires_revalidation` flags, fences, history and intervention lineage.

## Tests

```
cmake --build build --config Release
build/tests/Release/ncf_tests.exe            # no timeouts anywhere
build/tests/Release/ncf_tests.exe --list
build/tests/Release/ncf_tests.exe --filter=evaluator
```

The suite contains 146 tests: unit, integration, end-to-end, property, seeded
randomized, adversarial, concurrency/race and real multiprocess tests. No test
sets or relies on a timeout; a hanging test is a defect to diagnose.

The multiprocess tests spawn `ncf_node` as real child processes over a real
named-pipe endpoint, hard-kill the coordinator, restart it against the same
durable directory, and assert that the epoch advanced, that restored state is
STALE and flagged for revalidation, and that a well-behaved publisher is still
served after malformed traffic.

## Benchmark

```
build/Release/ncf_bench.exe --evaluations 300 --out docs/benchmark-results.csv
```

`docs/benchmark-results.csv` holds a recorded run. The benchmark measures
**completed** congestion-state evaluations — work that ran to completion and
committed a state — across resource count, metric fan-in, path count, domain
count and intervention-policy complexity. It never measures enqueue or submission
latency.

## Proof surface

**REAL** (exercised on this machine with real operating-system resources)

* named-pipe endpoints and child processes (`tests/test_ipc.cpp`,
  `tests/test_multiprocess.cpp`);
* hard process kill and restart with durable epoch advance, fence survival and
  state demotion;
* 146 passing tests in Release and Debug at `/W4 /WX`;
* AddressSanitizer run of the full suite: clean (`/fsanitize=address`);
* MSVC static analysis (`/analyze`) over library, tools and tests: no first-party
  findings;
* `cmake --install` plus an independent `find_package` consumer that builds and
  runs;
* a fresh clone of the committed tree configured, built and tested.

**SYNTHETIC**

* every benchmark number in `docs/benchmark-results.csv`;
* the evidence emitted by the `publisher` tool role, which is a scripted
  generator, not a device.

**UNSUPPORTED in 1.0.0** (stated so that no reader infers otherwise)

* no physical switch, NIC, DPU, RDMA, NVLink or optical hardware was involved at
  any point, and no hardware result is claimed anywhere;
* the POSIX `AF_UNIX` branch of `include/ncf/ipc/channel.hpp` and the POSIX
  branch of `src/ipc/process.cpp` are implemented but were **not** compiled or
  executed on this platform; only the Windows named-pipe path is validated;
* AddressSanitizer leak detection is not available for MSVC ASAN on Windows, so
  the sanitizer run covers memory errors but not leaks;
* the library is built and exported as a static library; no shared-library build
  is provided in 1.0.0;
* publisher-to-coordinator clock alignment is a handshake-derived offset
  approximation, not a synchronised clock.

## Further documentation

* `docs/DEADLOCK_AUDIT.md` — the lock inventory, the ten required audit items and
  the one re-entrancy defect that audit found and fixed.
* `docs/LIMITS.md` — every hard bound.
* `docs/benchmark-results.csv` — a recorded synthetic benchmark run.
* `examples/consumer/` — the downstream `find_package` consumer.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
