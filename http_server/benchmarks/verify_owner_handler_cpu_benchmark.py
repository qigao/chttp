#!/usr/bin/env python3
import json
import statistics
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit("usage: verify_owner_handler_cpu_benchmark.py <jsonl>")

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
connections = int(envs[0].get("connections", 0))
requests = int(envs[0].get("requests_per_connection", 0))
for env in envs:
    if env.get("commit", "unknown") != commit:
        raise SystemExit("benchmark repeats changed exact head")
    if int(env.get("connections", 0)) != connections:
        raise SystemExit("benchmark repeats changed connection count")
    if int(env.get("requests_per_connection", 0)) != requests:
        raise SystemExit("benchmark repeats changed request count")

groups = {}
for row in rows:
    if row.get("benchmark") != "chttp_server_owner_handler_cpu":
        continue
    key = (int(row["target_work_ns"]), int(row["owners"]))
    groups.setdefault(key, []).append(row)

expected_work = {0, 50_000, 250_000, 1_000_000, 5_000_000}
if {key[0] for key in groups} != expected_work:
    raise SystemExit(
        f"unexpected work budgets {sorted({key[0] for key in groups})}"
    )

for work in sorted(expected_work):
    owner_set = {owners for (budget, owners) in groups if budget == work}
    if owner_set != {1, 2, 4}:
        raise SystemExit(f"work={work}: expected owners 1/2/4, got {sorted(owner_set)}")
    for owners in (1, 2, 4):
        samples = groups[(work, owners)]
        if len(samples) != len(envs):
            raise SystemExit(
                f"work={work}/owners={owners}: expected {len(envs)} repeats, got {len(samples)}"
            )
        for row in samples:
            if int(row.get("errors", -1)) != 0:
                raise SystemExit(f"work={work}/owners={owners}: errors={row.get('errors')}")
            if int(row.get("rejected_connections", -1)) != 0:
                raise SystemExit(f"work={work}/owners={owners}: rejected connections")
            if int(row.get("accepted_connections", -1)) != connections:
                raise SystemExit(f"work={work}/owners={owners}: accepted count mismatch")

            leases = [int(row.get(f"owner{i}_leases", 0)) for i in range(owners)]
            if sum(leases) != connections or max(leases) - min(leases) > 1:
                raise SystemExit(
                    f"work={work}/owners={owners}: invalid owner leases {leases}"
                )

            expected_samples = (connections // 2) * requests
            if int(row["fast_samples"]) != expected_samples:
                raise SystemExit(f"work={work}/owners={owners}: fast sample mismatch")
            if int(row["slow_samples"]) != expected_samples:
                raise SystemExit(f"work={work}/owners={owners}: slow sample mismatch")

            fast_mask = int(row["fast_owner_mask"])
            slow_mask = int(row["slow_owner_mask"])
            all_mask = (1 << owners) - 1
            if (fast_mask | slow_mask) != all_mask:
                raise SystemExit(
                    f"work={work}/owners={owners}: route classes do not cover all owners "
                    f"fast={fast_mask:#x} slow={slow_mask:#x}"
                )
            if owners == 1:
                if fast_mask != 1 or slow_mask != 1:
                    raise SystemExit("single-owner route masks are invalid")
            elif fast_mask & slow_mask:
                raise SystemExit(
                    f"work={work}/owners={owners}: slow/fast connections share an owner "
                    f"fast={fast_mask:#x} slow={slow_mask:#x}"
                )

            for field in (
                "ops_per_second",
                "cpu_ns_per_op",
                "fast_p50_ns",
                "fast_p95_ns",
                "fast_p99_ns",
                "slow_p50_ns",
                "slow_p95_ns",
                "slow_p99_ns",
            ):
                if float(row[field]) <= 0:
                    raise SystemExit(
                        f"work={work}/owners={owners}: invalid {field}={row[field]}"
                    )

def median(work, owners, field):
    return statistics.median(
        float(row[field]) for row in groups[(work, owners)]
    )

print(f"exact head: {commit}")
print(f"connections: {connections} (alternating slow/fast)")
print(f"requests per connection: {requests}")
print(f"repeats per point: {len(envs)}")
print()
print(
    "| slow target | owners | fast p50 ns | fast p95 ns | fast p99 ns | "
    "slow p50 ns | ops/s | CPU ns/op | fast-owner mask | slow-owner mask |"
)
print(
    "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |"
)
for work in sorted(expected_work):
    for owners in (1, 2, 4):
        row = groups[(work, owners)][-1]
        print(
            f"| {work} | {owners} | "
            f"{median(work, owners, 'fast_p50_ns'):.0f} | "
            f"{median(work, owners, 'fast_p95_ns'):.0f} | "
            f"{median(work, owners, 'fast_p99_ns'):.0f} | "
            f"{median(work, owners, 'slow_p50_ns'):.0f} | "
            f"{median(work, owners, 'ops_per_second'):.1f} | "
            f"{median(work, owners, 'cpu_ns_per_op'):.1f} | "
            f"{int(row['fast_owner_mask']):#x} | {int(row['slow_owner_mask']):#x} |"
        )
    if work:
        one = median(work, 1, "fast_p95_ns")
        two = median(work, 2, "fast_p95_ns")
        four = median(work, 4, "fast_p95_ns")
        print(
            f"fast-route p95 isolation at slow={work} ns: "
            f"owner1/owner2={one / two:.2f}x, owner1/owner4={one / four:.2f}x"
        )
    print()

print(
    "Correctness gate: fixed owner leases stay balanced; sequential admission places "
    "alternating slow/fast connections on disjoint owners for owner_count 2/4. "
    "Performance rows quantify owner-local head-of-line blocking only; they do not "
    "authorize implicit worker-pool execution."
)
