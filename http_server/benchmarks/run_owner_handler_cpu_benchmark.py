"""Formal CTest driver: three measurements, retained raw data, checked admission."""
import datetime
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

binary, output_dir, source, compiler = sys.argv[1:]
commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=source,
                        check=True, capture_output=True, text=True).stdout.strip()
dirty = bool(subprocess.run(["git", "status", "--porcelain"], cwd=source,
                            check=True, capture_output=True, text=True).stdout.strip())
commit += "-dirty" if dirty else ""
root = Path(output_dir)
root.mkdir(parents=True, exist_ok=True)
stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
raw_path = root / ("handler-cpu-" + stamp + ".jsonl")
print("raw measurements: " + str(raw_path), flush=True)
env = dict(os.environ, GITHUB_SHA=commit)
with raw_path.open("x", encoding="utf-8") as raw:
    raw.write(json.dumps({"kind": "host", "commit": commit, "compiler": compiler,
                          "platform": platform.platform(), "processor": platform.processor()}) + "\n")
    for repeat in range(3):
        result = subprocess.run([binary], env=env, capture_output=True, text=True, timeout=180)
        with raw_path.with_suffix(".stderr").open("a", encoding="utf-8") as diagnostics:
            diagnostics.write(f"REPEAT {repeat+1}\n" + result.stderr)
        raw.write(result.stdout)
        raw.flush()
        if result.returncode:
            raise SystemExit(f"repeat {repeat+1} failed ({result.returncode}): {result.stderr}")
subprocess.run([sys.executable, str(Path(__file__).with_name("verify_owner_handler_cpu_benchmark.py")),
                str(raw_path)], check=True)
