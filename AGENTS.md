# Project Guidelines

This repository is a learning and portfolio-oriented systems project.

The goal is to build understandable, correct, measurable systems rather than accumulate features or technologies.

## Priorities

In order:

1. Correctness
2. Clear architecture and ownership
3. Readability and maintainability
4. Measurable performance
5. Optimization

Never sacrifice correctness or architectural clarity for speculative performance improvements.

Do not optimize without evidence.

## Engineering Principles

Prefer simple, explicit designs.

Important system properties such as ownership, execution order, persistence boundaries, and failure semantics should be visible in the architecture rather than hidden behind unnecessary abstractions.

Prefer:

* explicit ownership;
* deterministic behavior where practical;
* narrow interfaces;
* reviewable changes;
* measurable performance;
* failure modes that are explicit rather than silently recovered from.

Avoid introducing complexity merely because it may be useful in a hypothetical future system.

## Core Technical Rules

* Use C++20.
* Use CMake for builds.
* Use GoogleTest for tests.
* Use Google Benchmark for performance benchmarks.
* Do not use floating point for prices or financial quantities where exact arithmetic is required.
* Prefer simple STL-based implementations unless measurement demonstrates a real limitation.
* Do not introduce lock-free structures without a demonstrated need.
* Avoid unnecessary external dependencies.
* Preserve deterministic execution semantics where the architecture relies on them.
* Do not weaken validation, durability, or correctness to improve benchmark results.

## Performance

Every meaningful optimization should be justified by measurement.

Before optimizing:

1. Establish a reproducible baseline.
2. Identify the actual bottleneck.
3. Make the smallest targeted change.
4. Measure again.
5. Preserve correctness tests.

Do not assume that a data structure, allocation, thread, queue, syscall, or subsystem is a bottleneck without evidence.

Benchmarks should clearly state what their timed region includes and excludes.

## Testing

Every bug fix should have a regression test where practical.

Architectural changes should include tests for the invariants they affect.

Prefer tests that verify meaningful system properties rather than only successful return values.

For persistence, networking, concurrency, and recovery behavior, use integration or process-level tests when unit tests cannot validate the real boundary.

Do not weaken existing tests simply to make a refactor or feature pass.

## Architecture Changes

Before making a major architectural change:

1. Inspect and understand the current implementation.
2. Explain the problem with the current design.
3. Propose the smallest change that solves the problem.
4. Identify affected invariants and ownership boundaries.
5. Keep the implementation reviewable.

Do not perform broad redesigns when a local change is sufficient.

Do not combine unrelated architectural changes in the same task.

## Repository Boundaries

Keep major responsibilities clearly separated.

The repository may contain multiple layers such as:

* exchange execution and matching;
* accounting and durability;
* networking and protocol;
* agent/runtime experimentation;
* external providers and integrations;
* applications and demos;
* tests and benchmarks.

Experimental or provider-specific code should not unnecessarily become a dependency of the core exchange system.

Do not introduce circular dependencies between layers.

When moving or refactoring code, prefer clearer dependency direction over cosmetic directory changes.

## Agent and Tool Behavior

When working autonomously on this repository:

* Do only the requested task.
* Avoid adding unrequested features.
* Avoid creating abstractions for hypothetical future requirements.
* Avoid introducing dependencies unless required by the task.
* Preserve current behavior unless behavior changes are explicitly requested.
* Keep changes small enough to review.
* Inspect existing tests and architecture before modifying core behavior.

If a requested implementation reveals a larger architectural issue, explain it rather than silently expanding the scope.

## Documentation

Do not create or modify README files unless explicitly requested.

Do not create new documentation files, design documents, changelogs, project notes, summaries, or reports unless explicitly requested.

If code movement requires updating a path, build reference, or existing comment, make only the minimal necessary documentation adjustment.

Do not use documentation changes as a substitute for implementing or validating the requested behavior.

## Scope Discipline

Do not add a technology solely because it is interesting or useful for résumé breadth.

New languages, frameworks, storage systems, concurrency mechanisms, GPU code, distributed systems components, or external services should solve a concrete problem in the project.

Prefer finishing and validating an existing system boundary before expanding into a new one.

When uncertain, choose the smaller change.
