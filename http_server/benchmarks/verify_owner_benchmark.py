#!/usr/bin/env python3
import json
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit("usage: verify_owner_benchmark.py <log>")

rows = []
for line in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines():
    line = line.strip()
    if not line.startswith("{"):
        continue
    value = json.loads(line)
    if value.get("kind") == "measurement" and value.get("benchmark") == "chttp_server_owner_scaling":
        rows.append(value)

by_owner = {int(row["owners"]): row for row in rows}
if set(by_owner) != {1, 2, 4}:
    raise SystemExit(f"expected owner rows 1/2/4, got {sorted(by_owner)}")

base = by_owner[1]
for owners, row in sorted(by_owner.items()):
    if int(row.get("errors", -1)) != 0:
        raise SystemExit(f"owner={owners} reported errors={row.get('errors')}")
    if row["connections"] != base["connections"]:
        raise SystemExit("connection count changed across owner rows")
    if row["requests_per_connection"] != base["requests_per_connection"]:
        raise SystemExit("request count changed across owner rows")
    if row["operations"] != base["operations"]:
        raise SystemExit("total work changed across owner rows")
    for field in ("ops_per_second", "cpu_ns_per_op", "p50_ns", "p95_ns", "p99_ns"):
        if float(row[field]) <= 0:
            raise SystemExit(f"owner={owners} invalid {field}={row[field]}")

print("| owners | ops/s | speedup vs 1 | p50 ns | p95 ns | p99 ns | CPU ns/op | CPU ratio |")
print("| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
for owners in (1, 2, 4):
    row = by_owner[owners]
    speedup = float(row["ops_per_second"]) / float(base["ops_per_second"])
    cpu_ratio = float(row["cpu_ns_per_op"]) / float(base["cpu_ns_per_op"])
    print(
        f"| {owners} | {float(row['ops_per_second']):.1f} | {speedup:.3f}x | "
        f"{int(row['p50_ns'])} | {int(row['p95_ns'])} | {int(row['p99_ns'])} | "
        f"{float(row['cpu_ns_per_op']):.1f} | {cpu_ratio:.3f}x |"
    )

print()
print("Evidence-only gate: equal-work accounting and zero errors are enforced;")
print("performance ACCEPT/REJECT remains an explicit issue-level decision.")
