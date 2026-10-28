#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Unit test runner: build Zynq-A9 test firmware, run in QEMU.
# Uses UART output markers (ZYNQ_TEST_EXIT:PASS/FAIL) for result detection.

import os
import subprocess
import sys

PROJECT_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..")
)
BUILD_DIR = os.path.join(PROJECT_ROOT, "build", "zynq-a9-qemu")
CROSS_FILE = os.path.join(PROJECT_ROOT, "platform", "zynq-a9", "cross.ini")
QEMU_TIMEOUT = 120


def build():
    """Build Zynq-A9 firmware with unit test target."""
    if not os.path.isfile(os.path.join(BUILD_DIR, "build.ninja")):
        subprocess.check_call(
            ["meson", "setup", BUILD_DIR,
             "--cross-file", CROSS_FILE],
            cwd=PROJECT_ROOT,
        )

    print(">>> Building unit test firmware (zynq-a9) ...")
    subprocess.check_call(
        ["meson", "compile", "-C", BUILD_DIR],
        cwd=PROJECT_ROOT,
    )


def run_qemu():
    """Run unit test ELF in QEMU, parse output for PASS/FAIL marker."""
    kernel = os.path.join(
        BUILD_DIR, "platform", "zynq-a9", "rtclaw_test.elf"
    )
    if not os.path.isfile(kernel):
        print(f"Test ELF not found: {kernel}")
        return 1

    cmd = [
        "qemu-system-arm",
        "-M", "xilinx-zynq-a9",
        "-smp", "1",
        "-nographic",
        "-kernel", kernel,
    ]

    print(f">>> Running QEMU (timeout={QEMU_TIMEOUT}s) ...")
    try:
        result = subprocess.run(
            cmd,
            timeout=QEMU_TIMEOUT,
            capture_output=True,
            text=True,
        )
        output = result.stdout
        print(output, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
    except subprocess.TimeoutExpired as e:
        output = e.stdout.decode() if e.stdout else ""
        print(output, end="")

    if "ZYNQ_TEST_EXIT:PASS" in output:
        return 0
    elif "ZYNQ_TEST_EXIT:FAIL" in output:
        return 1
    else:
        print("ERROR: test did not produce exit marker")
        return 124


def main():
    try:
        build()
    except subprocess.CalledProcessError:
        print("Build failed")
        return 1

    rc = run_qemu()
    if rc == 0:
        print("\n>>> Unit tests PASSED (zynq-a9)")
    else:
        print(f"\n>>> Unit tests FAILED (rc={rc})")
    return rc


if __name__ == "__main__":
    sys.exit(main())
