# Autoreview effort evaluation

## Decision

Use `gpt-5.6-sol` at high effort for the final gate. Low is an optional early
screen. Do not use medium as the final gate, and do not pay for extra-high after
a clean high run unless a user explicitly requests it.

This calibration was run on 2026-07-21 with Codex CLI 0.145.0. Re-run it after a
material model, prompt, schema, or rulebook change; the result is evidence for
this version, not a permanent property of an effort label.

## Method

- Review pre-fix historical diffs from this repository. Exclude the later fix
  commits and their explanatory messages from the supplied bundle.
- Give every run the same repo rulebook, structured schema, and approximately
  31 KB primary change bundle.
- Count a bug class only when the output identifies its causal failure, not
  merely a nearby file. Count unsupported actionable findings separately.
- Use reported token usage as the cost proxy because the CLI does not expose a
  dollar charge.

The primary corpus covered five regressions: missing Views metadata
(`f7b3ad8`), fixed-delay animation race proof (`e521e8a`), a false-green remote
build and stale warm-builder overlay (`a11765e`), and UI-thread configuration
I/O (`a97973e`).

## Primary corpus results

| Effort | Recall | Unsupported | Latency | Input | Output | Reasoning |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| low | 4/5 | 0 | 30 s | 23,748 | 1,085 | 452 |
| medium | 4/5 | 1 | 118 s | 68,258 | 4,516 | 3,751 |
| high | 5/5 | 0 | 140 s | 23,744 | 5,091 | 4,367 |
| xhigh | 5/5 | 0 | 255 s | 23,746 | 9,524 | 8,804 |

High was the lowest effort with full recall. Medium had no recall gain over low
and invented a missing registration from intentionally omitted context. Xhigh
had no recall gain over high while taking 1.8 times the latency and about twice
the reasoning tokens.

## Holdout and rule iteration

A separate protocol/queue holdout used the pre-fix diffs for a switch that
failed open on unknown enum values (`4449708`) and unbounded ready-state input
writes (`a149129`). Low found neither seeded defect in 38 seconds. High found the
fail-open defect plus genuine generation/reconnect hazards in 201 seconds, but
initially missed ready-state backpressure.

The rulebook was then made explicit about one-in-flight writes, capped/coalesced
pending work, ordering, and generation-fenced completion. A focused high run on
the unchanged pre-fix input diff caught the unbounded write queue in 178
seconds. This is why the rulebook is part of the gate: model effort alone did
not supply the repository-specific invariant.

## Recalibration rule

Keep a blinded set of historical pre-fix diffs. Compare at least low, high, and
the next more expensive candidate. Select the cheapest configuration that
catches every P0/P1 and the labeled P2 set without unsupported actionable
findings. Do not trade a known lifetime, protocol, or false-green miss for a
small latency improvement.
