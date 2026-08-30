"""Regenerate the golden frames and the double-formatting corpus from the Python implementation.

The C++ encoder is only worth anything if its output is the bytes `orjson` produces, so the
expectation cannot be written by hand in the C++ test: it has to come from a run of the real
`ipc/protocol.py`. This script imports that module out of a prediction-market-infra checkout by
path, encodes a fixed set of messages, and writes each frame whole, length prefix included.

    uv run --with orjson python tests/golden/generate_golden.py --infra ../prediction-market-infra

`doubles.tsv` is the other half. It pairs a double, written as its IEEE-754 bits so no decimal text
sits between the value and the test, with the string `orjson.dumps` produces for it. The C++ side
reproduces that string from `std::to_chars` shortest digits and orjson's fixed-versus-scientific
threshold, and this file is what holds the reproduction honest.

Nothing here runs in CI. The outputs are committed, and regenerating them is a deliberate act with
a diff to review.
"""

from __future__ import annotations

import argparse
import importlib.util
import math
import random
import struct
import subprocess
import sys
from datetime import date
from pathlib import Path
from types import ModuleType

DOUBLE_SAMPLE_COUNT = 1000
DOUBLE_SEED = 20260829


def load_protocol(infra_root: Path) -> ModuleType:
    """Import `kalshi_bot.ipc.protocol` from a checkout, without installing the package.

    The module imports `orjson` and stdlib only, so loading it by path is enough and avoids
    pulling aiohttp/uvloop/prefect in to encode twenty fields.
    """
    path = infra_root / "src" / "kalshi_bot" / "ipc" / "protocol.py"
    spec = importlib.util.spec_from_file_location("kalshi_bot_ipc_protocol", path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    # `dataclasses` resolves annotations through `sys.modules[cls.__module__]`, so a module loaded
    # by path has to be registered before it executes or every `@dataclass` in it raises.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def wake_message_cases(protocol: ModuleType) -> dict[str, object]:
    message = protocol.WakeMessage
    version = protocol.SCHEMA_VERSION

    required = {
        "schema_version": version,
        "correlation_id": "0f9e6a2c-7d41-4b8e-9a55-1c2d3e4f5a6b",
        "market_ticker": "KXBTCD-26AUG2917-T112750",
        "asset": "BTC",
        "direction": "yes",
        "kalshi_price": 0.56,
        "model_probability": 0.62,
        "fee": 0.02,
        "edge": 0.06,
        "decision_ts_ms": 1787289600123,
        "sent_at_ms": 1787289600124,
        "sent_at_ns": 412737100845213,
    }

    return {
        # Every post-v1 field left alone, so the frame pins the defaults themselves. A change to
        # any of them in Python shows up here as a byte diff and not as a behaviour surprise.
        "wake_defaults": message(**required),
        "wake_full_yes": message(
            **required,
            recv_ns=412737100311907,
            clock_domain="Ivory.local:1787289000",
            dry_run=True,
            wire_price_yes_dollars=0.56,
            exchange_index=2,
            available_size_contracts=143.0,
            price_ranges=[[0.0, 0.05, 0.001], [0.05, 0.95, 0.01], [0.95, 1.0, 0.001]],
            correlation_group="btc-majors",
        ),
        # A "no" fire: `kalshi_price` is 1 - yes_bid and the wire price is the YES-side price, the
        # asymmetry that the wrong-side bug in the parent project came out of.
        "wake_no_side": message(
            **{**required, "direction": "no", "kalshi_price": 0.30},
            recv_ns=0,
            clock_domain="",
            dry_run=False,
            wire_price_yes_dollars=0.70,
            exchange_index=-1,
            available_size_contracts=0.0,
            correlation_group="",
        ),
        # Strings that exercise the escape table on both sides: a quote, a backslash, the five
        # shortcut escapes, a control character that has none, and multi-byte UTF-8 including a
        # code point above the BMP.
        "wake_escapes": message(
            **{
                **required,
                "correlation_id": 'quote:" backslash:\\ tab:\t newline:\n',
                "asset": "\x00\x1f\b\f\r",
                "market_ticker": "café € \U0001f680",
            },
            clock_domain="host:ü",
            correlation_group="中文",
        ),
        # Every float field carrying a value that is not round in binary, so the golden fails if
        # the C++ formatter rounds, truncates, or picks the wrong notation.
        "wake_awkward_floats": message(
            **{
                **required,
                "kalshi_price": 0.1 + 0.2,
                "model_probability": 1 / 3,
                "fee": 1e-06,
                "edge": -0.0,
            },
            available_size_contracts=1e16,
            wire_price_yes_dollars=0.9999999999999999,
            price_ranges=[[0.0, 1.0, 0.01], [1e-05, 5e-324, 1.7976931348623157e308]],
        ),
    }


def wake_ack_cases(protocol: ModuleType) -> dict[str, object]:
    ack = protocol.WakeAck
    version = protocol.SCHEMA_VERSION
    return {
        "ack_accepted": ack(
            schema_version=version,
            correlation_id="0f9e6a2c-7d41-4b8e-9a55-1c2d3e4f5a6b",
            received_at_ms=1787289600125,
            status="accepted",
            reason=None,
        ),
        "ack_rejected": ack(
            schema_version=version,
            correlation_id="",
            received_at_ms=1787289600125,
            status="rejected",
            reason='WakeMessage.__init__() got an unexpected keyword argument \'wat\'',
        ),
    }


def legacy_bodies(orjson: ModuleType) -> dict[str, bytes]:
    """Frames from before the fields that now carry defaults existed.

    Written as raw dicts rather than through the dataclass, because the whole point is a body the
    current dataclass could not produce. The executor has to read these during the window of a
    rolling restart where the poller is on the older ref.
    """
    v1 = {
        "schema_version": 1,
        "correlation_id": "corr-v1",
        "market_ticker": "KXBTCD-T100",
        "asset": "BTC",
        "direction": "yes",
        "kalshi_price": 0.56,
        "model_probability": 0.62,
        "fee": 0.02,
        "edge": 0.06,
        "decision_ts_ms": 1000,
        "sent_at_ms": 1001,
        "sent_at_ns": 1001000000,
    }
    v2 = {
        **v1,
        "schema_version": 2,
        "correlation_id": "corr-v2",
        "recv_ns": 5000000000,
        "clock_domain": "host-a:1787290000",
        "dry_run": False,
    }
    v3 = {
        **v2,
        "schema_version": 3,
        "correlation_id": "corr-v3",
        "wire_price_yes_dollars": 0.56,
        "exchange_index": 2,
        "available_size_contracts": 0.0,
        "price_ranges": [[0.0, 1.0, 0.01]],
    }
    return {
        name: struct.pack(">I", len(body)) + body
        for name, body in (
            (f"legacy_v{n}", orjson.dumps(d)) for n, d in ((1, v1), (2, v2), (3, v3))
        )
    }


def double_corpus() -> list[float]:
    """Curated boundaries first, then a seeded sample, so a diff on this file is readable.

    The boundaries are where the layout rule can break: the two exponents that switch notation,
    the smallest subnormal, the largest finite double, a negative zero, and values whose shortest
    round-trip digit string is 17 digits long.
    """
    values = [
        0.0,
        -0.0,
        1.0,
        -1.0,
        0.01,
        0.56,
        0.1 + 0.2,
        1 / 3,
        1e-5,
        1e-6,
        9.999999999999999e15,
        1e16,
        1e15,
        1.2345678901234567e15,
        5e-324,
        2.2250738585072014e-308,
        1.7976931348623157e308,
        1e21,
        1.2345e-7,
        0.9999999999999999,
    ]
    rng = random.Random(DOUBLE_SEED)
    while len(values) < DOUBLE_SAMPLE_COUNT:
        # Half from the wire's own domain (prices, probabilities, contract counts, timestamps in
        # seconds), half from the whole double range by random bit pattern, which is the only way
        # to reach the exponents a price never visits.
        if len(values) % 2 == 0:
            values.append(rng.uniform(0.0, 1.0))
        else:
            candidate = struct.unpack("<d", struct.pack("<Q", rng.getrandbits(64)))[0]
            if math.isfinite(candidate):
                values.append(candidate)
    return values


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--infra", type=Path, required=True, help="prediction-market-infra checkout"
    )
    args = parser.parse_args()

    import orjson

    protocol = load_protocol(args.infra)
    here = Path(__file__).parent
    frames = here / "frames"
    frames.mkdir(exist_ok=True)

    cases: dict[str, bytes] = {}
    for name, payload in (wake_message_cases(protocol) | wake_ack_cases(protocol)).items():
        cases[name] = protocol.encode_frame(payload)
    cases |= legacy_bodies(orjson)

    for name, frame in cases.items():
        (frames / f"{name}.frame").write_bytes(frame)

    lines = [
        f"{struct.unpack('<Q', struct.pack('<d', value))[0]:016x}\t{orjson.dumps(value).decode()}"
        for value in double_corpus()
    ]
    (here / "doubles.tsv").write_text("\n".join(lines) + "\n")

    revision = subprocess.run(
        ["git", "-C", str(args.infra), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    (here / "SOURCE.txt").write_text(
        "\n".join(
            [
                "Generated by generate_golden.py. Do not edit by hand.",
                "",
                f"date: {date.today().isoformat()}",
                f"prediction-market-infra: {revision}",
                f"orjson: {orjson.__version__}",
                f"python: {sys.version.split()[0]}",
                f"schema_version: {protocol.SCHEMA_VERSION}",
                f"frames: {len(cases)}",
                f"doubles: {len(lines)}",
            ]
        )
        + "\n"
    )

    print(f"wrote {len(cases)} frames and {len(lines)} doubles")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
