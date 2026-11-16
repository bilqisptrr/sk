#!/usr/bin/env python3
"""Smoke-test WebRTC search configs and independently check every reported network."""
import ast
import itertools
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

binary = os.path.abspath(sys.argv[1])
configs = Path(__file__).resolve().parent.parent / "configs" / "webrtc"
# Outputs not listed here are not guaranteed to be sorted.
cases = {
    "feedback_sort_4.txt": (4, None, False),
    "feedback_sort_8.txt": (8, None, False),
    "neteq_waiting_stats_8.txt": (8, (0, 3, 4, 7), False),
    "rtcp_clock_insert_7.txt": (7, None, True),
    "rtcp_clock_median_7.txt": (7, (3,), False),
    "video_order_3.txt": (3, None, False),
}


def apply(network, values):
    values = list(values)
    for lo, hi in network:
        if values[lo] > values[hi]:
            values[lo], values[hi] = values[hi], values[lo]
    return values


with tempfile.TemporaryDirectory() as directory:
    for name, (n, targets, insertion) in cases.items():
        # Keep the shipped config unseeded for normal independent searches.
        config = Path(directory) / name
        config.write_text((configs / name).read_text() + "\nRandomSeed=1234\n")
        result = subprocess.run([binary, str(config), "--timeout", "1"],
                                capture_output=True, text=True, timeout=10)
        if result.returncode:
            raise AssertionError(f"{name}: {result.stdout}\n{result.stderr}")
        networks = []
        for line in result.stdout.splitlines():
            if line.lstrip().startswith("{'N':"):
                item = ast.literal_eval(line.strip())
                assert item["N"] == n, (name, item)
                networks.append(item["nw"])
        assert networks, f"{name}: no network reported: {result.stdout}"
        # Exhaust every binary input or, for insertion, every valid presorted input.
        bits = itertools.product((0, 1), repeat=n)
        inputs = [v for v in bits if not insertion or list(v[:-1]) == sorted(v[:-1])]
        for network in networks:
            assert all(0 <= lo < hi < n for lo, hi in network), (name, network)
            for values in inputs:
                out = apply(network, values)
                expected = sorted(values)
                indices = range(n) if targets is None else targets
                assert all(out[i] == expected[i] for i in indices), (name, network, values)
            # Check non-binary values, duplicates, and negative values as well.
            rng = random.Random(1234)
            for _ in range(128):
                values = [rng.randrange(-5, 6) for _ in range(n)]
                if insertion:
                    values = sorted(values[:-1]) + values[-1:]
                out = apply(network, values)
                expected = sorted(values)
                indices = range(n) if targets is None else targets
                assert all(out[i] == expected[i] for i in indices), (name, network, values)
        # Compare complete records too: a wire must carry its payload along with
        # its key, rather than sorting timestamps/bitrates independently.
        rng = random.Random(456)
        for network in networks:
            for _ in range(128):
                if name.startswith("feedback_sort_"):
                    records = [((rng.randrange(3), rng.randrange(3), rng.randrange(3)), i)
                               for i in range(n)]
                    out = list(records)
                    for lo, hi in network:
                        if out[lo][0] > out[hi][0]:
                            out[lo], out[hi] = out[hi], out[lo]
                    assert [x[0] for x in out] == sorted(x[0] for x in records)
                    assert sorted(out) == sorted(records)  # tied keys may reorder
                elif name == "video_order_3.txt":
                    rates = [rng.randrange(3) for _ in range(n)]
                    # Original index is the tie-breaker required for stable sort.
                    records = [(rate, i) for i, rate in enumerate(rates)]
                    assert apply(network, records) == sorted(records)
        print(f"{name}: {len(networks)} network(s) verified")

print("WebRTC configurations: all tests passed")
