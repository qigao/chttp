#!/usr/bin/env python3
import json
import statistics
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit(
        "usage: verify_listener_admission_stall_benchmark.py <jsonl>"
    )

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
samples_per_point = int(envs[0].get("samples", 0))
if samples_per_point <= 0:
    raise SystemExit("invalid environment sample count")

for env in envs:
    if env.get("commit", "unknown") != commit:
        raise SystemExit("benchmark repeats changed exact head")
    if int(env.get("samples", 0)) != samples_per_point:
        raise SystemExit("benchmark repeats changed sample count")

groups = {}
for row in rows:
    if row.get("benchmark") != "chttp_server_listener_admission_stall":
        continue
    key = (int(row["target_work_ns"]), int(row["owners"]))
    groups.setdefault(key, []).append(row)

expected_work = {0, 50_000, 250_000, 1_000_000, 5_000_000}
if {work for work, _ in groups} != expected_work:
    raise SystemExit(
        f"unexpected work budgets {sorted({work for work, _ in groups})}"
    )

for work in sorted(expected_work):
    owner_set = {owners for budget, owners in groups if budget == work}
    if owner_set != {1, 2, 4}:
        raise SystemExit(
            f"work={work}: expected owners 1/2/4, got {sorted(owner_set)}"
        )

    for owners in (1, 2, 4):
        samples = groups[(work, owners)]
        if len(samples) != len(envs):
            raise SystemExit(
                f"work={work}/owners={owners}: expected {len(envs)} repeats, "
                f"got {len(samples)}"
            )

        for row in samples:
            if int(row.get("errors", -1)) != 0:
                raise SystemExit(
                    f"work={work}/owners={owners}: errors={row.get('errors')}"
                )
            if int(row.get("rejected_connections", -1)) != 0:
                raise SystemExit(
                    f"work={work}/owners={owners}: rejected connections"
                )
            if int(row.get("stall_owner", -1)) != 0:
                raise SystemExit(
                    f"work={work}/owners={owners}: stall connection left owner0"
                )
            if row.get("admitted_while_stalled") is not True:
                raise SystemExit(
                    f"work={work}/owners={owners}: probe admission did not complete "
                    "while owner0 remained gated"
                )
            if int(row.get("cross_owner_data_plane_hops", -1)) != 0:
                raise SystemExit(
                    f"work={work}/owners={owners}: unexpected data-plane hop"
                )

            point_samples = int(row["samples"])
            if point_samples != samples_per_point:
                raise SystemExit(
                    f"work={work}/owners={owners}: sample count changed"
                )
            if int(row["accepted_connections"]) != point_samples + 1:
                raise SystemExit(
                    f"work={work}/owners={owners}: accepted count "
                    f"{row['accepted_connections']} != {point_samples + 1}"
                )

            admissions = [
                int(row.get(f"owner{i}_admissions", 0))
                for i in range(owners)
            ]
            if sum(admissions) != point_samples:
                raise SystemExit(
                    f"work={work}/owners={owners}: admission sum "
                    f"{sum(admissions)} != {point_samples}"
                )
            if max(admissions) - min(admissions) > 1:
                raise SystemExit(
                    f"work={work}/owners={owners}: owner selection became "
                    f"unbalanced {admissions}"
                )

            for field in (
                "connect_p50_ns",
                "connect_p95_ns",
                "connect_p99_ns",
                "admission_p50_ns",
                "admission_p95_ns",
                "admission_p99_ns",
                "response_p50_ns",
                "response_p95_ns",
                "response_p99_ns",
            ):
                if int(row[field]) <= 0:
                    raise SystemExit(
                        f"work={work}/owners={owners}: invalid "
                        f"{field}={row[field]}"
                    )

            if work == 0 and int(row["burn_iterations"]) != 0:
                raise SystemExit(
                    f"owners={owners}: zero-work point has CPU burn"
                )
            if work != 0 and int(row["burn_iterations"]) == 0:
                raise SystemExit(
                    f"work={work}/owners={owners}: missing CPU burn"
                )


def median(work, owners, field):
    return statistics.median(
        float(row[field]) for row in groups[(work, owners)]
    )


print(f"exact head: {commit}")
print(f"samples per point: {samples_per_point}")
print(f"repeats per point: {len(envs)}")
print()
print(
    "| owner0 stall target | owners | calibrated ns | connect p95 ns | "
    "admission p50 ns | admission p95 ns | response p95 ns | "
    "admission p50 / calibrated | probe admissions |"
)
print(
    "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |"
)

for work in sorted(expected_work):
    for owners in (1, 2, 4):
        row = groups[(work, owners)][-1]
        calibrated = median(work, owners, "calibrated_work_ns")
        admission_p50 = median(work, owners, "admission_p50_ns")
        ratio = admission_p50 / calibrated if calibrated > 0 else 0.0
        admissions = [
            int(row.get(f"owner{i}_admissions", 0))
            for i in range(owners)
        ]
        print(
            f"| {work} | {owners} | {calibrated:.0f} | "
            f"{median(work, owners, 'connect_p95_ns'):.0f} | "
            f"{admission_p50:.0f} | "
            f"{median(work, owners, 'admission_p95_ns'):.0f} | "
            f"{median(work, owners, 'response_p95_ns'):.0f} | "
            f"{ratio:.2f}x | {'/'.join(map(str, admissions))} |"
        )
    print()

print("Owner-count comparison at the 5 ms primary-owner stall:")
one = median(5_000_000, 1, "admission_p50_ns")
two = median(5_000_000, 2, "admission_p50_ns")
four = median(5_000_000, 4, "admission_p50_ns")
print(
    f"- admission p50: owners=1 {one:.0f} ns; "
    f"owners=2 {two:.0f} ns; owners=4 {four:.0f} ns"
)
print(
    "- Correctness contract: the probe owner lease is acquired while owner0 remains "
    "blocked at the handler gate. The calibrated application CPU work starts only "
    "after this admission witness succeeds."
)
print(
    "- Timing is diagnostic rather than a pass/fail threshold: isolated listener "
    "admission should remain near the zero-work control instead of scaling with the "
    "later 50us..5ms owner0 CPU burn."
)
