# executor-hotpath-cpp

The executor process from
[prediction-market-infra](https://github.com/anaborne/prediction-market-infra), rewritten in C++20
against the same wire protocol, so the Python poller can drive either one and the two can be timed
against each other in the same run on the same machine.

The Python side is a published reference implementation, not a running system.
`prediction-market-infra` is an extraction from a private trading bot, and that bot was shut down
on 2026-08-29 when the last surviving strategy failed its pre-registered replication. The
infrastructure was published; the trading was not continued. The comparison does not rest on it
still trading. It is two implementations of one wire protocol, timed on one machine in one run,
with `poller_client.py` driving both unchanged. What is not on offer is a production A/B, and no
number here will be presented as one.

## Status: the harness runs on both sides. No numbers reported yet.

`WakeMessage`, `WakeAck`, the length-prefixed frame codec, a JSON layer written against orjson's
output rather than against the JSON grammar alone, the executor server from `accept` to the point
where the Python calls `dispatch()`, the telemetry path that carries `wake_recv` off that server
and into SQLite, the Kalshi request signer, and the benchmark harness on both sides of the wire.

No figure is reported anywhere in this repository yet. The run happens on one machine in one
session and RESULTS.md is written the same day. [`BENCHMARK.md`](BENCHMARK.md) records what will be
measured, how, and what is expected, written before any of it existed and corrected in place at the
bottom rather than edited above.

The encoder is byte identical to `orjson.dumps` over these two dataclasses, and ten frames produced
by running the Python itself hold it there. [`PORT-FIDELITY.md`](PORT-FIDELITY.md) records what is
identical, the six places this decoder is stricter than Python's, and the evidence behind each,
because every one of those was observed by running the Python and not inferred from its source.

orjson's float formatting is where a general-purpose C++ serializer differs. It writes shortest
round-trip digits in fixed notation for a decimal exponent in [-5, 16) and scientific outside it, so
`1e15` is `1000000000000000.0` and `1e16` is `1e+16`. `tests/golden/doubles.tsv` pins 1,000 values
against orjson's own output.

The server is one thread on one Unix domain socket, and the read loop runs in the Python's order:
read the body, stamp, decode, ack, fire. The stamp comes after the read returns, since the read
blocks until a frame arrives and a stamp taken ahead of it measures the gap between wakes. The ack
goes out before the fire, so a dispatch never lands inside the span the poller measures. Price
snapping matches Python's half-to-even `round()` against 424 cases the Python produced.

## Telemetry

`--telemetry-db` writes `latency_events` rows into the same SQLite file and the same columns
`benchmarks/latency_bench.py` queries, so the Python harness reads the C++ executor's numbers with
its own `SELECT` unchanged. A `record` call copies the row into a fixed-size ring and returns; one
writer thread owns the connection and commits in batches of 500.

Nothing on the read loop can wait for a write. The ring holds 8192 rows, pushing to a full one
drops the row rather than blocking, and the four counters are printed at exit next to the server's:

```
frames=5 accepted=5 rejected=0 fired=5 refused_price=0 refused_kill_switch=0
telemetry rows_written=5 dropped_ring_full=0 dropped_oversized_id=0 dropped_write_failed=0
```

A writer that fell behind shows up as `rows_written` short of `frames`. That is why the drops are
counted rather than logged: a log line is something to go looking for, and a run that silently
under-reported would look like a run that was quiet.

Those five wakes came from the Python's own `protocol.py`. Opening the file the C++ wrote with the
Python's own `TelemetryDB.initialize()` afterwards fills in the seven tables the executor does not
write and leaves all five rows in place.

```bash
./build/dev/bin/executor_hotpath --socket /tmp/executor.sock --kill-switch /tmp/halt \
    --telemetry-db /tmp/telemetry.db
```

## Signer

RSA-PSS over `timestamp + method + path`, MGF1 and the digest both SHA-256, salt length equal to
the digest length, base64 out, through OpenSSL 3's EVP interface. Kalshi's published construction,
and `auth/signer.py` is the specification for it.

PSS salts randomly, so signing one message twice with one key gives two different valid signatures
and there is no golden signature to compare bytes against. The check runs both ways instead.
`tests/golden/signing` holds five signatures the Python produced against a throwaway RSA-2048 key
and the public key that verifies them, and the test suite verifies each one against the message
this port builds. A signature carrying OpenSSL's default salt, 222 bytes rather than 32, fails that
same verifier, which is what keeps the assertion from being vacuous.

The other direction is a command rather than a gate, because gating it would put `cryptography` on
all three CI runners to check a value that is different every run:

```bash
uv run python tests/golden/generate_signing_fixture.py --verify-cpp build/dev/signer_cross_check
```

```
5 C++ signatures verified against the Python's own PSS parameters
```

No private key is committed. Both sides generate one into a temporary directory and only the public
half is written out. [`PORT-FIDELITY.md`](PORT-FIDELITY.md) records the two differences and the one
caveat the fixture does not close.

## Benchmarks

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
[`PORT-FIDELITY.md`](PORT-FIDELITY.md) has that and the rest of what the harnesses do differently.

## Build

SQLite and OpenSSL 3 are the external dependencies and both come from the system: macOS ships
SQLite and Homebrew's `openssl@3` supplies the rest, Ubuntu needs `libsqlite3-dev` and
`libssl-dev`. Catch2 comes from FetchContent.

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --no-tests=error
```

Under sanitizers. `asan-ubsan` is what CI runs on Linux, `ubsan` is what runs on this Mac:

```bash
cmake --preset ubsan
cmake --build --preset ubsan
ctest --preset ubsan --no-tests=error
```

AddressSanitizer does not run on macOS here. On Darwin 25.5 with Apple clang 17 an
ASan-instrumented `int main(){return 0;}` hangs before reaching `main`. That is a platform problem
with no local workaround. UndefinedBehaviorSanitizer
works, which is why it gets its own preset. ASan and LeakSanitizer are exercised by the Linux job
in CI, and only those results are believed.

## Linters

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

## Regenerating the fixtures

Nothing in CI regenerates them. They are committed, and changing one is a deliberate act with a
diff to review:

```bash
uv run --with orjson python tests/golden/generate_golden.py --infra ../prediction-market-infra
uv run --with cryptography python tests/golden/generate_signing_fixture.py \
    --infra ../prediction-market-infra
```

Regenerating the frames against `prediction-market-infra` at `e3fd937` under Python 3.14.7
reproduced all ten frames, all 1,000 doubles and all 424 snap cases byte for byte. The percentile
vectors came out of the same run.

## License

MIT.
