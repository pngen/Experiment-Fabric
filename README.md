# Experiment Fabric

Experiment Fabric is an open-source, vendor-neutral C++20 runtime for governing autonomous
experiments as durable systems objects: hypothesis, branch, trial, attempt, observation,
comparison, decision, rollback and lineage.

The governing systems question is:

> How should an autonomous experiment be defined, branched, executed, measured, compared,
> accepted, rejected, rolled back, and reproduced while preserving authoritative lineage and
> preventing stale or partial evidence from becoming truth?

An experiment is treated as a governed state machine, not as a directory containing scripts and
results. A valid experiment result must be supported by explicit experiment generation, branch
lineage, trial authority, metric definitions, observations, completion state and comparison
criteria. A process exit code is not an experiment result. A produced artifact is not an accepted
artifact. A metric observation is not authoritative merely because it was emitted.

Between a running trial and an authoritative result sits an experiment that the runtime must be able
to define, branch, measure, compare, accept, reject, roll back and reproduce without ever letting
stale or partial evidence become truth.

## What Experiment Fabric is not

Experiment Fabric governs the experiment itself. It does not schedule arbitrary lab resources and is
not a research notebook, workflow engine, training framework, benchmark suite, artifact registry or
agent runtime. It never marks an artifact as production trusted: an experiment result becoming
authoritative under Experiment Fabric policy does not promote its artifacts for production use.

## Exact boundary

Experiment Fabric owns:

- experiment identity and generation;
- hypothesis identity, revision and supersession;
- experiment definitions and parameters;
- branch identity, parentage, forks, retirement, invalidation and supersession;
- baseline/control and treatment/candidate branches;
- trial identity and attempt identity;
- trial and experiment lifecycle;
- experimental conditions, input-set identity and environment fingerprint;
- executor and producer identity;
- metric definitions, observations and aggregation;
- deterministic comparison against baselines or other branches;
- deterministic decision criteria and result authority;
- evidence freshness and stale-authority rejection;
- artifact references produced by trials;
- rollback to a prior authoritative experiment state;
- retry, cancellation and partial-failure semantics;
- reproducibility records;
- persistence, crash recovery and generation/epoch/boot/attempt fencing;
- deterministic explanation of why an experiment or branch reached its current status.

Experiment Fabric does not own, and does not implement:

- persistent autonomous-agent lifecycle;
- global agent scheduling;
- model discovery, global model routing, model hosting or token generation;
- ensemble orchestration;
- critic implementation or reusable critic-worker policy;
- GPU or lab-wide resource scheduling;
- dataset storage systems;
- simulator or physical test-environment scheduling;
- benchmark catalog management;
- general-purpose workflow or dependency orchestration;
- global research accounting across unrelated experiments;
- trusted artifact promotion into production;
- production deployment;
- model training frameworks or hyperparameter-optimization frameworks;
- arbitrary policy engines;
- prompt management;
- observability platforms or experiment dashboards.

Those remain adjacent concerns.

### Adjacent runtime separation

- Ensemble Fabric governs multiple models acting together as one runtime.
- Critic Fabric governs reusable evidence-backed review.
- Experiment Fabric governs the experiment lifecycle and the authoritative result.
- Lab Scheduler will govern placement and scheduling of experiments across models, GPUs, simulators,
  datasets and physical or virtual environments.
- Research Ledger will govern broader provenance and accounting across experiments, model calls,
  hypotheses, artifacts, failures and accepted results.
- Artifact Promotion will govern when outputs become trusted enough for a subsequent stage.
- Autonomous Foundry will coordinate higher-order populations of autonomous research and coding
  workers.

## Architecture

