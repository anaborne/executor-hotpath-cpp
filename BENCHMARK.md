# Benchmark plan, written before the executor exists

Written 2026-08-29, before the first line of the C++ executor. Nothing in this repository has been
measured yet. The point of writing it now is that the expectation below is falsifiable and the
methodology is fixed, so a number that comes back wrong cannot be reinterpreted into a number that
comes back right.

Corrections are appended with a date, never edited into place.

## 1. What is being compared

One process, not one system. `prediction-market-infra` runs a poller and an executor over a Unix
domain socket. This repository reimplements the executor, from `accept` to the point where the
Python version calls `dispatch()`: read the frame, stamp, decode, ack, kill switch, wire-price
refusal, template fill, telemetry enqueue. The REST client is a fake, exactly as in
`benchmarks/latency_bench.py`. No network, no credentials, no exchange.

The wire protocol is unchanged, byte for byte. 4-byte big-endian length prefix, JSON body,
`WakeMessage` schema v4 with every post-v1 default preserved, `WakeAck` with status in
{accepted, rejected}. The Python poller must drive the C++ executor with zero changes to
`poller_client.py`. If that stops being true the comparison is not honest and the numbers are
withdrawn.

## 2. The three configurations

| Configuration | Poller | Executor |
|---|---|---|
| py-to-py | Python, uvloop | Python, uvloop |
| py-to-cpp | Python, uvloop | C++ |
| cpp-to-cpp | C++ | C++ |

py-to-py is the baseline and already exists. py-to-cpp is the one that matters, because it is the
drop-in substitution a real deployment would make. cpp-to-cpp exists so the Python poller's floor
can be separated from the executor's cost, and for no other reason. The C++ poller client is a
measurement instrument, not a product, and it is the first thing cut if the schedule slips.

## 3. What is measured

Executor-side `wake_recv`, which is decode plus ack, the same span the Python executor already
records. End-to-end `wake_send` as the Python harness measures it, from the poller's `put_nowait`
to the poller reading the ack back. The signer, RSA-PSS over `timestamp + method + path`, in both
languages. A cpp-to-cpp round trip.

Reported as p50, p90, p99 over n=2000, from a sorted vector. No histogram and no HdrHistogram
dependency; at n=2000 a sorted vector is exact and the dependency buys nothing.

The percentile estimator must be the one `latency_bench.py::_percentile` already uses, which is
linear interpolation on `rank = (n - 1) * p` between the floor and ceiling ranks. This is the type-7
estimator. It is written down here because a nearest-rank C++ implementation would produce numbers
that look comparable to the Python ones and are not, and that error would be invisible in every
output the harness prints.

## 4. Warm-up, and an open decision

Warm-up iterations are discarded and the count is stated in the output.

`latency_bench.py` currently discards none. Every iteration lands in the percentile set including
the first, with a cold RSA context, a cold socket, and a cold SQLite page cache. That is a real gap
in the existing harness, and it has to be closed on both sides or neither: a C++ run with warm-up
against a Python run without it is a rigged comparison, and the rigging favours the result this
repository wants.

Not resolved here. Recorded as an open decision to be made at step 6, when the harness flag is
added, and whichever way it goes it goes the same way in both languages. Settled in section 9,
under 2026-08-30, second.

## 5. The machine

One session, on the Mac, fresh boot, nothing else open. Apple silicon, Apple clang, Python 3.12.
Every row carries a platform tag, as `history.csv` already does, because latency is a property of
the machine and not of the code. Numbers produced on a GitHub runner are not reported anywhere.

Raw CSVs are committed. `history.csv` in `prediction-market-infra` gains an `executor` column and
existing rows are backfilled with `python`.

## 6. The expectation, pre-registered

Executor-side `wake_recv` drops by roughly an order of magnitude, and end-to-end `wake_send` barely
moves.

The reasoning is the span boundaries, not optimism about C++. `wake_send` starts at the poller's
`put_nowait` and ends when the poller reads the ack back, so it contains a Python queue hop, a
Python `drain()`, and a Python telemetry enqueue on either side of whatever the executor does. The
C++ executor replaces only the middle. The Python poller is the floor, and a floor is not something
the executor can optimise.

The current Python baseline, from `benchmarks/history.csv` on Linux-aarch64-py3.14.7:

