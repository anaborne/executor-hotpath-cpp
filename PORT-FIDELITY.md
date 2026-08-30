# Port fidelity

The Python in [prediction-market-infra](https://github.com/anaborne/prediction-market-infra) is the
specification. Where this code and that code disagree about a byte, this code is wrong. This file
records the places where they disagree on purpose, so that a difference found later is either
listed here or is a defect.

Every behaviour attributed to Python below was observed by running it, under orjson 3.12.0, the
version that repository's `uv.lock` pins. None of it is inferred from documentation.

That repository is frozen. The private system it was extracted from was shut down on 2026-08-29,
so the protocol this port tracks is a fixed target rather than one moving under it.
`tests/golden/SOURCE.txt` and `tests/golden/signing/SOURCE.txt` record the commit each fixture was
generated from.

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

Position sizing, the risk gate, the `orders_fired` write, and the dispatch itself are not ported.
Sizing and the risk gate are outside this repository's scope, and the benchmark harness that drives
the whole thing is step 6.

## The telemetry path

`TelemetrySink` ports `telemetry/db.py`, restricted to `latency_events` and the `correlations`
parent row its foreign key requires. A `record` call copies the row into a bounded ring and
returns; a writer thread owns the SQLite connection, drains up to 500 rows, and commits once. That
is the shape `db.py` already has, for the reason its own docstring gives: a telemetry write must
never be able to delay the path it is observing.

A row written by this process and a row written by the Python are the same row. Same six bound
columns in the same order, `id` and `created_at_ms` left to the table's own defaults;
`metadata_json` is the pre-serialized `{"dry_run": false}` / `{"dry_run": true}` pair from
`executor_server.py::_DRY_RUN_METADATA` rather than a serializer call; and the
`INSERT OR IGNORE INTO correlations` that `db.py::_insert` performs ahead of any child row runs
here too, so the foreign key means the same thing on both sides. Five wakes driven through
`ipc/protocol.py` into this binary come back out of
`SELECT duration_ms FROM latency_events WHERE stage = 'wake_recv'`, which is `latency_bench.py`'s
query with nothing changed.

Four differences.

1. The stage vocabulary is an enum. `record_latency_event()` validates a string against
   `LATENCY_STAGES` and raises, because a mistyped stage there produced a silent hole in the exact
   data the stage was added to collect. `LatencyStage` cannot hold a name that is not on the list,
   so the check has nowhere to live and the failure mode it guards against does not exist.
2. `correlations` and `latency_events` are the only tables this process creates, out of the nine in
   `schema.sql`. It writes no others, and transcribing seven table definitions it never touches
   would leave two copies of a schema to keep in step. `schema.sql` is
   `CREATE TABLE IF NOT EXISTS` throughout, so a Python `initialize()` against the same file fills
   in the rest afterwards.
3. `PRAGMA user_version` is stamped at 6, `migrations.py::SCHEMA_VERSION`, and only on a database
   this process created. That is the branch `initialize()` takes on a database `schema.sql` has
   just built. Stamping zero instead sends the next Python open into `_add_order_fill_columns`,
   which alters an `orders_fired` that does not exist yet; the open then raises
   `OperationalError: no such table: orders_fired`, which was confirmed by running it. The constant
   is a hand-copy of a number that lives in that module, so appending a migration there and not
   here is a defect this file will not catch.
4. A row is dropped for three reasons and each has its own counter, printed at exit. `db.py` drops
   on a full queue and logs at most one warning per ten seconds, since a saturated queue drops
   thousands of rows a second. This drops on a full ring, on a `correlation_id` longer than the
   128-byte slot, and on an insert SQLite refuses. The long id is the one case with no Python
   counterpart: `uuid4().hex` is 32 bytes and the benchmark's labels are shorter, so nothing
   either process generates comes close, and the row is dropped rather than truncated because a
   truncated id still satisfies the foreign key and still joins, to the wrong parent.

Three smaller ones. Both inserts are prepared once at open rather than re-prepared per row, where
`sqlite3` leans on its own statement cache. The writer polls the ring on a 500 microsecond interval
rather than blocking on a queue, because a condition variable would put the wake-up cost on the
producer, which is the thread being measured. And a `record` call before `open()` is accepted into
the ring and written once the writer starts, where `_enqueue` raises `RuntimeError`; a sink that is
never opened reports those rows as queue depth at exit rather than losing them silently.

## The signer

`RequestSigner` ports `auth/signer.py`. RSA-PSS over `timestamp + method + path` concatenated with
no separator, SHA-256 as the digest and inside MGF1, salt length equal to the digest length, base64
with no line breaks. `sign_websocket_auth` signs the fixed `GET` and `/trade-api/ws/v2` pair and
keeps its own entry point, for the reason that file gives: no caller reaches for `sign` and
authenticates a socket with a REST path.

PSS salts randomly, so one message signed twice with one key yields two valid signatures and
neither is the expected value of the other. What replaces a byte comparison runs in both
directions. `tests/golden/signing` holds a public key and five signatures the real `signer.py`
produced against a throwaway RSA-2048 key, and `tests/test_signer.cpp` verifies every one of them
against the message this port builds, through a verifier written straight against OpenSSL rather
than through anything in `src/signer.cpp`. That pins the message construction, the digest, the MGF
and the salt length: a signature carrying OpenSSL's own default salt, 222 bytes, the widest an
RSA-2048 modulus allows, fails that verifier, and a test asserts it rather than assuming it.

The other direction runs on demand:

```bash
uv run python tests/golden/generate_signing_fixture.py --verify-cpp build/dev/signer_cross_check
```

It reads what the test wrote on its last run and puts each signature through `cryptography`'s own
PSS verifier. `signing/SOURCE.txt` carries the date that last passed. It is not a CI gate, and the
reason belongs here rather than nowhere: gating it would install `cryptography` on all three
runners to check a value that differs every run, while the committed direction already fails on any
disagreement about the four parameters. One caveat that the gate does not close. `cryptography` is
OpenSSL underneath, so both verifiers share an implementation, and what the fixture proves is
agreement about the parameters and the message rather than agreement between two independent
implementations of PSS.

Two differences.

1. No private key enters this repository. `tests/auth/test_signer.py` generates one per test into
   `tmp_path` and the fixture generator does the same, writing out only the public half, so a
   checkout carries no PEM for a secret scanner to rule on. Committed: one public key and five
   signatures.
2. A key that will not load comes back from `load()` as a message, where `__init__` raises
   `TypeError` for a non-RSA key and lets `cryptography` raise for the rest. A signature that will
   not compute from a key that already loaded throws, as `_sign_message` raises. The split is that
   the first is an operator handing the process the wrong file and the second is an allocation or
   the RNG failing, which has no caller-side recovery to write.

One mechanical note. `load_pem_private_key(..., password=None)` raises on an encrypted PEM;
OpenSSL's default callback reads a passphrase from the terminal instead, so the callback here
refuses, and an executor started by launchd with an encrypted key fails rather than hanging on a
prompt no one is watching.

The signer is not wired into the executor's fire path, on either side. Signing happens inside
`transport/rest_client.py`, past the `dispatch()` boundary step 3 stopped at, and
`benchmarks/latency_bench.py` times `sign()` standalone over 2000 iterations against a throwaway
key. Step 6 mirrors that in `bench/`.