`@@`@@`@
apps/ef_coordinator      coordinator process: durable state, authority, framed TCP control plane
apps/ef_worker           worker process: claims, executes, publishes, commits; owns no authority
apps/ef_ctl              inspection and control client (deterministic text output)
apps/scenarios           reference experiment catalogue shared by tools, examples and tests
include/experiment_fabric
  identity.hpp           strongly typed identities and generation counters
  error.hpp              typed Status/Result error model
  domain.hpp             enumerations and the explicit lifecycle state machines
  model.hpp              hypothesis, metric, branch, trial, attempt, observation, decision
  state.hpp              the durable coordinator state model and its validator
  authority.hpp          the authority envelope
  canonical.hpp          canonical encodings and definition digests
  protocol.hpp           strict bounded framing and payload codecs
  transport.hpp          loopback TCP transport
  persistence.hpp        versioned, integrity-checked, transactional persistence
  coordinator.hpp        the governance core and its read-only snapshots
  worker.hpp             the worker runtime
  client.hpp             the control-plane client
  inspect.hpp            deterministic rendering of authoritative state
src/                     the implementation of the library above
examples/                eleven reference experiments
tests/                   the proof obligations, including the multi-process proofs
benchmarks/              measurements of completed operations
cuda/                    a real accelerator-backed reference trial
consumer/                an independent downstream find_package consumer
`@@`@@`@

The coordinator owns one coherent durable state and one live process authority. A single state lock
guards durable state and worker liveness together, so an authority decision is always made against
one view. Network I/O, file I/O and observer callbacks all run with no coordinator lock held. The
server uses a fixed thread pool: no thread is created per connection on demand, per request, or per
experiment.

## Experiment state model

Experiment states: `OPEN`, `FINALIZED`, `CANCELLED`, `SUPERSEDED`, `RECOVERING`.

Branch states: `ACTIVE`, `RETIRED`, `INVALID`, `SUPERSEDED`.
Branch roles: `BASELINE`, `CANDIDATE`.

Trial states: `CREATED`, `READY`, `ASSIGNED`, `RUNNING`, `OBSERVING`,
`COMPLETED`, `FAILED`, `CANCELLED`, `INVALID`, `SUPERSEDED`,
`REVALIDATION_REQUIRED`. Terminal states accept no outgoing transition except the
`SUPERSEDED` audit marker, and `REVALIDATION_REQUIRED` may only move to a conservative
state.

Attempt states: `ISSUED`, `RUNNING`, `PUBLISHED`, `COMPLETED`, `FAILED`,
`CANCELLED`, `ABANDONED`, `REVALIDATION_REQUIRED`.

Every transition is validated. Arbitrary enumeration reassignment is not reachable through any API.

## Hypothesis, branch and trial model

A hypothesis has stable identity, a revision, a statement, an optional expected direction and
metric, creation lineage, supersession state and provenance. Revising a hypothesis increments the
hypothesis revision and the experiment generation, supersedes the previous revision in a retained
history, and fences every trial derived from the earlier revision.

Branches are first class. A fork preserves lineage and merges parameters without sharing mutable
authority: the parent keeps its own branch generation, the child receives its own, and live trial
state is never shared. Branch parentage is validated on every mutation and on every load, and a cycle
or impossible parent is rejected. A retired branch cannot receive authoritative new evidence until
it is explicitly reactivated in a new generation.

A logical trial spans one or more attempts. A retry preserves the logical `TrialId` and issues
a fresh `TrialAttemptId` by default. Only one attempt may authoritatively complete a logical
trial; late results from losing, cancelled, failed, abandoned or superseded attempts are refused with
a typed stale-authority error and recorded as rejected evidence.

## Authority and generation model

Trial result publication carries a complete authority envelope: `CoordinatorEpoch`,
`ExperimentId`, `ExperimentGeneration`, `HypothesisId`, `HypothesisRevision`,
`PolicyId`, `PolicyGeneration`, `BranchId`, `BranchGeneration`, `TrialId`,
`TrialAttemptId`, `WorkerId`, `WorkerBootId` and `ProducerId`. Evidence is
never accepted because the identifiers parse.

Publication is refused for a stale coordinator epoch, a stale or revoked worker boot identity, a
stale experiment generation, a stale hypothesis revision, a stale branch or branch generation, a
stale attempt, a producer that does not match the registered incarnation, a duplicate logical
completion, publication after cancellation, publication after branch retirement, publication after
experiment closure, and publication after rollback or supersession.

Which mutations advance which counter:

