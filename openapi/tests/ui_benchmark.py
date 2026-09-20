"""Reproducible same-runner OpenAPI UI startup/render measurements.

This is evidence, not a pass/fail performance threshold. Startup includes process
launch, OpenAPI parse/model projection, template bundle load/compile/probe, and
server start.
"""

import argparse
import json
import os
import pathlib
import platform
import re
import statistics
import subprocess
import tempfile
import time
import urllib.request


def percentile(samples, fraction):
    ordered = sorted(samples)
    return ordered[round((len(ordered) - 1) * fraction)]


def make_document(count):
    paths = {}
    for index in range(count):
        paths[f"/operation/{index}"] = {
            "get": {
                "operationId": f"op{index}",
                "summary": f"Operation {index}",
                "description": "benchmark operation",
                "responses": {"200": {"description": "OK"}},
            }
        }
    return {
        "openapi": "3.1.0",
        "info": {"title": f"Benchmark {count}", "version": "1"},
        "servers": [{"url": "https://api.example.test/v1"}],
        "paths": paths,
    }


def start_server(server, document, assets):
    started = time.perf_counter_ns()
    process = subprocess.Popen(
        [server, str(document), assets],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    line = process.stdout.readline()
    elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000
    match = re.search(r"http://127\.0\.0\.1:\d+", line)
    if not match:
        _, stderr = process.communicate("\n", timeout=10)
        raise RuntimeError(f"server failed to start: {line!r} {stderr}")
    return process, match.group(), elapsed_ms


def stop_server(process):
    _, stderr = process.communicate("\n", timeout=10)
    if process.returncode != 0:
        raise RuntimeError(stderr)


def request_ms(url):
    started = time.perf_counter_ns()
    with urllib.request.urlopen(url, timeout=10) as response:
        body = response.read()
        if response.status != 200 or not body:
            raise RuntimeError(f"bad HTTP response: {response.status}")
    elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000
    return elapsed_ms, len(body)


def measure(server, assets, count, startup_samples, warmup, samples):
    with tempfile.TemporaryDirectory() as tmp:
        document = pathlib.Path(tmp) / "openapi.json"
        document.write_text(
            json.dumps(make_document(count), separators=(",", ":")),
            encoding="utf-8",
        )

        startup = []
        process = None
        origin = None
        try:
            for attempt in range(startup_samples):
                process, origin, elapsed = start_server(server, document, assets)
                startup.append(elapsed)
                if attempt + 1 != startup_samples:
                    stop_server(process)
                    process = None

            for _ in range(warmup):
                request_ms(origin + "/docs")
                request_ms(origin + "/docs/operations/op0")

            docs = []
            details = []
            docs_bytes = detail_bytes = 0
            for _ in range(samples):
                elapsed, docs_bytes = request_ms(origin + "/docs")
                docs.append(elapsed)
                elapsed, detail_bytes = request_ms(
                    origin + "/docs/operations/op0"
                )
                details.append(elapsed)

            return {
                "operations": count,
                "document_bytes": document.stat().st_size,
                "warmup": warmup,
                "startup_ms": {
                    "samples": startup_samples,
                    "total": sum(startup),
                    "mean": statistics.fmean(startup),
                    "p50": percentile(startup, 0.50),
                    "p95": percentile(startup, 0.95),
                },
                "docs_render_http_ms": {
                    "samples": samples,
                    "total": sum(docs),
                    "mean": statistics.fmean(docs),
                    "p50": percentile(docs, 0.50),
                    "p95": percentile(docs, 0.95),
                    "output_bytes": docs_bytes,
                },
                "detail_render_http_ms": {
                    "samples": samples,
                    "total": sum(details),
                    "mean": statistics.fmean(details),
                    "p50": percentile(details, 0.50),
                    "p95": percentile(details, 0.95),
                    "output_bytes": detail_bytes,
                },
            }
        finally:
            if process is not None and process.poll() is None:
                stop_server(process)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("server")
    parser.add_argument("assets")
    parser.add_argument("--counts", default="44,4096")
    parser.add_argument("--startup-samples", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--samples", type=int, default=10)
    args = parser.parse_args()

    metadata = {
        "kind": "environment",
        "commit": os.environ.get("GITHUB_SHA", "unknown"),
        "salts_sha": os.environ.get("SALTS_SHA", "unknown"),
        "salts_utils_sha": os.environ.get("SALTS_UTILS_SHA", "unknown"),
        "vcpkg_sha": os.environ.get("VCPKG_SHA", "unknown"),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "gcc": subprocess.check_output(
            ["gcc", "-dumpfullversion", "-dumpversion"], text=True
        ).strip(),
        "cmake": subprocess.check_output(
            ["cmake", "--version"], text=True
        ).splitlines()[0],
        "cpu_count": os.cpu_count(),
        "note": (
            "startup includes process launch, OpenAPI parse/model projection, "
            "CHttp::Web/Jinja template-bundle load/compile/probe, and server start"
        ),
    }
    print(json.dumps(metadata, sort_keys=True), flush=True)

    for count in (int(value) for value in args.counts.split(",")):
        result = measure(
            args.server,
            args.assets,
            count,
            args.startup_samples,
            args.warmup,
            args.samples,
        )
        result["kind"] = "measurement"
        print(json.dumps(result, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
