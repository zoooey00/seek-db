#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Compile and run hot-path checks using an existing CMake compilation database.

First build seekdb and oceanbase from this checkout. Then run:
python3 unittest/storage/fts/run_fts_cpp.py --build-dir /path/to/build --output /path/to/evidence
The compiler, include paths and ABI flags are taken from compile_commands.json.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shlex
import subprocess

REFERENCE_SHA256 = "d26f03bfb9eb494a5259e1345744369473d7de0b781831e0492d16add2db6ca8"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build-dir", required=True, type=pathlib.Path)
    p.add_argument("--output", required=True, type=pathlib.Path)
    p.add_argument("--runtime-library", type=pathlib.Path)
    p.add_argument("--delta-dir", type=pathlib.Path, help=argparse.SUPPRESS)
    args = p.parse_args()
    repo = pathlib.Path(__file__).resolve().parents[3]
    build = args.build_dir.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # Unmodified oracle from official commit 5bce9e0, retained as a fixture
    # because the destination repository imported the competition as a squash.
    reference = (repo/"unittest/storage/fts/reference_ik_char_util.h").read_bytes()
    if hashlib.sha256(reference).hexdigest() != REFERENCE_SHA256:
        raise SystemExit("Reference classifier fixture checksum mismatch")
    (out/"reference_ik_char_util.h").write_bytes(reference)
    commands = json.loads((build/"compile_commands.json").read_text())
    template = next(c for c in commands if "ob_storage_fts" in c["file"] and "Unity" in c["file"])
    command = shlex.split(template["command"])
    # Reuse generated build headers, and this checkout's source headers.
    source_root = next(pathlib.Path(x[2:]) for x in command if x.startswith("-I") and (pathlib.Path(x[2:])/"AGENTS.md").exists())
    command = [x.replace(str(source_root), str(repo)) for x in command]
    command[command.index("-c")+1] = str(repo/"unittest/storage/fts/test_fts_hot_path.cpp")
    obj = out/"test_fts_hot_path.o"
    command[command.index("-o")+1] = str(obj)
    command.insert(1, "-I"+str(out))
    compiler = pathlib.Path(command[0])
    library = args.runtime_library or build/"src/observer/liboceanbase.so"
    binary = out/"test_fts_hot_path"
    objects = [str(obj)]
    if args.delta_dir:
        objects += [str(x) for x in sorted(args.delta_dir.glob("*.o"))]
    toolchain = next(x for x in command if x.startswith("--gcc-toolchain="))
    link = [str(compiler), toolchain, "-fuse-ld="+str(compiler.parent/"ld.lld"), *objects,
            str(library), "-pthread", "-ldl", "-lm", "-Wl,--threads=2",
            "-Wl,-rpath,"+str(library.parent), "-Wl,-rpath,"+str(compiler.parent.parent/"lib64"),
            "-o", str(binary)]
    records = []
    for name, cmd in [("compile", command), ("link", link), ("run", [str(binary)])]:
        with (out/(name+".log")).open("w") as log:
            result = subprocess.run(cmd, cwd=template["directory"], stdout=log, stderr=subprocess.STDOUT)
        records.append({"step": name, "exit_code": result.returncode, "command": cmd})
        print(name, result.returncode, flush=True)
        print((out/(name+".log")).read_text()[-5000:], flush=True)
        (out/"result.json").write_text(json.dumps(records, indent=2))
        if result.returncode:
            raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