| Mutation | Experiment generation | Hypothesis revision | Branch generation | Policy generation |
| --- | --- | --- | --- | --- |
| Hypothesis revision | increments | increments | unchanged | unchanged |
| Branch fork or branch parameter change | unchanged | unchanged | the new branch starts at 1 | unchanged |
| Branch retirement | unchanged | unchanged | increments for that branch only | unchanged |
| Policy change | increments | unchanged | unchanged | increments |
| Rollback | restores the target generation | unchanged | restored from the recorded authority snapshot | unchanged |

A worker process incarnation is identified by `WorkerBootId`, minted per process start. A
`WorkerId` is a logical identity and stays stable across restarts. A coordinator mints a fresh
`CoordinatorEpoch` on every start from a durable high-water mark, so a retired epoch can never
be re-issued. A boot identity is bound to the epoch that issued it and can never be resurrected under
a later epoch.

## Metrics and observation semantics

A metric definition carries identity, name, unit, kind (`REAL`, `INTEGER`, `BOOLEAN`,
`CATEGORICAL`), direction (`MINIMIZE`, `MAXIMIZE`, `TARGET`,
`INFORMATIONAL`), aggregation rule (`NONE`, `MEAN`, `MEDIAN`, `MIN`,
`MAX`, `SUM`, `MAJORITY`), required status, optional validity bounds, optional target
range, weight and lexicographic priority.

An observation binds an experiment, generation, branch, branch generation, trial, attempt, metric,
producer, worker incarnation, coordinator epoch, validity state, typed value, provenance and
sequence. Validity is one of `VALID`, `INVALID`, `MISSING` or `UNSUPPORTED`;
these are never collapsed. Invalid, missing and unsupported observations never become numeric values,
and a missing metric is never zero.

Observation uniqueness is scoped to the producing attempt, and aggregation reads only the
authoritative attempt of each trial, so evidence left behind by an abandoned attempt is retained as
history and never contributes to a current aggregate. Duplicate transport delivery of byte-identical
evidence is idempotent; a conflicting re-use of an observation identity is refused. Non-finite values
are refused unless the metric explicitly accepts them, and configured validity bounds and declared
categories are enforced.

## Comparison and decision semantics

A comparison applies hard evidence constraints before any ranking:

- experiment open and candidate branch eligible;
- baseline present when the policy requires one;
- minimum authoritative completed trials per branch;
- every required metric present on both branches;
- minimum valid observations per required metric;
- no unresolved invalid observations when the policy rejects them;
- matching environment fingerprint when the policy requires it.

Only then are the configured deterministic comparison factors evaluated, ordered by lexicographic
priority and then by metric identity. Supported primitives are `GREATER_THAN`, `LESS_THAN`,
`MIN_IMPROVEMENT`, `MAX_REGRESSION_TOLERANCE`, `TARGET_RANGE`, `MAJORITY_WINS`
and `REPORT_ONLY`. An explicitly configured weighted score is computed only when the policy
opts into it; unrelated units are never silently normalized into one score. No statistical confidence
is claimed because no statistical component is implemented.

Decision outcomes are `ACCEPT`, `REJECT`, `INCONCLUSIVE`, `INSUFFICIENT_EVIDENCE`,
`INVALID`, `CANCELLED` and `SUPERSEDED`. A numerically favorable candidate is not
accepted when the evidence is insufficient, invalid, stale or incomplete. A policy with no decisive
factor yields `INCONCLUSIVE`. An exact tie is `INCONCLUSIVE` with an explicit tie-break
rule, never an acceptance. Finalization does not force a binary result: an insufficient or
inconclusive outcome is recorded and the experiment stays open.

## Explanations

Every authoritative decision is explainable deterministically. The explanation identifies the
experiment and generation, the hypothesis and revision, the comparison policy, the compared branches,
the baseline, the per-branch evidence counts, the metric aggregates with their valid, invalid,
missing and unsupported counts, every hard constraint with its pass or fail state, every comparison
factor, the tie-break rule where one was used, the rejected evidence, the final outcome, the reason
and a canonical digest of the compared evidence. Given the same canonical state and policy the
rendering is byte-identical, and no renderer iterates an unordered container.

## Lineage

