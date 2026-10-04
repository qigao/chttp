#!/usr/bin/env python3
import json
import statistics
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit("usage: verify_owner_benchmark.py <jsonl>")

environments = []
rows = []
for raw in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines():
    line = raw.strip()
    if not line.startswith("{"):
        continue
    item = json.loads(line)
    if item.get("kind") == "environment":
        environments.append(item)
    elif item.get("kind") == "measurement":
        rows.append(item)

if not environments:
    raise SystemExit("missing environment row")

commit = environments[0].get("commit", "unknown")
backend = environments[0].get("backend", "unknown")
connections = int(environments[0].get("connections", 0))
for environment in environments:
    if environment.get("commit", "unknown") != commit:
        raise SystemExit("benchmark repeats do not use one exact head")
    if environment.get("backend", "unknown") != backend:
        raise SystemExit("benchmark repeats changed backend")
    if int(environment.get("connections", 0)) != connections:
        raise SystemExit("benchmark repeats changed connection count")

expected_workloads = {"1k-copy", "64k-retained"}
groups = {}
for row in rows:
    if row.get("benchmark") != "chttp_server_owner_scaling":
        continue
    key = (row["workload"], int(row["owners"]))
    groups.setdefault(key, []).append(row)

seen_workloads = {key[0] for key in groups}
if seen_workloads != expected_workloads:
    raise SystemExit(
        f"expected workloads {sorted(expected_workloads)}, got {sorted(seen_workloads)}"
    )

def validate_row(workload, owners, row):
    if int(row.get("errors", -1)) != 0:
        raise SystemExit(f"{workload}/{owners}: errors={row.get('errors')}")
    if int(row.get("rejected_connections", -1)) != 0:
        raise SystemExit(f"{workload}/{owners}: rejected connections")
    if int(row["connections"]) != connections:
        raise SystemExit(f"{workload}/{owners}: connection count changed")
    if int(row["samples"]) != int(row["operations"]):
        raise SystemExit(f"{workload}/{owners}: missing latency samples")
    if int(row["accepted_connections"]) != connections:
        raise SystemExit(
            f"{workload}/{owners}: accepted connection count mismatch"
        )
    for field in (
        "ops_per_second",
        "cpu_ns_per_op",
        "p50_ns",
        "p95_ns",
        "p99_ns",
    ):
        if float(row[field]) <= 0:
            raise SystemExit(
                f"{workload}/{owners}: invalid {field}={row[field]}"
            )

    leases = [int(row.get(f"owner{index}_leases", 0))
              for index in range(owners)]
    if sum(leases) != connections:
        raise SystemExit(
            f"{workload}/{owners}: lease sum {sum(leases)} != {connections}"
        )
    if max(leases) - min(leases) > 1:
        raise SystemExit(
            f"{workload}/{owners}: unbalanced fixed-owner leases {leases}"
        )

    expected_handoffs = connections - leases[0]
    if int(row["cross_owner_admission_handoffs"]) != expected_handoffs:
        raise SystemExit(
            f"{workload}/{owners}: handoff count "
            f"{row['cross_owner_admission_handoffs']} != "
            f"non-owner0 live leases {expected_handoffs}"
        )

for workload in expected_workloads:
    owner_set = {owner for (name, owner) in groups if name == workload}
    if owner_set != {1, 2, 4}:
        raise SystemExit(
            f"{workload}: expected owners 1/2/4, got {sorted(owner_set)}"
        )
    operations = None
    for owners in (1, 2, 4):
        samples = groups[(workload, owners)]
        if len(samples) != len(environments):
            raise SystemExit(
                f"{workload}/{owners}: expected {len(environments)} repeats, "
                f"got {len(samples)}"
            )
        for row in samples:
            validate_row(workload, owners, row)
            row_operations = int(row["operations"])
            if operations is None:
                operations = row_operations
            elif row_operations != operations:
                raise SystemExit(
                    f"{workload}: equal-work operation count changed"
                )

def median(workload, owners, field):
    return statistics.median(
        float(row[field]) for row in groups[(workload, owners)]
    )

print(f"exact head: {commit}")
print(f"backend: {backend}")
print(f"persistent connections: {connections}")
print(f"repeats per point: {len(environments)}")
print()
for workload in ("1k-copy", "64k-retained"):
    baseline_throughput = median(workload, 1, "ops_per_second")
    baseline_cpu = median(workload, 1, "cpu_ns_per_op")
    print(f"### {workload}")
    print()
    print(
        "| owners | runs | ops/s | speedup | p50 ns | p95 ns | p99 ns | "
        "CPU ns/op | CPU ratio | accepted | rejected | admission handoffs | "
        "owner leases |"
    )
    print(
        "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | "
        "---: | ---: | ---: | --- |"
    )
    for owners in (1, 2, 4):
        samples = groups[(workload, owners)]
        throughput = median(workload, owners, "ops_per_second")
        cpu = median(workload, owners, "cpu_ns_per_op")
        representative = samples[-1]
        lease_values = [
            int(representative.get(f"owner{index}_leases", 0))
            for index in range(owners)
        ]
        print(
            f"| {owners} | {len(samples)} | {throughput:.1f} | "
            f"{throughput / baseline_throughput:.3f}x | "
            f"{median(workload, owners, 'p50_ns'):.0f} | "
            f"{median(workload, owners, 'p95_ns'):.0f} | "
            f"{median(workload, owners, 'p99_ns'):.0f} | "
            f"{cpu:.1f} | {cpu / baseline_cpu:.3f}x | "
            f"{int(representative['accepted_connections'])} | "
            f"{int(representative['rejected_connections'])} | "
            f"{int(representative['cross_owner_admission_handoffs'])} | "
            f"{'/'.join(str(value) for value in lease_values)} |"
        )
    print()

print(
    "Correctness gate: equal work, balanced fixed-owner leases, zero request "
    "errors/rejections and exact admission-handoff attribution."
)
print(
    "Performance values are same-run evidence only; hosted-runner absolute "
    "numbers are not cross-machine rankings. Full #172 acceptance still "
    "requires H2/WS/WSS/TLS profiles."
)
