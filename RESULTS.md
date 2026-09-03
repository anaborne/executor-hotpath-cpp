# Results

Run 2026-08-30. One machine, one session, the three configurations BENCHMARK.md section 2 names.
Written the same day, as section 7 requires.

The headline is that the pre-registered expectation in section 6 is wrong in both halves, and that
one of the three measurements does not compare what it was built to compare.

## The run

| | |
|---|---|
| Machine | Apple silicon, macOS, `Darwin-arm64` |
| Toolchain | Apple clang 17.0.0, Release, CPython 3.12.14 |
| Sample | n=2000 after 200 discarded warm-up iterations, every row |
| Estimator | type-7, the one `latency_bench.py::_percentile` uses |
| Raw data | `bench_history.csv` here, `benchmarks/history.csv` in prediction-market-infra |

**Deviation from section 5, disclosed.** Section 5 pre-registered a fresh boot. This machine had
been up 38 days and was not rebooted; every application was quit instead, and the 1-minute load
average was 1.2 at the start of the run rather than the sub-1.0 that was wanted.

What stands in for the reboot is a repeat. The C++ benchmark was run twice back to back and the two
runs agree: `sign` within 1.3%, `roundtrip` within 3.8%, `wake_recv` within 5.3%, `decode` within
12.5% on an absolute difference of 200ns. Only the first run was written to the CSV, so the
percentages above are the only record of the second; nobody can check them, and nobody can check an
assertion that the machine was quiet either.

## The three configurations

Executor-side `wake_recv`, in milliseconds, under uvloop, which is the configuration section 6
reasoned about:

| Configuration | p50 | p90 | p99 |
|---|---|---|---|
| py-to-py | 0.0045 | 0.0056 | 0.0062 |
| py-to-cpp | 0.0016 | 0.0019 | 0.0026 |
| cpp-to-cpp | 0.0019 | 0.0020 | 0.0026 |

End-to-end `wake_send`, uvloop, milliseconds:

| Configuration | p50 | p90 | p99 |
|---|---|---|---|
| py-to-py | 0.0257 | 0.0302 | 0.0331 |
| py-to-cpp | 0.0123 | 0.0166 | 0.0257 |

`sign`, milliseconds, on the same machine in the same session:

| Implementation | p50 | p90 | p99 |
|---|---|---|---|
| `cryptography` 50.0.1 | 0.9227 | 0.9352 | 1.8646 |
| OpenSSL 3 EVP, this port | 0.3297 | 0.3348 | 0.4937 |

The C++ component breakdown, which has no Python counterpart because `latency_bench.py` measures no
such spans:

| Benchmark | p50 | p90 | p99 |
|---|---|---|---|
| `decode` | 0.0016 | 0.0019 | 0.0019 |
| `encode_ack` | 0.0001 | 0.0002 | 0.0002 |
| `roundtrip` | 0.0078 | 0.0082 | 0.0107 |

## Against the pre-registration

Section 6 said: "Executor-side `wake_recv` drops by roughly an order of magnitude, and end-to-end
`wake_send` barely moves."

**`wake_recv` did not drop by an order of magnitude.** It dropped by a factor of 2.4 at p99 and 2.8
at p50 under uvloop. The prediction was off by roughly half in log terms, and it was off in the
direction that flattered the port.

There is a floor under this, and it is worth naming because it bounds how much better the number
could get. The same C++ executor, measured two ways, reports `wake_recv` at 1.6us and 1.9us: driven
by the uvloop poller and by the C++ client. Those two should be identical, because the executor does
not know what is on the other end of the socket. They differ by 0.3us, which is the resolution of
this measurement at this scale. Any claim about `wake_recv` finer than "between 1 and 2
microseconds" is reading noise.

**`wake_send` moved, and it should not have.** It halved at p50, 0.0257 to 0.0123 under uvloop.
Section 6 predicted it would barely move, and the reasoning given was that the span brackets the
executor with Python work on both sides. That reasoning was right and the prediction still failed,
because of something section 6 did not account for: `wake_send` ends at `drain()` and never
contains the executor's work at all. Swapping the executor should have left it untouched.

The explanation is a confound in the harness, described in the next section.

## What this run does not measure

**The `sign` row compares two OpenSSL builds, not two languages.**

