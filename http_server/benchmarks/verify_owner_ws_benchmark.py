#!/usr/bin/env python3
import json
import statistics
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit("usage: verify_owner_ws_benchmark.py <jsonl>")

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
    "ws-echo-64": ("tcp", "callback-echo", 64),
    "ws-push-64": ("tcp", "captured-push", 64),
    "ws-echo-64k": ("tcp", "callback-echo", 64 * 1024),
    "ws-push-64k": ("tcp", "captured-push", 64 * 1024),
    "wss-echo-64": ("tls", "callback-echo", 64),
    "wss-push-64": ("tls", "captured-push", 64),
    "wss-echo-64k": ("tls", "callback-echo", 64 * 1024),
    "wss-push-64k": ("tls", "captured-push", 64 * 1024),
}

groups = {}
for row in rows:
    if row.get("benchmark") != "chttp_server_owner_ws_scaling":
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
    transport, mode, payload = expected[workload]
    by_owner = groups[workload]
    if set(by_owner) != {1, 2, 4}:
        raise SystemExit(
            f"{workload}: expected owners 1/2/4, got {sorted(by_owner)}"
        )
    operations = None
    for owners in (1, 2, 4):
        points = by_owner[owners]
        if len(points) != repeat_count:
            raise SystemExit(
                f"{workload}/{owners}: expected {repeat_count} repeats, got {len(points)}"
            )
        for row in points:
            if row.get("transport") != transport or row.get("mode") != mode:
                raise SystemExit(f"{workload}/{owners}: transport/mode mismatch")
            if int(row["payload_bytes"]) != payload:
                raise SystemExit(f"{workload}/{owners}: payload mismatch")
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
            if operations is None:
                operations = int(row["operations"])
            elif int(row["operations"]) != operations:
                raise SystemExit(f"{workload}: operation count changed")
            for field in (
                "messages_per_second",
                "cpu_ns_per_message",
                "p50_ns",
                "p95_ns",
                "p99_ns",
            ):
                if float(row[field]) <= 0:
                    raise SystemExit(
                        f"{workload}/{owners}: invalid {field}={row[field]}"
                    )

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
                    f"{workload}/{owners}: admission handoffs "
                    f"{row['cross_owner_admission_handoffs']} != {expected_handoffs}"
                )

            captured = int(row["captured_command_admissions"])
            if mode == "callback-echo" and captured != 0:
                raise SystemExit(
                    f"{workload}/{owners}: callback echo used captured command path"
                )
            if mode == "captured-push" and captured != int(row["operations"]):
                raise SystemExit(
                    f"{workload}/{owners}: captured admissions {captured} "
                    f"!= operations {row['operations']}"
                )

    base = by_owner[1]
    base_rate = median_value(base, "messages_per_second")
    base_cpu = median_value(base, "cpu_ns_per_message")
    print(f"### {workload}")
    print()
    print(
        "| owners | repeats | msg/s | speedup | p50 ns | p95 ns | p99 ns | "
        "CPU ns/msg | CPU ratio | handoffs | leases | peak cmd queues |"
    )
    print(
        "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |"
    )
    for owners in (1, 2, 4):
        points = by_owner[owners]
        rate = median_value(points, "messages_per_second")
        cpu = median_value(points, "cpu_ns_per_message")
        p50 = int(median_value(points, "p50_ns"))
        p95 = int(median_value(points, "p95_ns"))
        p99 = int(median_value(points, "p99_ns"))
        sample = points[0]
        leases = "/".join(
            str(int(sample[f"owner{index}_leases"])) for index in range(owners)
        )
        peaks = "/".join(
            str(max(int(point[f"peak_owner{index}_command_queue"]) for point in points))
            for index in range(owners)
        )
        print(
            f"| {owners} | {len(points)} | {rate:.1f} | {rate/base_rate:.3f}x | "
            f"{p50} | {p95} | {p99} | {cpu:.1f} | {cpu/base_cpu:.3f}x | "
            f"{int(sample['cross_owner_admission_handoffs'])} | {leases} | {peaks} |"
        )
    print()

print(
    "Correctness gate: fixed equal work, verified WS/WSS setup, zero errors/rejections, "
    "balanced leases, exact connection-admission handoffs, and zero server-owner "
    "data-plane handoffs."
)
print(
    "callback-echo measures callback-local sends; captured-push measures the bounded "
    "captured-session command queue/wake path."
)
print(
    "Performance remains evidence-only for #175/#172 acceptance; no hosted-runner "
    "absolute threshold is enforced."
)
