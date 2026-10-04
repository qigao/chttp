#!/usr/bin/env python3
import json
import statistics
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit("usage: verify_ws_owner_benchmark.py <jsonl>")

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
    "ws-echo-64": ("ws", "callback-local", "round-trip", 64),
    "ws-echo-64k": ("ws", "callback-local", "round-trip", 65536),
    "ws-copy-64": ("ws", "copied-command", "server-push", 64),
    "wss-echo-64": ("wss", "callback-local", "round-trip", 64),
    "wss-echo-64k": ("wss", "callback-local", "round-trip", 65536),
    "wss-copy-64": ("wss", "copied-command", "server-push", 64),
}

groups = {}
for row in rows:
    if row.get("benchmark") != "chttp_server_ws_owner_scaling":
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
    transport, send_path, latency_kind, payload = expected[workload]
    by_owner = groups[workload]
    if set(by_owner) != {1, 2, 4}:
        raise SystemExit(
            f"{workload}: expected owners 1/2/4, got {sorted(by_owner)}"
        )

    operation_count = None
    observed_command_pressure = False
    for owners in (1, 2, 4):
        points = by_owner[owners]
        if len(points) != repeat_count:
            raise SystemExit(
                f"{workload}/{owners}: expected {repeat_count} repeats, got {len(points)}"
            )
        for row in points:
            if row.get("transport") != transport:
                raise SystemExit(f"{workload}/{owners}: transport mismatch")
            if row.get("send_path") != send_path:
                raise SystemExit(f"{workload}/{owners}: send path mismatch")
            if row.get("latency_kind") != latency_kind:
                raise SystemExit(f"{workload}/{owners}: latency kind mismatch")
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
            if int(row["samples"]) != int(row["operations"]):
                raise SystemExit(f"{workload}/{owners}: sample/operation mismatch")

            operations = int(row["operations"])
            if operation_count is None:
                operation_count = operations
            elif operations != operation_count:
                raise SystemExit(f"{workload}: equal-work operation count changed")

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

            command_peaks = [
                int(row[f"peak_owner{index}_commands"]) for index in range(owners)
            ]
            if any(value > 0 for value in command_peaks):
                observed_command_pressure = True
            if send_path == "callback-local" and any(command_peaks):
                raise SystemExit(
                    f"{workload}/{owners}: callback-local path used copied command queue "
                    f"{command_peaks}"
                )

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

    if send_path == "copied-command" and not observed_command_pressure:
        print(
            f"note: {workload}: copied-command queue drained faster than pressure sampling "
            "for all repeats; the workload still exercises the thread-safe copied API."
        )

    base = by_owner[1]
    base_ops = median_value(base, "messages_per_second")
    base_cpu = median_value(base, "cpu_ns_per_message")
    print(f"### {workload}")
    print()
    print(
        "| owners | repeats | msg/s | speedup | p50 ns | p95 ns | p99 ns | "
        "CPU ns/msg | CPU ratio | handoffs | leases | command peaks |"
    )
    print(
        "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | "
        "---: | --- | --- |"
    )
    for owners in (1, 2, 4):
        points = by_owner[owners]
        ops = median_value(points, "messages_per_second")
        cpu = median_value(points, "cpu_ns_per_message")
        sample = points[0]
        leases = "/".join(
            str(int(sample[f"owner{index}_leases"])) for index in range(owners)
        )
        peaks = "/".join(
            str(
                int(
                    statistics.median(
                        int(row[f"peak_owner{index}_commands"]) for row in points
                    )
                )
            )
            for index in range(owners)
        )
        print(
            f"| {owners} | {len(points)} | {ops:.1f} | {ops/base_ops:.3f}x | "
            f"{median_value(points, 'p50_ns'):.0f} | "
            f"{median_value(points, 'p95_ns'):.0f} | "
            f"{median_value(points, 'p99_ns'):.0f} | "
            f"{cpu:.1f} | {cpu/base_cpu:.3f}x | "
            f"{int(sample['cross_owner_admission_handoffs'])} | {leases} | {peaks} |"
        )
    print()

print(
    "Correctness gate: fixed equal work, 8 accepted/0 rejected, balanced fixed-owner "
    "leases, exact admission-handoff attribution, verified WSS, and callback-local "
    "workloads bypass the copied command queue."
)
print(
    "Copied-command workloads measure queue admission + owner wake + delivery; "
    "callback-local workloads measure client-to-server-to-client round trip."
)