Section 6 anticipated one surprise here: "a large gap there would be surprising and would mean the
Python binding overhead is bigger than assumed rather than that the C++ is fast." There is a large
gap, 2.8x, and the cause is neither of those. The two implementations are not calling the same
library.

`cryptography` 50.0.1 statically bundles **OpenSSL 4.0.2**. This port links Homebrew's
**OpenSSL 3.6.3**. Two builds, two versions, different compilation.

The C++ side adds nothing on top of its library. `openssl speed rsa2048` against that same Homebrew
build reports 0.000334s per signature, and this port's signer measures 0.3297ms. The wrapper is
free; the number is the library's number.

Binding overhead was ruled out rather than assumed away. Hoisting the per-call `padding.PSS`,
`padding.MGF1` and `hashes.SHA256` objects in `_sign_message` out of the call and reusing them
changed the Python timing by -1%, which is noise. The time is inside the modexp, and the two
modexps come from different code.

So the honest reading of the `sign` row is: on this machine, `cryptography`'s bundled OpenSSL 4.0.2
signs RSA-2048 about 2.8 times slower than Homebrew's OpenSSL 3.6.3. That is a fact about two
library builds. It says nothing about C++ against Python, and it is not evidence for the port. The
row stays in the table because removing a measurement that came out inconvenient is worse than
publishing it with its meaning corrected.

**py-to-py and py-to-cpp differ by process topology as well as by language.**

`latency_bench.py::_run_wake_roundtrip_python` constructs the `ExecutorServer` and the
`IPCPollerClient` in the same process, on the same event loop. The cpp path spawns the executor as
a separate process. So py-to-cpp is not "the same benchmark with a faster executor." It is the same
benchmark with the executor moved off the poller's event loop and onto another core, and rewritten.

This is the confound that explains `wake_send`. In py-to-py the poller's `drain()` competes with the
executor's read, decode, ack and dispatch on one thread; in py-to-cpp it does not. Some unknown
share of every py-to-cpp improvement in this document is process separation rather than C++.

Separating the two would need a py-to-py configuration that spawns a Python executor subprocess,
which does not exist and is not written here. Until it does, every ratio above is an upper bound on
what the language change bought.

Worth stating plainly: production ran the poller and the executor as separate processes. The
single-process py-to-py baseline is a property of the benchmark harness, not of the system it was
extracted from, and it predates this work.

**`roundtrip` has no Python counterpart.** The C++ client writes one frame and blocks for its ack;
`wake_send` never waits for an ack. The 7.8us figure is the cpp-to-cpp write-to-ack path and is not
comparable to any number in the `wake_send` column. PORT-FIDELITY.md carries the detail.

## What the run does support

Inside the C++ executor, `decode` measured standalone at 1.6us accounts for most of `wake_recv` at
1.9us and `encode_ack` at 0.1us for a small remainder, with the ack's `send` inside the difference.
Both figures are rounded to 100ns and the two are separate measurements, so no percentage split
should be read off them. If the executor's half of the wire is ever worth optimizing further, the
decoder is the only place with anything in it.

`sign` moved 0.3% across the executor swap, 0.9227 to 0.9256. That is the control working. Signing
lives in `rest_client.py`, past the `dispatch()` boundary, so it is in neither executor process and
swapping them should not have touched it. It did not.

The Python poller drove the C++ executor with no change to `poller_client.py`, at 2200 frames per
loop, 4400 in the session, with every frame accepted, every fire built, and zero telemetry rows
dropped. That was the protocol claim this repository was built to make, and it holds.

## Corrections to earlier documents

BENCHMARK.md section 9 records two errors found in the days before this run: `wake_send` was
measuring the poller's queue depth under a burst-shaped benchmark, and section 3 described the span
as ending when the ack is read when it ends at `drain()`. The numbers above are all post-fix. The
three rows in `history.csv` from before 2026-08-30 are not comparable to anything here on
`wake_send`, on top of already not being comparable by machine.

## What would have to change for a cleaner answer

1. A py-to-py configuration with the Python executor in its own process, so the language change can
   be separated from the process separation.
2. Both signers against one OpenSSL build, or the `sign` row dropped as a cross-language comparison
   and kept only as a per-library note.
3. A measurement floor better than 0.3us if `wake_recv` is ever to be reported more precisely than
   an order of magnitude band.

None of these change the protocol result, and none of them are done here.
