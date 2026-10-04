#!/usr/bin/env python3
import json
import statistics
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit("usage: verify_h2_flow_window_benchmark.py <jsonl>")

envs = []
rows = []
for raw in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines():
    line = raw.strip()
    if not line.startswith("{"):
        continue
    item = json.loads(line)
    if item.get("kind") == "environment" and item.get("benchmark") == "chttp_h2_flow_window":
        envs.append(item)
    elif item.get("kind") == "measurement" and item.get("benchmark") == "chttp_h2_flow_window":
        rows.append(item)

if not envs:
    raise SystemExit("missing flow-window environment rows")

heads = {e.get("commit") for e in envs}
backends = {e.get("backend") for e in envs}
connections = {int(e.get("connections", 0)) for e in envs}
flow_modes = {bool(e.get("flow_mode")) for e in envs}
flow_rounds = {int(e.get("flow_rounds", 0)) for e in envs}
if len(heads) != 1 or len(backends) != 1 or connections != {8}:
    raise SystemExit(
        f"environment mismatch heads={heads} backends={backends} connections={connections}"
    )
if flow_modes != {True} or len(flow_rounds) != 1 or next(iter(flow_rounds)) <= 0:
    raise SystemExit(
        f"invalid flow mode/rounds: modes={flow_modes} rounds={flow_rounds}"
    )
repeat_count = len(envs)

copy_sizes = (16383, 16384, 32768, 65535, 65536, 131072)
retained_sizes = (65535, 65536, 131072)
transports = ("tcp", "tls")
windows = ("default", "wide")
owners_set = {1, 2, 4}

expected = {}
for transport in transports:
    for size in copy_sizes:
        for window in windows:
            name = f"{transport}-copy-{size}-{window}"
            expected[name] = {
                "transport": transport,
                "payload": size,
                "retained": False,
                "window": 65535 if window == "default" else 131072,
                "client_nodelay": True,
            }
    for size in retained_sizes:
        for window in windows:
            name = f"{transport}-retained-{size}-{window}"
            expected[name] = {
                "transport": transport,
                "payload": size,
                "retained": True,
                "window": 65535 if window == "default" else 131072,
                "client_nodelay": True,
            }
    for size in (65535, 65536):
        for window in windows:
            name = f"{transport}-copy-{size}-{window}-nagle"
            expected[name] = {
                "transport": transport,
                "payload": size,
                "retained": False,
                "window": 65535 if window == "default" else 131072,
                "client_nodelay": False,
            }

groups = {}
for row in rows:
    groups.setdefault(row.get("workload"), {}).setdefault(int(row["owners"]), []).append(row)

if set(groups) != set(expected):
    missing = sorted(set(expected) - set(groups))
    extra = sorted(set(groups) - set(expected))
    raise SystemExit(f"workload mismatch missing={missing} extra={extra}")

expected_ops = 8 * 4 * next(iter(flow_rounds))
for name, spec in expected.items():
    by_owner = groups[name]
    if set(by_owner) != owners_set:
        raise SystemExit(f"{name}: expected owners 1/2/4, got {sorted(by_owner)}")
    for owners in (1, 2, 4):
        points = by_owner[owners]
        if len(points) != repeat_count:
            raise SystemExit(
                f"{name}/{owners}: expected {repeat_count} repeats, got {len(points)}"
            )
        for row in points:
            if row.get("protocol") != "h2":
                raise SystemExit(f"{name}/{owners}: protocol != h2")
            if row.get("transport") != spec["transport"]:
                raise SystemExit(f"{name}/{owners}: transport mismatch")
            if bool(row.get("retained")) != spec["retained"]:
                raise SystemExit(f"{name}/{owners}: retained mismatch")
            if int(row["payload_bytes"]) != spec["payload"]:
                raise SystemExit(f"{name}/{owners}: payload mismatch")
            if int(row["streams_per_connection"]) != 4:
                raise SystemExit(f"{name}/{owners}: streams != 4")
            if int(row["connections"]) != 8:
                raise SystemExit(f"{name}/{owners}: connections != 8")
            if int(row["operations"]) != expected_ops or int(row["samples"]) != expected_ops:
                raise SystemExit(
                    f"{name}/{owners}: expected operations/samples={expected_ops}, "
                    f"got {row['operations']}/{row['samples']}"
                )
            if int(row["stream_receive_window"]) != spec["window"]:
                raise SystemExit(f"{name}/{owners}: stream window mismatch")
            if int(row["connection_receive_window"]) != spec["window"]:
                raise SystemExit(f"{name}/{owners}: connection window mismatch")
            if bool(row["client_nodelay"]) != spec["client_nodelay"]:
                raise SystemExit(f"{name}/{owners}: client_nodelay mismatch")
            if bool(row["server_nodelay"]):
                raise SystemExit(f"{name}/{owners}: unexpected server_nodelay")
            if int(row["accepted_connections"]) != 8 or int(row["rejected_connections"]) != 0:
                raise SystemExit(f"{name}/{owners}: connection accounting mismatch")
            if int(row.get("errors", -1)) != 0:
                raise SystemExit(f"{name}/{owners}: errors={row.get('errors')}")
            if int(row.get("cross_owner_data_plane_hops", -1)) != 0:
                raise SystemExit(f"{name}/{owners}: owner data-plane handoff detected")
            leases = [int(row[f"owner{i}_leases"]) for i in range(owners)]
            if sum(leases) != 8 or max(leases) - min(leases) > 1:
                raise SystemExit(f"{name}/{owners}: unbalanced leases {leases}")
            expected_handoffs = 8 - leases[0]
            if int(row["cross_owner_admission_handoffs"]) != expected_handoffs:
                raise SystemExit(
                    f"{name}/{owners}: handoffs={row['cross_owner_admission_handoffs']} "
                    f"expected={expected_handoffs}"
                )
            for field in ("ops_per_second", "cpu_ns_per_op", "p50_ns", "p95_ns", "p99_ns"):
                if float(row[field]) <= 0:
                    raise SystemExit(f"{name}/{owners}: invalid {field}={row[field]}")