Durable lineage spans experiment, hypothesis revision, branch, trial, attempt, observation, artifact,
comparison and decision, and remains queryable after restart. Parent references are validated on
load, cycles are rejected, and rollback never deletes history to make state look clean: it records a
new authoritative point that names the generation it restored from and preserves the superseded
history.

## Reproducibility

A reproducibility record captures the experiment definition digest, hypothesis revision, branch
definition digest, policy digest, input-set identity, environment identity and fingerprint,
executable identity, seed, parameter set, producer identity, artifact references and valid
observation count. Missing material is named explicitly. The record is classified
`REPRODUCIBLE`, `PARTIALLY_REPRODUCIBLE`, `NOT_REPRODUCIBLE` or `UNKNOWN` from
the material actually present; a stored random seed alone never makes an experiment reproducible.

## Persistence and recovery

The state image is versioned, little-endian, length-prefixed and integrity checked per record with
CRC-32 plus a whole-image SHA-256. The commit protocol builds the complete image, writes a temporary
file in the same directory, flushes and closes it, then atomically replaces the live file, so a
half-written image never becomes authoritative.

Decoding is bounded at every step and rejects truncation, header or record checksum mismatch,
unsupported versions, impossible enumeration values, absurd counts, integer overflow, duplicate
identities, invalid cross references, branch cycles and trailing garbage. A retained high-water mark
of the coordinator epoch guarantees epochs strictly increase across restarts.

On recovery, durable historical records are distinguished from live process authority. Every
surviving dynamic assignment is first marked `REVALIDATION_REQUIRED` and that conservative
state is written durably; it is then resolved explicitly, with attempts moving to `ABANDONED`
and trials to `READY` for re-execution or to `FAILED` when no retry budget remains.
Recovered in-flight state is never silently continued, and a missing or unreadable result is never
invented.

## Distributed control plane

The reference implementation is a real coordinator and worker runtime over framed loopback TCP.
Every frame has a fixed 32-byte header carrying a magic value, the protocol version, the message type,
a correlation identity, the declared payload length, a payload CRC-32, a header CRC-32 and a reserved
word that must be zero. Declared lengths are bounded before allocation. Short frames, oversized
frames, unknown types, checksum mismatches, non-zero reserved words and trailing payload bytes are all
refused, and every decoder must consume its buffer exactly.

## Provenance

| Claim | Label |
| --- | --- |
| Library, tools, tests, examples and benchmarks | REAL |
| Loopback TCP transport, operating-system processes and filesystem persistence | REAL |
| Worker process death and coordinator restart by real process termination | REAL |
| Deterministic synthetic branch workloads and metrics | SYNTHETIC |
| CUDA device discovery, allocation, kernel execution, CPU parity and cleanup | REAL (NVIDIA GeForce RTX 5090, compute capability 12.0) |
| Multi-node, RDMA, NVLink, NVSwitch, MIG, SmartNIC/DPU and cluster schedulers | UNSUPPORTED |
| External model providers, model hosting and token generation | UNSUPPORTED |
| Physical lab devices, simulators and dataset storage systems | UNSUPPORTED |
| Statistical confidence or significance testing | UNSUPPORTED (no statistical component is implemented) |

Windows x64 with MSVC is the validated platform.

## Build

`@@`@@`@
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
`@@`@@`@

Options: `EXPERIMENT_FABRIC_BUILD_TOOLS`, `EXPERIMENT_FABRIC_BUILD_EXAMPLES`,
`EXPERIMENT_FABRIC_BUILD_TESTS`, `EXPERIMENT_FABRIC_BUILD_BENCHMARKS`,
`EXPERIMENT_FABRIC_ENABLE_CUDA`, `EXPERIMENT_FABRIC_ENABLE_ASAN` and
`EXPERIMENT_FABRIC_WARNINGS_AS_ERRORS`.

Release and Debug both build with `/W4 /WX` on MSVC and zero first-party warnings.

## Test

`@@`@@`@
build/tests/ef_tests
build/tests/ef_tests --filter=multiprocess
`@@`@@`@

