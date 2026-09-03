# executor-hotpath-cpp

[![CI](https://github.com/anaborne/executor-hotpath-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/anaborne/executor-hotpath-cpp/actions/workflows/ci.yml)

The executor process from
[prediction-market-infra](https://github.com/anaborne/prediction-market-infra), rewritten in C++20
against the same wire protocol, so the Python poller drives either one and the two are timed against
each other in the same run on the same machine.

The Python poller drove this executor across 4400 frames with `poller_client.py` unmodified, every
frame accepted and no telemetry row dropped. Executor-side `wake_recv` dropped by a factor of 2.4 at
p99 and 2.8 at p50 under uvloop, 0.0045ms to 0.0016ms. The caveat is that the two configurations
differ by more than language: the Python baseline runs the poller and the executor on one event loop
and the C++ configuration spawns a second process, so every ratio here is an upper bound on what the
rewrite bought. [`RESULTS.md`](RESULTS.md) is where those numbers live and it carries the confound
in full.

The pre-registered expectation in [`BENCHMARK.md`](BENCHMARK.md) section 6 was written before any
code existed, and it was wrong in both halves. It predicted `wake_recv` would fall by roughly an
order of magnitude, and 2.4x to 2.8x is not that. It predicted end-to-end `wake_send` could not move
because the span brackets the executor with Python work on both sides, and `wake_send` halved,
because the span ends at `drain()` and never contained the executor at all.

The Python side is a published reference implementation, not a running system.
`prediction-market-infra` is an extraction from a private trading bot, and that bot was shut down on
2026-08-29 when the last surviving strategy failed its pre-registered replication. What is on offer
is two implementations of one wire protocol, timed on one machine in one run. What is not on offer
is a production A/B, and no number here will be presented as one.

## The contract test

The frames the C++ is tested against were produced by running the Python, not by reading it. Ten of
them: five `WakeMessage` shapes, the v1 through v3 legacy frames, and both `WakeAck` statuses. The
C++ suite asserts field-for-field equality against the five message shapes and the two acks. The v1
frame asserts that every field added since arrives at its Python default, two of which are refusals
rather than permissions; the v2 and v3 frames assert the fields added after each of them. The
rejection cases are separate, in `test_protocol.cpp` and `test_executor.cpp`: an oversize length
prefix closes the connection, and a malformed body produces a `rejected` ack whose reason names the
field when the failure is a missing or unexpected one, and names the parse error otherwise. The
reason text is this port's own, where the Python sends `str(exc)`; the poller only logs it, and
[`PORT-FIDELITY.md`](PORT-FIDELITY.md) records the difference.

The encoder is byte identical to `orjson.dumps` over `WakeMessage` and `WakeAck`. That is a stricter
claim than valid JSON, because `orjson` serializes a dataclass in field-declaration order, so the
wire key order is the attribute order and any reordering is a protocol change.

Float formatting is where a general-purpose C++ serializer would diverge. `orjson` writes shortest
round-trip digits in fixed notation for a decimal exponent in [-5, 16) and scientific outside it, so
`1e15` is `1000000000000000.0` and `1e16` is `1e+16`. `tests/golden/doubles.tsv` pins 1,000 values
against `orjson`'s own output. Price snapping goes through `round_half_even`, since Python's
`round()` is half-to-even and `std::round` is half-away-from-zero, and 424 cases the Python produced
hold it there.

[`PORT-FIDELITY.md`](PORT-FIDELITY.md) records what is identical, the six places this decoder is
stricter than Python's, and the evidence behind each.

```bash
uv run --with orjson python tests/golden/generate_golden.py --infra ../prediction-market-infra
```

Nothing in CI regenerates the fixtures. They are committed, and changing one is a deliberate act
with a diff to review. Regenerating against `prediction-market-infra` at `e3fd937` under Python
3.14.7 reproduced all ten frames, all 1,000 doubles and all 424 snap cases byte for byte, and the
same three fixtures regenerate identically under CPython 3.11 on x86-64 Linux. `percentiles.tsv` is
the one that does not travel: its sample vectors come from `random.lognormvariate`, which goes
through the platform's `exp`, and three of the 2,000 samples land one ulp apart between Apple's
libm and glibc's. The percentile values the test asserts are unchanged by that, and the file is only
ever regenerated on the machine that produced it.

## Results

Run 2026-08-30, one machine, one session. Executor-side `wake_recv` in milliseconds, under uvloop,
n=2000 after 200 discarded warm-up iterations:

| Configuration | p50 | p90 | p99 |
|---|---|---|---|
| py-to-py | 0.0045 | 0.0056 | 0.0062 |
| py-to-cpp | 0.0016 | 0.0019 | 0.0026 |
| cpp-to-cpp | 0.0019 | 0.0020 | 0.0026 |

Those bottom two rows are one executor measured from two different clients, and they should agree,
because the executor does not know what is on the other end of the socket. The same binary reports
1.6us and 1.9us across the two. The 0.3us spread is the resolution of this measurement at this
scale, and any claim about `wake_recv` finer than "between 1 and 2 microseconds" is reading noise.

Inside the C++ executor, `decode` measured standalone accounts for most of `wake_recv` and
`encode_ack` for a small remainder, with the ack's `send` inside the difference. Both figures are
rounded to 100ns and the two are separate measurements, so no percentage split should be read off
them. The decoder is the only place with anything in it.

The signer, benchmarked in the same session, turned out not to compare what it was built to
compare. That row is 2.8x in this port's favour and it is not a language result: `cryptography`
bundles OpenSSL 4.0.2 and this port links Homebrew's OpenSSL 3.6.3, so the row compares two library
builds.
`openssl speed rsa2048` against that Homebrew build lands within 2% of what this port's signer
measures, so the C++ wrapper costs nothing and contributes nothing to the gap. Binding overhead was
ruled out with an experiment rather than an argument. The row stays in the table because removing a
measurement that came out inconvenient is worse than publishing it with its meaning corrected.

[`RESULTS.md`](RESULTS.md) has the `wake_send` and component tables, the disclosed deviation from
the pre-registered method, and the three things that would have to change for a cleaner answer.
Raw rows are `bench_history.csv` here and `benchmarks/history.csv` in `prediction-market-infra`.

## What is here

`WakeMessage`, `WakeAck`, the length-prefixed frame codec with its 64 KiB cap, the JSON layer, the
executor server from `accept` to the point where the Python calls `dispatch()`, the telemetry path
that carries `wake_recv` off that server and into SQLite, the Kalshi request signer, and the
benchmark harness on both sides of the wire.

The server is one thread on one Unix domain socket, and the read loop runs in the Python's order:
read the body, stamp, decode, ack, fire. The stamp comes after the read returns, since the read
blocks until a frame arrives and a stamp taken ahead of it measures the gap between wakes. The ack
goes out before the fire, so a dispatch never lands inside the span the poller measures.

### Telemetry

`--telemetry-db` writes `latency_events` rows into the same SQLite file and the same columns
`benchmarks/latency_bench.py` queries, so the Python harness reads the C++ executor's numbers with
its own `SELECT` unchanged. A `record` call copies the row into a fixed-size ring and returns; one
writer thread owns the connection and commits in batches of 500.

Nothing on the read loop can wait for a write. The ring holds 8192 rows, pushing to a full one drops
the row rather than blocking, and the four counters are printed at exit next to the server's:

```
frames=5 accepted=5 rejected=0 fired=5 refused_price=0 refused_kill_switch=0
telemetry rows_written=5 dropped_ring_full=0 dropped_oversized_id=0 dropped_write_failed=0
```

A writer that fell behind shows up as `rows_written` short of `frames`. The drops are counted rather
than logged because a log line is something to go looking for, and a run that silently
under-reported would look like a run that was quiet.

Opening a file the C++ wrote with the Python's own `TelemetryDB.initialize()` afterwards fills in
the seven tables the executor does not write and leaves every row in place.

### Signer

RSA-PSS over `timestamp + method + path`, MGF1 and the digest both SHA-256, salt length equal to the
digest length, base64 out, through OpenSSL 3's EVP interface. Kalshi's published construction, and
`auth/signer.py` is the specification for it.

PSS salts randomly, so signing one message twice with one key gives two different valid signatures
and there is no golden signature to compare bytes against. The check runs both ways instead.
`tests/golden/signing` holds five signatures the Python produced against a throwaway RSA-2048 key
and the public key that verifies them, and the suite verifies each against the message this port
builds. A signature carrying OpenSSL's default salt, 222 bytes rather than 32, fails that same
verifier, which is what keeps the assertion from being vacuous.

The other direction is a command rather than a gate, because gating it would put `cryptography` on
all three CI runners to check a value that is different every run:

```bash
uv run python tests/golden/generate_signing_fixture.py --verify-cpp build/dev/signer_cross_check
```

No private key is committed. Both sides generate one into a temporary directory and only the public
half is written out.

## What was deliberately not built

The poller, the matcher, the decision logic, and Polymarket. Position sizing, the risk gate, and the
`orders_fired` write. `ExecutorConfig::dispatch` is where a real order would go and `main.cpp`
leaves it unset. The REST client is a fake, exactly as in `benchmarks/latency_bench.py`: no network,
no credentials, no exchange call. A half-built version of any of those would be worse than its
absence.

`epoll` and `kqueue` tuning beyond what a single-connection Unix socket server needs, kernel bypass,
and custom allocators. The executor handles one connection from one local peer, and the benchmark
exists to find out where the time goes rather than to assume it.

Throughput, process startup, memory, and binary size. This is a latency project.

`simdjson`, which is the obvious choice for the decoder and is not used here. The encoder has to be
byte identical to `orjson` and no general-purpose serializer is, so the encoder was going to be
hand-written either way, and the decoder was written alongside it to match. Parsing here is not
obviously faster than `simdjson` would be and has never been measured against it. `src/json.hpp`
carries that trade-off, including the half of it that did not survive.

The C++ poller client in `bench/` is a benchmark fixture, not a product. It exists so a cpp-to-cpp
round trip can be measured next to py-to-py and py-to-cpp.

Cross-platform comparison. CI builds on Linux and macOS so the code is portable. The numbers come
off one machine and are never compared across two.

## Running it

SQLite and OpenSSL 3 are the external dependencies and both come from the system: macOS ships SQLite
and Homebrew's `openssl@3` supplies the rest, Ubuntu needs `libsqlite3-dev` and `libssl-dev`. Catch2
comes from FetchContent.

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --no-tests=error
```

```bash
./build/dev/bin/executor_hotpath --socket /tmp/executor.sock --kill-switch /tmp/halt \
    --telemetry-db /tmp/telemetry.db
```

Under sanitizers. `asan-ubsan` is what CI runs on Linux, `ubsan` is what runs on this Mac:

```bash
cmake --preset ubsan
cmake --build --preset ubsan
ctest --preset ubsan --no-tests=error
```

AddressSanitizer does not run on macOS here. On Darwin 25.5 with Apple clang 17 an
ASan-instrumented `int main(){return 0;}` hangs before reaching `main`, with no local workaround.
UndefinedBehaviorSanitizer works, which is why it gets its own preset. ASan and LeakSanitizer are
exercised by the Linux job in CI, and only those results are believed.

### Benchmarks

Two harnesses, one estimator. `bench/` times four things inside this process, and
`benchmarks/latency_bench.py` in `prediction-market-infra` gained `--executor cpp:PATH`, which puts
this binary on the far end of the poller's socket with `poller_client.py` unmodified:

```bash
cmake --preset release && cmake --build --preset release
./build/release/bin/executor_hotpath_bench --csv bench_history.csv
```

```bash
uv run python benchmarks/latency_bench.py \
    --executor cpp:/path/to/executor-hotpath-cpp/build/release/bin/executor_hotpath
```

Both discard 200 warm-up iterations and report p50, p90 and p99 over the 2000 that follow, from a
sorted vector rather than a histogram, which at that sample size is exact and buys no dependency.
Both use the type-7 estimator `_percentile` uses; `tests/golden/percentiles.tsv` holds that
function's own output for seven vectors and the C++ is compared against it as bit patterns, because
a nearest-rank implementation would agree to the printed precision and be wrong.

`roundtrip` from the cpp-to-cpp configuration is write-to-ack and is not the Python's `wake_send`,
which never waits for an ack. The number the two languages can be compared on is `wake_recv`.

### Linters

Version-pinned, because Ubuntu ships clang-format 18 and Homebrew ships 23 and the two format the
same file differently:

```bash
uv tool install clang-format==23.1.0
uv tool install clang-tidy==22.1.8
```

```bash
clang-format --dry-run --Werror $(git ls-files '*.cpp' '*.hpp')
clang-tidy -p build/dev --warnings-as-errors='*' \
    --extra-arg="-isysroot$(xcrun --show-sdk-path)" \
    $(git ls-files 'src/*.cpp' 'tests/*.cpp' 'bench/*.cpp')
```

The `-isysroot` argument is a macOS detail. The pip-installed clang-tidy is not Apple's, so it does
not know where the SDK headers live, and without it every `#include <cstddef>` fails to parse and
the resulting cascade invents findings that are not real. CI runs on Linux and needs no equivalent.

## License

MIT.
