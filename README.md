# executor-hotpath-cpp

The executor process from
[prediction-market-infra](https://github.com/anaborne/prediction-market-infra), rewritten in C++20
against the same wire protocol, so the Python poller can drive either one and the two can be timed
against each other in the same run on the same machine.

## Status: scaffold and gates. No executor yet.

What is here is the build, the test harness, the linters, and CI on Linux and macOS with an
AddressSanitizer and UndefinedBehaviorSanitizer job. The protocol, the server, the telemetry ring,
and the signer are not written. [`BENCHMARK.md`](BENCHMARK.md) records what will be measured and
what is expected, written before any of it exists.

## Build

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

## License

MIT.
