#!/usr/bin/env python3
"""Replay the production DMA-BUF cache header/regressions and real GPU import probe."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import tempfile
import time


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--contention", action="store_true", help="Also compare under bounded synthetic GPU load")
    args = parser.parse_args()
    output = root / "code-review/engine-bench"
    output.mkdir(parents=True, exist_ok=True)
    patch = root / "host/patches/0006-cache-linux-dmabuf-source-imports.patch"
    began = time.monotonic()
    deadline = began + 300
    env = dict(os.environ, CUDA_CACHE_DISABLE="1", __GL_SHADER_DISK_CACHE="0")
    manifest = {"patch_sha256": hashlib.sha256(patch.read_bytes()).hexdigest(), "commands": [],
                "boundary": "offscreen GBM imports and readback only; no Sunshine stream or TV"}

    def stop(process):
        if process is None:
            return
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate()

    def run(command, name, cwd):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Five-minute total deadline reached")
        command = [str(x) for x in command]
        process = subprocess.Popen(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, start_new_session=True)
        try:
            stdout, stderr = process.communicate(timeout=remaining)
        except BaseException:
            stop(process)
            raise
        (output / (name + ".stdout")).write_text(stdout)
        (output / (name + ".stderr")).write_text(stderr)
        manifest["commands"].append({"argv": command, "exit": process.returncode})
        if process.returncode:
            raise RuntimeError(f"{name} failed: {stderr[-1500:]}")
        return stdout

    try:
        with tempfile.TemporaryDirectory(prefix="dmabuf-import-hunt-", dir="/tmp") as directory:
            temp = Path(directory)
            run(["git", "apply", "--include=src/platform/linux/dmabuf_import_cache.h",
                 "--include=tests/unit/test_dmabuf_import_cache.cpp", patch], "import-replay-prepare", temp)
            test = temp / "tests/unit/test_dmabuf_import_cache.cpp"
            run(["clang++", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-std=c++17", "-DDMABUF_CACHE_STANDALONE", "-I" + str(temp), test,
                 "-o", temp / "cache-tests"], "import-replay-test-build", temp)
            run([temp / "cache-tests"], "import-replay-tests", temp)
            run(["clang++", "-O3", "-std=c++17", "-I" + str(temp), root / "host/tools/dmabuf_import_probe.cpp",
                 "-lEGL", "-lGL", "-lgbm", "-o", temp / "probe"], "import-replay-build", temp)
            run([temp / "probe"], "import-replay-idle", temp)
            if args.contention:
                run(["/opt/cuda/bin/nvcc", "-O3", "-std=c++17", "-ccbin", "/usr/bin/g++-14", "-arch=sm_89",
                     root / "host/tools/gpu_conversion.cu", "-o", temp / "load"], "import-replay-load-build", temp)
                load = subprocess.Popen([str(temp / "load"), "--contention-seconds", "20"], cwd=temp,
                                        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=True, start_new_session=True)
                try:
                    if not select.select([load.stdout], [], [], 5)[0] or "ready" not in load.stdout.readline():
                        raise RuntimeError("Synthetic load did not signal readiness")
                    run([temp / "probe", "--short"], "import-replay-busy", temp)
                    manifest["load_alive_at_end"] = load.poll() is None
                    if not manifest["load_alive_at_end"]:
                        raise RuntimeError("Interference ended before measurement")
                finally:
                    stop(load)
        manifest["result"] = "pass"
    except BaseException as error:
        manifest["result"] = "failed"
        manifest["error"] = str(error)
        raise
    finally:
        manifest["seconds"] = time.monotonic() - began
        (output / "import-replay-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Import replay passed in {manifest['seconds']:.2f}s; evidence: {output}")


if __name__ == "__main__":
    main()
