#!/usr/bin/env python3
"""Reproduce synthetic streaming probes without downloads or persistent build files."""
import argparse
import hashlib
import json
import os
import resource
from pathlib import Path
import signal
import subprocess
import tempfile
import time


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sunshine-source", type=Path, default=repo / "host/sunshine-build")
    parser.add_argument("--fec-only",action="store_true",help="Skip all GPU operations")
    parser.add_argument("--tiled-fec",action="store_true",help="Compare register-blocked GFNI parity with byte-identical native parity")
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    source = args.sunshine_source.resolve()
    nanors = source / "third-party/moonlight-common-c/nanors"
    if not (source / "src/fec_encoder_cache.h").is_file() or not (nanors / "rs.c").is_file():
        parser.error("Prepare the pinned Sunshine source, apply host/patches, and initialize its nanors submodule first")
    output = repo / "code-review/engine-bench"
    output.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    deadline = start + 300
    prefix = "fec-tiled-" if args.tiled_fec else "fec-only-" if args.fec_only else ""
    manifest = {"fec_only":args.fec_only,"tiled_fec":args.tiled_fec,"boundary": "synthetic encode/conversion/FEC only; not capture-to-TV latency", "commands": []}
    environment = dict(os.environ, CUDA_CACHE_DISABLE="1", __GL_SHADER_DISK_CACHE="0")

    def execute(command, name, cwd):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Five-minute total benchmark deadline reached")
        command = [str(x) for x in command]
        began = time.monotonic()
        process = subprocess.Popen(command, cwd=cwd, env=environment, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, start_new_session=True)
        try:
            stdout, stderr = process.communicate(timeout=remaining)
        except BaseException:
            try:
                os.killpg(process.pid, signal.SIGTERM)
                process.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.communicate()
            raise
        manifest["commands"].append({"argv": command, "exit": process.returncode,
                                     "seconds": time.monotonic() - began})
        (output / (prefix + name + ".stdout")).write_text(stdout)
        (output / (prefix + name + ".stderr")).write_text(stderr)
        if process.returncode:
            raise RuntimeError(f"{name} failed: {stderr[-2000:]}")
        return stdout

    try:
        with tempfile.TemporaryDirectory(prefix="sunshine-offline-benches-", dir="/tmp") as directory:
            temp = Path(directory)
            tools = repo / "host/tools"
            for item in ["nvenc_latency.cpp", "fec_latency.cpp", "gpu_conversion.cu"]:
                manifest[item + "_sha256"] = hashlib.sha256((tools / item).read_bytes()).hexdigest()
            if (source/"src/fec_gfni.h").exists():manifest["production_fec_sha256"]=hashlib.sha256((source/"src/fec_gfni.h").read_bytes()).hexdigest()
            if not args.fec_only:
                execute(["nvidia-smi", "--query-gpu=name,driver_version,pstate,clocks.gr,clocks.video", "--format=csv,noheader"], "gpu-metadata", temp)
                execute(["clang++", "--version"], "compiler-metadata", temp)
                execute(["clang++", "-O3", "-std=c++17", "-I/opt/cuda/include", tools / "nvenc_latency.cpp",
                         "-lavcodec", "-lavutil", "-lcuda", "-o", temp / "nvenc"], "nvenc-build", temp)
                execute([temp / "nvenc"], "nvenc-replay", temp)
                execute(["/opt/cuda/bin/nvcc", "-O3", "-std=c++17", "-ccbin", "/usr/bin/g++-14", "-arch=sm_89",
                         tools / "gpu_conversion.cu", "-o", temp / "conversion"], "conversion-build", temp)
                execute([temp / "conversion"], "conversion-replay", temp)
            includes = ["-I" + str(nanors), "-I" + str(nanors / "deps/obl")]
            objects = []
            for name in ["rs.c", "deps/obl/oblas_common.c", "deps/obl/oblas_lite.c"]:
                obj = temp / (Path(name).stem + ".o")
                execute(["clang", "-O3", "-std=c11", *includes, "-c", nanors / name, "-o", obj], "fec-build-" + obj.stem, temp)
                objects.append(obj)
            execute(["clang++", "-O3", "-std=c++23", "-I" + str(source), *includes, tools / "fec_latency.cpp",
                     *objects, "-o", temp / "fec"], "fec-build", temp)
            execute([temp / "fec", *(["--tiled"]if args.tiled_fec else [])], "fec-replay", temp)
        manifest["result"] = "pass"
    except Exception as error:
        manifest["result"] = "failed"
        manifest["error"] = str(error)
        raise
    finally:
        manifest["seconds"] = time.monotonic() - start
        (output / (prefix + "replay-manifest.json")).write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Synthetic probes passed in {manifest['seconds']:.2f}s; results: {output}")


if __name__ == "__main__":
    main()
