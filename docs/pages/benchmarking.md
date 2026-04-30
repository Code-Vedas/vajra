---
title: Benchmarking
nav_order: 11
permalink: /benchmarking/
---

# Benchmarking

Use this page to interpret benchmark results conservatively.

## Benchmarking Posture

Benchmarks explain how Vajra behaves under known conditions and within the
documented runtime surface.

## What Results Show

- local startup cost and rebuild impact
- request/response behavior for the HTTP request path
- obvious regressions introduced by packaging or runtime changes

## Benchmark Fixture Families

The benchmark suite is organized around representative workload families:

- generic Rack fixtures
- Rails fixtures
- startup and boot-cost checks
- overload and degradation scenarios
- protocol- and transport-specific suites

## Reading Results Safely

When you review benchmark output, ask:

- which runtime path was exercised
- which build artifact and configuration were used
- whether the result reflects the supported platform set
- whether the result says anything about reliability, only throughput, or both

Benchmark interpretation stays tied to documented support boundaries.

## Environment Assumptions And Caveats

Every benchmark report makes these assumptions explicit:

- operating system and toolchain
- build mode and artifact provenance
- application fixture type
- whether the result covers the exercised protocol and transport path
- whether the selected platform and integration fixture match the reported claim