```
wake_recv (uvloop)   p50=0.0039ms  p99=0.0053ms
wake_send (uvloop)   p50=0.5905ms  p99=0.6958ms
sign()               p50=0.3303ms  p99=0.4962ms
```

`wake_recv` at 3.9us p50 is already fast. How much faster, and whether the difference survives
contact with the end-to-end number, is what the run has to answer. If py-to-cpp `wake_send` comes
back materially better than py-to-py, the expectation above was wrong and the README says so in its
first paragraph.

The signer is a separate question. Both languages call into an RSA implementation and spend their
time in a modular exponentiation neither of them wrote, so a large gap there would be surprising and
would mean the Python binding overhead is bigger than assumed rather than that the C++ is fast.

## 7. Done means

`history.csv` has rows for all three configurations from one machine on one day. The golden-frame
contract test passes in both languages. RESULTS.md is written the same day as the run. If the
numbers contradict section 6, that is the finding and it leads.

## 8. Not measured, recorded so the absence is a decision

Throughput. This is a latency project.

Process startup, memory, and binary size.

The poller, the matcher, the decision logic, and Polymarket. All out of scope by design, and a
half-built version of any of them would be worse than their absence.

epoll and kqueue tuning beyond what a single-connection Unix socket server needs, kernel bypass,
and custom allocators. The executor handles one connection from one local peer. Reaching for any of
those would be optimising a path that is not the bottleneck, and the benchmark is supposed to find
out where the bottleneck is rather than assume it.

Cross-platform comparison. CI builds on Linux and macOS so the code is portable. The numbers come
off one machine and are never compared across two.

## 9. Corrections

**2026-08-30.** Section 1 says `prediction-market-infra` runs a poller and an executor. It no
longer runs anything. The private system it was extracted from was shut down on 2026-08-29, the
extraction is what remains, and it is a published reference implementation rather than a live
process.

Nothing in the plan moves. The three configurations, the spans in section 3, the warm-up decision
left open in section 4 and the pre-registered expectation in section 6 all describe a benchmark
that runs both implementations on this machine, and none of them ever needed either process to be
serving real orders.

One claim does move. Section 2 calls py-to-cpp "the drop-in substitution a real deployment would
make." There is no deployment left to make it in. It is the substitution the protocol permits,
demonstrated by `poller_client.py` driving both executors without a change, and that is how the
number will be labelled in RESULTS.md.

**2026-08-30, second.** Section 4's open decision is settled. Both harnesses discard 200 warm-up
iterations from the front of every sample and report over the 2000 that follow, and both print the
discarded count on every line. `latency_bench.py` discarded none before today and its wake round
trip ran 300 iterations rather than 2000, so both of those moved with it.

The reasoning is that p99 at n=2000 is the twentieth worst sample. A handful of first-iteration
outliers, a cold RSA blinding context, a cold socket buffer, a cold SQLite page cache, land inside
that twenty rather than beside it, so leaving them in reports a startup cost as a steady-state
percentile. 200 is ten percent of the sample: past every transient named above and far too small to
absorb anything that persists.

The three rows already in `history.csv` are backfilled with `warmup=0`, which is what those runs
did, and their new `p90` cells are left empty rather than interpolated from a p50 and a p99 they
were never computed with. They came off a different machine and section 5 already forbids comparing
them to the Mac's numbers.

**2026-08-30, third.** One asymmetry in the py-to-cpp configuration, recorded before the run rather
than discovered in it. The Python executor dispatches every accepted fire through `OrderDispatcher`
and the fake REST client and writes its own telemetry for the dispatch; the C++ binary's dispatch
hook is empty, so past the ack it builds the order template and stops. `wake_recv` closes before the
fire in both, so the span itself is unaffected, but the two processes are not equally loaded and
`wake_send` in the py-to-cpp row runs against an executor with less to do. Any part of a py-to-cpp
`wake_send` improvement could be that rather than the decoder, and RESULTS.md has to say so.

**2026-08-30, fourth.** The cpp-to-cpp round trip does not measure `wake_send`. The Python's
`wake_send` runs from the poller's `put_nowait` to the end of `drain()` and never waits for the ack;
the C++ client in `bench/poller_client.cpp` writes one frame and blocks for its ack, which is a
different span. It is reported as `roundtrip` and never in a `wake_send` column. The comparable
number out of that configuration is `wake_recv`, which both executors record the same way.
