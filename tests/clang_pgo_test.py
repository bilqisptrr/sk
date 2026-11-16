#!/usr/bin/env python3
"""Exercise the manual Clang PGO workflow in an isolated temporary directory."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

compiler, profdata = sys.argv[1:]
root = Path(__file__).resolve().parent.parent
sources = ["makefile", "SorterHunter.cpp", "ConfigParser.cpp", "hutils.cpp",
           "prefix_processor.cpp", "ConfigParser.h", "hutils.h", "htypes.h",
           "prefix_processor.h", "timeout.h"]
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    for name in sources:
        shutil.copy2(root / name, work / name)
    (work / "small.conf").write_text(
        "Ninputs=4\nSymmetric=0\nRandomSeed=1234\nVerbosity=0\n")

    def make(target, expect_success=True):
        result = subprocess.run(["make", target, f"CXX={compiler}",
                                 f"LLVM_PROFDATA={profdata}"], cwd=work,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=120)
        if (result.returncode == 0) != expect_success:
            raise AssertionError(f"make {target} failed unexpectedly:\n{result.stdout}")
        return result.stdout

    # An existing non-instrumented build must be replaced by pgo-gen.
    make("all")
    make("pgo-gen")
    env = os.environ.copy()
    env["LLVM_PROFILE_FILE"] = str(work / "default.profraw")
    subprocess.run([str(work / "SorterHunter"), "small.conf", "--timeout", "1"],
                   cwd=work, env=env, stdout=subprocess.DEVNULL, check=True, timeout=10)
    assert (work / "default.profraw").stat().st_size > 0
    assert not (work / "default.profdata").exists()
    output = make("pgo-use")
    assert "Merging default.profraw" in output
    assert (work / "default.profdata").stat().st_size > 0

    # A newer raw profile must replace the old indexed profile.
    old = time.time() - 60
    os.utime(work / "default.profdata", (old, old))
    output = make("pgo-use")
    assert "Merging default.profraw" in output

    # Already merged profiles work without a raw file.
    (work / "default.profraw").unlink()
    make("pgo-use")

    # Missing profiles fail before cleaning an existing executable.
    (work / "default.profdata").unlink()
    output = make("pgo-use", expect_success=False)
    assert "no Clang PGO profile found" in output
    assert (work / "SorterHunter").is_file()

print("Clang manual PGO workflow: all tests passed")
