#!/usr/bin/env python3
"""Fail-closed TSan compilation proof for exact Salts ServiceHost pilot.

The published Salts::Plugin/CMeta/CNet/Platform SDK libraries remain prebuilt
and are explicitly NOT covered by this source-instrumentation proof.
"""
import json
from pathlib import Path
import sys

REQUIRED = (
    "app/src/chttp_service.c",
    "app/tests/service/chttp_service_host_single_listener_test.c",
    "app/tests/service/chttp_service_host_methodplan_switch_test.c",
    "app/tests/service/chttp_service_host_provider.c",
    "app/tests/service/chttp_service_host_second_business.c",
    "component-runtime/src/configurator.c",
    "component-runtime/src/service_host.c",
    "component-runtime/src/service_host_runtime.c",
)

def require(ok, message):
    if not ok:
        raise SystemExit("ServiceHost TSan compile proof failure: " + message)

def main():
    require(len(sys.argv) == 2, "expected compilation database path")
    path = Path(sys.argv[1])
    require(path.is_file(), "compilation database missing: " + str(path))
    commands = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(commands, list) and bool(commands),
            "compilation database empty or malformed")
    for suffix in REQUIRED:
        rows = []
        for entry in commands:
            src = str(entry.get("file", "")).replace("\\", "/")
            if src == suffix or src.endswith("/" + suffix):
                raw = entry.get("command")
                if raw is None:
                    raw = " ".join(entry.get("arguments", []))
                rows.append(str(raw))
        require(bool(rows), "missing original C11 source: " + suffix)
        for row in rows:
            require("-fsanitize=thread" in row,
                    "source was not TSan-instrumented: " + suffix)
        print(f"TSan instrumented: {suffix} ({len(rows)} compile row(s))")
    print("PASS: original candidate, CHttp and real Provider DSO source instrumentation")
    print("Not claimed: prebuilt Salts Plugin/CMeta/Platform/CNet SDK object instrumentation")

if __name__ == "__main__":
    main()
