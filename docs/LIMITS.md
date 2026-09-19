# Hard bounds

Every externally influenced size, count, depth, fan-in, payload and time unit is
validated against the constants in `include/ncf/core/limits.hpp` before any
allocation or iteration. The values below are the defaults in 1.0.0.

## Domain and topology

| Bound | Value |
| --- | --- |
| Domains per fabric | 65,536 |
| Resources per fabric | 4,194,304 |
| Resources per domain | 1,048,576 (policy default 4,096) |
| Containment depth | 64 |
| Links per topology | 4,194,304 |
| Paths per topology | 1,048,576 |
| Hops per path | 4,096 |

## Evidence

| Bound | Value |
| --- | --- |
| Metric readings per sample | 64 |
| Samples per batch | 65,536 |
| Publishers | 4,096 |
| Current-evidence window entries | 65,536 (fabric option) |
| Distinct publishers compared for contradiction | 64 |

## Policy

| Bound | Value |
| --- | --- |
| Threshold rules | 128 |
| Intervention rules | 64 |
| Hysteresis sample counts | 1,048,576 |
| Dwell, repeat interval | 2^40 ticks |

## Evaluation and explanation

| Bound | Value |
| --- | --- |
| Explanation entries per section | 4,096 (policy default 1,024) |
| Explanation bytes | 1 MiB |
| Propagation edges | 1,048,576 |
| Interventions per plan | 1,024 (policy default 64) |
| Retained intervention lineage | 65,536 |

## Transport and durability

| Bound | Value |
| --- | --- |
| Frame payload | 1 MiB |
| Connections | 4,096 (coordinator default 32 threads) |
| Replay window | 4,096 sequences |
| Sequence gap | 1,048,576 |
| Journal record | 4 MiB |
| Journal file | 16 GiB |
| Snapshot file | 2 GiB |
| Retained durable history | 16,777,216 entries (store default 65,536) |

## Arithmetic

`include/ncf/core/checked.hpp` provides checked add/subtract/multiply/divide,
saturating variants, lossless narrowing and parts-per-million scaling. Every
externally influenced size, capacity, rate, counter and time unit in the runtime
is routed through these helpers; unchecked arithmetic on such a value is treated
as a defect. Property and adversarial tests exercise the overflow and truncation
paths directly.
