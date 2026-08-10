# Project Guidelines

This is a learning and portfolio project.

## Priorities

1. Correctness
2. Readability
3. Measurable performance
4. Optimization

Never sacrifice correctness for speculative performance improvements.

## Architecture

Latency-sensitive path:

Order
-> Risk
-> MatchingEngine
-> OrderBook
-> Event

CUDA is only used for offline/batched analytics.

## Rules

- C++20
- CMake
- GoogleTest
- Google Benchmark
- No floating point for prices
- Prefer simple STL implementations initially
- No premature lock-free structures
- No unnecessary dependencies
- Every optimization must have a benchmark
- Every bug fix should have a regression test

## Agent behavior

Before major architectural changes:

1. Explain the issue.
2. Propose the smallest change.
3. Wait until the current implementation is understood.
4. Keep changes reviewable.

Do not create or modify README files unless explicitly requested.
Do not add documentation files, design documents, changelogs, or project notes without approval.
Do not generate project summaries or reports as files unless explicitly requested.

Avoid adding features, abstractions, or dependencies that were not requested.

Do not add features that were not requested.