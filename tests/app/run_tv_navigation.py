"""Isolate native UI regression files and remove them on every exit path."""
import os
import resource
import subprocess
import sys
import tempfile
from pathlib import Path

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

with tempfile.TemporaryDirectory(prefix="moonlight-ui-", dir="/tmp") as scratch:
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               MOONLIGHT_UI_SCRATCH=scratch, XDG_CACHE_DIR=str(Path(scratch) / "cache"),
               XDG_DATA_HOME=str(Path(scratch) / "data"))
    Path(env["XDG_CACHE_DIR"]).mkdir()
    result = subprocess.run(sys.argv[1:], env=env, timeout=25)
    sys.exit(result.returncode)