Tests run plainly: no timeout control of any kind is applied, because a hanging test is a defect to
diagnose rather than something to abandon. The suite covers unit, state-machine, integration,
end-to-end, persistence, protocol, property, seeded-randomized, adversarial, concurrency and
real-multiprocess obligations, and each claim in this README is bound to a test that exercises it.

The multi-process proofs launch real coordinator and worker executables, terminate them with real
process termination, and replay the framed traffic those processes actually sent before they died.

## Examples

`@@`@@`@
build/examples/example_latency_comparison
build/examples/example_throughput_memory_tradeoff
build/examples/example_branch_fork
build/examples/example_retry_after_failure
build/examples/example_cancelled_late_evidence
build/examples/example_rollback
build/examples/example_worker_loss
build/examples/example_coordinator_restart
build/examples/example_insufficient_evidence
build/examples/example_reproducibility
build/examples/example_lineage
build/cuda/ef_cuda_trial
`@@`@@`@

Each example exercises the systems boundary rather than displaying API syntax.
`example_insufficient_evidence` shows a numerically favorable candidate that is still not
accepted because the evidence floor is not met, and `example_cancelled_late_evidence` shows a
late result that can never become authoritative.

## Tools

`@@`@@`@
build/ef_coordinator --state <path> --port 0 --port-file <path> --scenario <name>
build/ef_worker --host 127.0.0.1 --port <n> --worker 1 --workload parameter --cycles 8
build/ef_ctl --host 127.0.0.1 --port <n> <command>
`@@`@@`@

`ef_ctl` supports `status`, `list`, `workers`, `validate`, `snapshot`,
`hypothesis`, `branches`, `lineage`, `trials`, `observations`,
`rejected`, `artifacts`, `repro`, `explain`, `finalize`,
`create-trial`, `fork`, `cancel-trial`, `cancel-experiment`, `rollback`,
`replay` and `shutdown`. Output ordering is deterministic.

## Benchmark

`@@`@@`@
build/ef_bench --scale=2000
`@@`@@`@

Every measurement covers a completed operation. Representative Release figures on the validated
platform (Windows x64, MSVC 19.44, 16 logical cores), scale 2000:

`@@`@@`@
experiment creation            3.4 us/op    (validated definition, identities allocated, state advanced)
trial admission               35.5 us/op
full trial lifecycle          44.9 us/op    (claim, observation publication, authoritative completion)
deterministic comparison       8.1 us/op
explanation rendering         15.4 us/op
snapshot read                 12.8 us/op
concurrent read path           2.3 us/op
durable commit                 1.2 ms/op    (serialise, flush, atomic replace)
state load                   302.5 us/op    (bounded decode, per-record checksums, whole-image digest)
`@@`@@`@

## Install and consume

`@@`@@`@
cmake --install build --prefix <prefix>
cmake -S consumer -B <outside-tree-build> -DCMAKE_PREFIX_PATH=<prefix>
cmake --build <outside-tree-build>
`@@`@@`@

`consumer/` is an independent downstream project that consumes only the installed package
through `find_package(ExperimentFabric 1.0 REQUIRED)` and the exported
`experiment_fabric::experiment_fabric` target. It is built outside the Experiment Fabric source
and build trees; an in-tree example is not treated as package proof.

## Known limitations

- Statistics are not implemented. No confidence interval, significance test or Bayesian comparison is
  computed, and none is claimed.
- The reference transport is loopback TCP only. Multi-node operation is UNSUPPORTED.
- The reference workers execute synthetic workloads. Physical lab devices, simulators and external
  model providers are UNSUPPORTED.
- The validated platform is Windows x64 with MSVC. Other platforms are supported by the code but are
  not validated.
- Persistence writes the whole state image on every mutation when a state path is configured. This is
  bounded and integrity checked but not incremental.
- Concurrent mutation is serialized by one coordinator lock. The runtime is a single-coordinator
  governance core, not a consensus group.
- The CUDA reference trial proves that a real accelerator-backed trial can participate in the
  lifecycle. It does not measure accelerator performance and makes no accelerator performance claim.
- Cancellation and rollback leave historical rejected evidence in place by design; they do not delete
  it.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
