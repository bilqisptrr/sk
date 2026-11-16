#!/usr/bin/env python3
"""Exercise CLI validation and wall-clock timeouts without waiting indefinitely."""
import os
import subprocess
import sys
import tempfile
import time

binary = os.path.abspath(sys.argv[1])
with tempfile.TemporaryDirectory() as directory:
    config = os.path.join(directory, "search.txt")
    with open(config, "w") as f:
        f.write("Ninputs=8\nSymmetric=0\nRandomSeed=1234\nVerbosity=0\n")

    for args in (("--timeout",), ("--timeout", "0"), ("--timeout", "-1"),
                 ("--timeout", "abc"), ("--timeout", "1.5"),
                 ("--timeout", "99999999999999999999999999999"), ("--unknown", "1")):
        result = subprocess.run([binary, config, *args], stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE, timeout=5)
        assert result.returncode != 0, args

    start = time.monotonic()
    result = subprocess.run([binary, config, "--timeout", "1"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=8)
    elapsed = time.monotonic() - start
    assert result.returncode == 0 and b"Timeout reached" in result.stderr, result.stderr
    assert 0.8 <= elapsed < 8, elapsed

    # The legacy invocation must keep searching without a deadline.
    process = subprocess.Popen([binary, config], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.2)
        assert process.poll() is None, "search stopped without --timeout"
    finally:
        process.terminate()
        process.wait(timeout=5)

    # An expensive prefix-output enumeration must also respect the deadline.
    with open(config, "w") as f:
        f.write("Ninputs=26\nSymmetric=0\nRandomSeed=1234\nPrefixType=0\nVerbosity=0\n")
    start = time.monotonic()
    result = subprocess.run([binary, config, "--timeout", "1"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=8)
    elapsed = time.monotonic() - start
    assert result.returncode == 0 and b"Timeout reached" in result.stderr, result.stderr
    assert 0.8 <= elapsed < 8, elapsed

print("Timeout CLI: all tests passed")
