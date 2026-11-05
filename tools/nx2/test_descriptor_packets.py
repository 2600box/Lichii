#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build isolated production-seam tests; optionally time a synthetic descriptor workload.

Requires the repository's populated CPM cache and Vulkan platform headers, including
libx11-dev on Linux. No Vulkan device or driver calls are made.
The benchmark measures input preparation, packet copying, real CommandChunk recording,
thread handoff, bounded drains, encoding, and an ordered consumer. It does not measure FPS
or predict vkGetDescriptorEXT driver performance.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def dependency(root, name):
    candidates = sorted((root / ".cache/cpm" / name).glob("*/"))
    if len(candidates) != 1:
        raise SystemExit(f"Expected one cached {name} version, found: {candidates}")
    return candidates[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--sanitize", choices=("address", "thread"))
    parser.add_argument("--compiler", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--build-dir", type=Path)
    args = parser.parse_args()
    if args.benchmark and args.sanitize:
        parser.error("Benchmark without instrumentation; run sanitizers separately")

    root = Path(__file__).resolve().parents[2]
    catch = dependency(root, "catch2")
    boost = dependency(root, "boost")
    vulkan = dependency(root, "vulkanheaders")
    temporary = tempfile.TemporaryDirectory(prefix="lichii-descriptor-packets-")
    build = args.build_dir or Path(temporary.name)
    build.mkdir(parents=True, exist_ok=True)
    config = build / "generated/catch2/catch_user_config.hpp"
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text("#pragma once\n")
    includes = [root / "src", catch / "src", build / "generated", vulkan / "include"]
    if (boost / "boost").is_dir():
        includes.append(boost)
    else:
        includes.extend(sorted((boost / "libs").glob("*/include")))
        includes.extend(sorted((boost / "libs").glob("*/*/include")))
    flags = ["-std=c++20", "-O2", "-pthread"]
    if args.sanitize:
        sanitizers = "address,undefined" if args.sanitize == "address" else "thread"
        flags += [f"-fsanitize={sanitizers}", "-g", "-fno-omit-frame-pointer"]
    binary = build / "descriptor-packet-tests"
    command = shlex.split(args.compiler) + flags
    command += [f"-I{directory}" for directory in includes]
    command += [str(root / "src/tests/video_core/descriptor_buffer_packet.cpp"),
                str(catch / "extras/catch_amalgamated.cpp"), "-o", str(binary)]
    subprocess.run(command, check=True, cwd=root)
    subprocess.run([str(binary), "[descriptor_packet]"], check=True, cwd=root)
    if args.benchmark:
        subprocess.run([str(binary), "[descriptor_packet_benchmark]"], check=True, cwd=root)


if __name__ == "__main__":
    main()