def med(name, owners, field):
    return statistics.median(float(p[field]) for p in groups[name][owners])

def ratio(a, b):
    return a / b if b else 0.0

print(f"exact head: {next(iter(heads))}")
print(f"backend: {next(iter(backends))}; repeats: {repeat_count}; operations/point: {expected_ops}")
print()

print("### 65,535 -> 65,536 boundary (client TCP_NODELAY on)")
print()
print("| transport | window | owners | 65535 ops/s | 65536 ops/s | ops ratio | 65535 p50 | 65536 p50 | p50 ratio |")
print("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
for transport in transports:
    for window in windows:
        for owners in (1, 4):
            a = f"{transport}-copy-65535-{window}"
            b = f"{transport}-copy-65536-{window}"
            a_ops = med(a, owners, "ops_per_second")
            b_ops = med(b, owners, "ops_per_second")
            a_p50 = med(a, owners, "p50_ns")
            b_p50 = med(b, owners, "p50_ns")
            print(
                f"| {transport} | {window} | {owners} | {a_ops:.1f} | {b_ops:.1f} | "
                f"{ratio(b_ops,a_ops):.3f}x | {int(a_p50)} | {int(b_p50)} | "
                f"{ratio(b_p50,a_p50):.3f}x |"
            )
print()

print("### Wide-window gain and owner scaling")
print()
print("| transport | body | size | owners | default ops/s | wide ops/s | wide/default | wide owner speedup vs 1 |")
print("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |")
for transport in transports:
    for body, sizes in (("copy", (32768, 65535, 65536, 131072)),
                        ("retained", retained_sizes)):
        for size in sizes:
            d = f"{transport}-{body}-{size}-default"
            w = f"{transport}-{body}-{size}-wide"
            wide_base = med(w, 1, "ops_per_second")
            for owners in (1, 4):
                d_ops = med(d, owners, "ops_per_second")
                w_ops = med(w, owners, "ops_per_second")
                print(
                    f"| {transport} | {body} | {size} | {owners} | {d_ops:.1f} | "
                    f"{w_ops:.1f} | {ratio(w_ops,d_ops):.3f}x | "
                    f"{ratio(w_ops,wide_base):.3f}x |"
                )
print()

print("### Copied vs retained equivalence")
print()
print("| transport | size | window | owners | copy ops/s | retained ops/s | retained/copy |")
print("| --- | ---: | --- | ---: | ---: | ---: | ---: |")
for transport in transports:
    for size in retained_sizes:
        for window in windows:
            for owners in (1, 4):
                c = f"{transport}-copy-{size}-{window}"
                r = f"{transport}-retained-{size}-{window}"
                c_ops = med(c, owners, "ops_per_second")
                r_ops = med(r, owners, "ops_per_second")
                print(
                    f"| {transport} | {size} | {window} | {owners} | {c_ops:.1f} | "
                    f"{r_ops:.1f} | {ratio(r_ops,c_ops):.3f}x |"
                )
print()

print("### Nagle/delayed-ACK controls")
print()
print("| transport | size | window | owners | nodelay ops/s | platform-default ops/s | default/nodelay |")
print("| --- | ---: | --- | ---: | ---: | ---: | ---: |")
for transport in transports:
    for size in (65535, 65536):
        for window in windows:
            on = f"{transport}-copy-{size}-{window}"
            off = f"{transport}-copy-{size}-{window}-nagle"
            for owners in (1, 4):
                on_ops = med(on, owners, "ops_per_second")
                off_ops = med(off, owners, "ops_per_second")
                print(
                    f"| {transport} | {size} | {window} | {owners} | {on_ops:.1f} | "
                    f"{off_ops:.1f} | {ratio(off_ops,on_ops):.3f}x |"
                )
print()

print(
    "Correctness gate: all 44 workloads x owners 1/2/4 are equal-work, zero-error, "
    "zero-rejection, balanced-lease, exact admission-handoff, and zero owner-to-owner "
    "steady-state handoff."
)
print(
    "This is evidence-only. The issue-level decision must distinguish flow-control, "
    "frame-size, TLS, retained/copy, and Nagle effects before exposing any public policy."
)
