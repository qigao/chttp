#!/usr/bin/env python3
import json
import statistics
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit("usage: verify_owner_protocol_benchmark.py <jsonl>")

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
    raise SystemExit("missing environment rows")

heads = {item.get("commit") for item in environments}
backends = {item.get("backend") for item in environments}
connections = {int(item.get("connections", 0)) for item in environments}
if len(heads) != 1 or len(backends) != 1 or connections != {8}:
    raise SystemExit(
        f"environment mismatch heads={heads} backends={backends} connections={connections}"
    )
repeat_count = len(environments)

expected = {
    "h2-tcp-1k-copy": ("h2", "tcp", 4),
    "h2-tcp-1k-copy-server-nodelay": ("h2", "tcp", 4),
    "h2-tcp-1k-copy-client-nodelay": ("h2", "tcp", 4),
    "h2-tcp-1k-copy-both-nodelay": ("h2", "tcp", 4),
    "h2-tcp-64k-retained": ("h2", "tcp", 4),
    "h1-tls-1k-copy": ("h1", "tls", 1),
    "h1-tls-64k-retained": ("h1", "tls", 1),
    "h2-tls-1k-copy": ("h2", "tls", 4),
    "h2-tls-16383-copy-single": ("h2", "tls", 1),
    "h2-tls-64k-copy-single": ("h2", "tls", 1),
    "h2-tls-16383-copy": ("h2", "tls", 4),
    "h2-tls-16384-copy": ("h2", "tls", 4),
    "h2-tls-64k-copy": ("h2", "tls", 4),
    "h2-tls-64k-retained": ("h2", "tls", 4),
}

groups = {}
for row in rows:
    if row.get("benchmark") != "chttp_server_owner_protocol_scaling":
        continue
    workload = row.get("workload")
    groups.setdefault(workload, {}).setdefault(int(row["owners"]), []).append(row)

if set(groups) != set(expected):
    raise SystemExit(
        f"workload mismatch expected={sorted(expected)} got={sorted(groups)}"
    )

def median_value(items, field):
    return statistics.median(float(item[field]) for item in items)

print(f"exact head: {next(iter(heads))}")
print(f"backend: {next(iter(backends))}; repeats: {repeat_count}")
print()

for workload in expected:
    protocol, transport, streams = expected[workload]
    by_owner = groups[workload]
    if set(by_owner) != {1, 2, 4}:
        raise SystemExit(f"{workload}: expected owners 1/2/4, got {sorted(by_owner)}")
    for owners in (1, 2, 4):
        points = by_owner[owners]
        if len(points) != repeat_count:
            raise SystemExit(
                f"{workload}/{owners}: expected {repeat_count} repeats, got {len(points)}"
            )
        operation_counts = {int(row["operations"]) for row in points}
        if len(operation_counts) != 1:
            raise SystemExit(f"{workload}/{owners}: operation count varied across repeats")
        for row in points:
            if row.get("protocol") != protocol or row.get("transport") != transport:
                raise SystemExit(f"{workload}/{owners}: protocol/transport mismatch")
            if int(row["streams_per_connection"]) != streams:
                raise SystemExit(
                    f"{workload}/{owners}: streams_per_connection="
                    f"{row['streams_per_connection']} expected={streams}"
                )
            if int(row["connections"]) != 8:
                raise SystemExit(f"{workload}/{owners}: connections != 8")
            if int(row["accepted_connections"]) != 8:
                raise SystemExit(f"{workload}/{owners}: accepted != 8")
            if int(row["rejected_connections"]) != 0:
                raise SystemExit(f"{workload}/{owners}: rejected connections")
            if int(row.get("errors", -1)) != 0:
                raise SystemExit(f"{workload}/{owners}: errors={row.get('errors')}")
            if int(row.get("cross_owner_data_plane_hops", -1)) != 0:
                raise SystemExit(f"{workload}/{owners}: cross-owner data-plane hop")
            if int(row["samples"]) != int(row["operations"]):
                raise SystemExit(f"{workload}/{owners}: sample/operation mismatch")
            leases = [
                int(row[f"owner{index}_leases"]) for index in range(owners)
            ]
            if sum(leases) != 8 or max(leases) - min(leases) > 1:
                raise SystemExit(
                    f"{workload}/{owners}: unbalanced leases {leases}"
                )
            expected_handoffs = 8 - leases[0]
            if int(row["cross_owner_admission_handoffs"]) != expected_handoffs:
                raise SystemExit(
                    f"{workload}/{owners}: handoffs="
                    f"{row['cross_owner_admission_handoffs']} expected={expected_handoffs}"
                )
            for field in ("ops_per_second", "cpu_ns_per_op", "p50_ns", "p95_ns", "p99_ns"):
                if float(row[field]) <= 0:
                    raise SystemExit(
                        f"{workload}/{owners}: invalid {field}={row[field]}"
                    )

    base = by_owner[1]
    base_ops = median_value(base, "ops_per_second")
    base_cpu = median_value(base, "cpu_ns_per_op")
    print(f"### {workload}")
    print()
    print(
        "| owners | repeats | ops/s | speedup | p50 ns | p95 ns | p99 ns | "
        "CPU ns/op | CPU ratio | handoffs | leases |"
    )
    print(
        "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |"
    )
    for owners in (1, 2, 4):
        points = by_owner[owners]
        ops = median_value(points, "ops_per_second")
        cpu = median_value(points, "cpu_ns_per_op")
        p50 = int(median_value(points, "p50_ns"))
        p95 = int(median_value(points, "p95_ns"))
        p99 = int(median_value(points, "p99_ns"))
        sample = points[0]
        leases = "/".join(
            str(int(sample[f"owner{index}_leases"])) for index in range(owners)
        )
        print(
            f"| {owners} | {len(points)} | {ops:.1f} | {ops/base_ops:.3f}x | "
            f"{p50} | {p95} | {p99} | {cpu:.1f} | {cpu/base_cpu:.3f}x | "
            f"{int(sample['cross_owner_admission_handoffs'])} | {leases} |"
        )
    print()

print(
    "Correctness gate: fixed equal work per workload, true H2 multiplex depth=4, "
    "TLS verification enabled, zero errors/rejections, balanced leases, exact "
    "admission handoffs, zero cross-owner data-plane hops."
)
print(
    "Performance remains evidence-only for #174/#172 acceptance; no hosted-runner "
    "absolute threshold is enforced."
)
