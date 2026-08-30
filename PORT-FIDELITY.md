# Port fidelity

The Python in [prediction-market-infra](https://github.com/anaborne/prediction-market-infra) is the
specification. Where this code and that code disagree about a byte, this code is wrong. This file
records the places where they disagree on purpose, so that a difference found later is either
listed here or is a defect.

Every behaviour attributed to Python below was observed by running it, under orjson 3.12.0, the
version that repository's `uv.lock` pins. None of it is inferred from documentation.

## What is byte identical

The frame is a 4-byte big-endian length prefix followed by the body. The body's key order is the
dataclass field-declaration order, because that is what orjson serializes, so reordering the members
of `WakeMessage` in `include/hotpath/protocol.hpp` is a wire change and not a cleanup.

Ten frames under `tests/golden/frames` were produced by running `ipc/protocol.py` itself and are
compared byte for byte, prefix included, by `tests/test_protocol_golden.cpp`. They cover a message
carrying only the required fields, so the post-v1 defaults are pinned at their exact wire values, a
fully populated v4 message, a "no"-side fire, strings exercising the whole escape table, floats
chosen to break a formatter, both ack statuses, and v1, v2, and v3 frames for the rolling-restart
path. `tests/golden/SOURCE.txt` records the checkout and the versions that produced them.

Float output is the one part no general-purpose C++ serializer gets right. orjson writes the
shortest digit string that round-trips, laid out in fixed notation when the decimal exponent is in
[-5, 16) and in scientific notation otherwise, always with a fractional digit in fixed notation
(`0.0`, `1000000000000000.0`) and never with one in scientific (`1e+16`, `1e-6`), with a signed
exponent that is not zero padded. That rule was checked against 349,927 values, 200,000 of them
random bit patterns, before any of this was written, and 1,000 of them are committed in
`tests/golden/doubles.tsv` as IEEE-754 bits beside the string orjson produced.

## Where this decoder is stricter than Python's

Six. All six exist because the Python dataclasses perform no runtime validation: `from_dict` is
`cls(**data)`, which checks the keys and nothing else. A C++ struct cannot hold the values Python
would accept here, so the choice is to reject the frame or to invent a coercion, and rejecting is
what produces a `rejected` ack the poller logs rather than an order nobody can explain.

1. `direction` must be `yes` or `no`. Python's `Literal["yes", "no"]` is not enforced at runtime and
   a frame carrying `"maybe"` decodes there.
2. `status` must be `accepted` or `rejected`, for the same reason.
3. Field types are enforced. A string where a number belongs, a fraction where an integer belongs,
   or a number where a string belongs is refused. Python accepts all three and fails later, if at
   all.
4. Integers must fit in an `int64`. orjson decodes up to an unsigned 64-bit integer as a Python
   `int` and anything larger as a float, so `9223372036854775808` and `18446744073709551616` both
   decode there and neither decodes here.
5. `price_ranges` entries must be exactly `[start, end, step]`. Python carries whatever list arrives
   and only unpacks the triple at fire time, inside `_snap_to_grid`, so a two-element entry decodes
   there and raises during dispatch instead.
6. A double that underflows is refused, where orjson returns a signed zero. This is the one case
   that rejects a frame orjson accepts outright rather than one it merely fails to check. It costs
   a second parse of the token to tell underflow from overflow through `std::from_chars`, and no
   price, probability, fee, or contract count reaches 1e-400.

## Where the two differ without one being stricter

The `reason` on a rejected `WakeAck` is not the same string. Python sends `str(exc)`, which is
CPython's `TypeError` text for the failed `cls(**data)` call. This sends its own message, naming the
same field. The poller only logs it (`poller_client.py`), so nothing downstream parses either.

Decoding then re-encoding a frame is not always the identity, in one case. A float field carrying an
integer literal, `"kalshi_price":1`, decodes in Python to the `int` 1 and re-encodes as `1`, while
this decodes it to the double 1.0 and re-encodes as `1.0`. Nothing re-encodes a decoded message in
either process, so this is recorded rather than fixed.

The encoder does not validate that the strings handed to it are UTF-8. orjson raises on a Python
`str` holding an unpaired surrogate. Every string the executor puts on the wire is either its own
ASCII or a `correlation_id` that came off a decoded frame, and decode validates, so the only way to
reach this is to construct a `WakeMessage` in C++ with invalid UTF-8 and encode it.

## Behaviours matched on purpose

These are the ones a JSON parser written without reading orjson's would have got wrong, and each has
a test in `tests/test_protocol.cpp`:

- A duplicate key takes the last value.
- An unexpected key is refused, so a schema version bump changes what decodes during a rolling
  restart.
- A missing required field is refused and named.
- `01`, `.5`, `1.`, `NaN`, and `Infinity` are refused. `1e+3` and `-0.0` are accepted.
- Content after the document is refused. Trailing whitespace is not content.
- A raw control character inside a string is refused. Its escape is not.
- Invalid UTF-8 is refused, including an overlong encoding, an encoded surrogate, and a code point
  above U+10FFFF.
- An unpaired surrogate escape is refused. A correct surrogate pair decodes to one code point.
- A double that overflows to infinity is refused.

## The executor server

`ExecutorServer` ports `ipc/executor_server.py` as far as the point where that file calls
`dispatch()`. The read loop's order is the Python's: read the body, stamp, decode, ack, fire. The
stamp is taken after the read returns rather than before it, because the read blocks until a frame
arrives and a stamp ahead of it measures the gap between wakes. `tests/test_executor.cpp` holds the
ack ahead of the fire by making the dispatch callback block until the client has the ack.

`_snap_to_grid` is matched including its rounding. Python's `round()` is half-to-even and
`std::round` is half-away-from-zero, so `round_half_even` transcribes CPython's `float.__round__`
for `ndigits=None`. 424 cases in `tests/golden/snap_to_grid.tsv` come from running that function
out of `executor_server.py`, and moving the implementation to `std::round` fails ten of them.

Four differences, all of them shape rather than behaviour on a frame:

1. One connection at a time. The Python serves connections concurrently on the event loop. The
   poller opens one, and the benchmark drives one wake at a time.
2. A fire runs inline on the read loop, where the Python spawns `asyncio.create_task()` per fire so
   a slow `dispatch()` never blocks the next read. The REST client here is the same fake
   `benchmarks/latency_bench.py` uses, which returns without doing any work.
3. The stale-socket unlink before bind is unguarded. The Python entry point holds a single-instance
   `flock` before `serve_forever` runs, so the file it removes can only be a crashed executor's.
   Two of these racing at the same path would each unlink the other's socket.
4. A template missing from the prebuilt map is built and inserted at fire time rather than refused.
   `build_template` validates its `outcome_side` against the same literal set the telemetry schema
   enforces; `Direction` is an enum here and cannot hold anything else.

Position sizing, the risk gate, the `orders_fired` and `latency_events` writes, and the dispatch
itself are not ported. Sizing and the risk gate are outside this repository's scope, the telemetry
ring is step 4, and the benchmark harness that fills `record_wake_recv` is step 6.
