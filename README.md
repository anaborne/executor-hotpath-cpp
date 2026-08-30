# executor-hotpath-cpp

The executor process from
[prediction-market-infra](https://github.com/anaborne/prediction-market-infra), rewritten in C++20
against the same wire protocol, so the Python poller can drive either one and the two can be timed
against each other in the same run on the same machine.

## Status: protocol, server, telemetry. No signer.

`WakeMessage`, `WakeAck`, the length-prefixed frame codec, a JSON layer written against orjson's
output rather than against the JSON grammar alone, the executor server from `accept` to the point
where the Python calls `dispatch()`, and the telemetry path that carries `wake_recv` off that
server and into SQLite. The signer is not written. [`BENCHMARK.md`](BENCHMARK.md) records what will
be measured and what is expected, written before any of it exists.

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

## Build

SQLite is the one external dependency and comes from the system: macOS ships it, and Ubuntu needs
`libsqlite3-dev`. Catch2 comes from FetchContent.

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
    --extra-arg="-isysroot$(xcrun --show-sdk-path)" $(git ls-files 'src/*.cpp' 'tests/*.cpp')
```

The `-isysroot` argument is a macOS detail. The pip-installed clang-tidy is not Apple's, so it does
not know where the SDK headers live, and without it every `#include <cstddef>` fails to parse and
the resulting cascade invents findings that are not real. CI runs on Linux and needs no equivalent.

## Regenerating the golden frames

Nothing in CI regenerates them. They are committed, and changing one is a deliberate act with a
diff to review:

```bash
uv run --with orjson python tests/golden/generate_golden.py --infra ../prediction-market-infra
```

## License

MIT.
