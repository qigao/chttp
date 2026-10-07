#!/usr/bin/env python3
import json
import statistics
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit("usage: verify_owner_churn_benchmark.py <jsonl>")

envs = []
rows = []
for raw in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines():
    raw = raw.strip()
    if not raw.startswith("{"):
        continue
    item = json.loads(raw)
    if item.get("kind") == "environment":
        envs.append(item)
    elif item.get("kind") == "measurement":
        rows.append(item)

if not envs:
    raise SystemExit("missing environment row")

commit = envs[0].get("commit", "unknown")
for env in envs:
    if env.get("commit", "unknown") != commit:
        raise SystemExit("benchmark repeats changed exact head")

groups = {}
for row in rows:
    if row.get("benchmark") != "chttp_server_owner_admission_churn":
        continue
    groups.setdefault(int(row["owners"]), []).append(row)

if set(groups) != {1, 2, 4}:
    raise SystemExit(f"expected owners 1/2/4, got {sorted(groups)}")

for owners, samples in groups.items():
    if len(samples) != len(envs):
        raise SystemExit(
            f"owners={owners}: expected {len(envs)} repeats, got {len(samples)}"
        )
    for row in samples:
        if int(row.get("errors", -1)) != 0:
            raise SystemExit(f"owners={owners}: errors={row.get('errors')}")
        if int(row.get("rejected_connections", -1)) != 0:
            raise SystemExit(f"owners={owners}: rejected connections")
        initial = int(row["initial_connections"])
        churn = int(row["churn_connections"])
        if int(row["accepted_connections"]) != initial + churn:
            raise SystemExit(
                f"owners={owners}: accepted count mismatch "
                f"{row['accepted_connections']} != {initial + churn}"
            )

        skew_owner = int(row["skew_owner"])
        kept = int(row["kept_connections"])
        skew = [int(row.get(f"skew_owner{i}_leases", 0)) for i in range(owners)]
        if sum(skew) != kept:
            raise SystemExit(f"owners={owners}: skew lease sum mismatch {skew}")
        if skew[skew_owner] != kept:
            raise SystemExit(
                f"owners={owners}: intended skew owner {skew_owner} does not own all kept leases {skew}"
            )

        admissions = [
            int(row.get(f"churn_owner{i}_admissions", 0)) for i in range(owners)
        ]
        if sum(admissions) != churn:
            raise SystemExit(
                f"owners={owners}: churn admission sum {sum(admissions)} != {churn}"
            )
        if max(admissions) - min(admissions) > 1:
            raise SystemExit(
                f"owners={owners}: rotating admission became unbalanced {admissions}"
            )

        for field in ("p50_ns", "p95_ns", "p99_ns"):
            if int(row[field]) <= 0:
                raise SystemExit(f"owners={owners}: invalid {field}={row[field]}")

def median(owners, field):
    return statistics.median(float(row[field]) for row in groups[owners])

print(f"exact head: {commit}")
print(f"repeats per point: {len(envs)}")
print()
print("| owners | kept skew leases | churn admissions | p50 ns | p95 ns | p99 ns |")
print("| ---: | --- | --- | ---: | ---: | ---: |")
for owners in (1, 2, 4):
    row = groups[owners][-1]
    skew = [int(row.get(f"skew_owner{i}_leases", 0)) for i in range(owners)]
    admissions = [int(row.get(f"churn_owner{i}_admissions", 0)) for i in range(owners)]
    print(
        f"| {owners} | {'/'.join(map(str, skew))} | "
        f"{'/'.join(map(str, admissions))} | "
        f"{median(owners, 'p50_ns'):.0f} | "
        f"{median(owners, 'p95_ns'):.0f} | "
        f"{median(owners, 'p99_ns'):.0f} |"
    )

print()
print(
    "Correctness gate: a deliberately skewed long-lived owner set is retained while "
    "new short-lived connections continue to rotate evenly across available owners. "
    "This is connection-admission evidence only; it does not claim CPU/load-aware scheduling."
)
