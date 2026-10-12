#!/usr/bin/env python3
"""Strict draft-only six-RID Salts + SaltsUtils NuGet restoration for CHttp CI.

All versions/hashes/producer workflow IDs are immutable. No published SDK
fallback and no package registry write. Only transient HTTP 502/503/504 is retried.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time
import zipfile
from xml.etree import ElementTree

SALTS = "21bdb1145fbb2b39ee3fa644173ec037adba7b87"
UTILS = "01cbc2f0e9566927331e783c1246e9d61636e6a6"
PRODUCERS = (
    ("salts", "qigao/salts", 38248972051, 1150,
     "test/cnet-module-once-tsan-1143", SALTS,
     "salts-native-nuget", "Salts.Native",
     f"2.3.0-cnet.once.sha{SALTS}",
     "aba3cc5242a80c162daa9155cdf8986baaa308d74e476db3d2982d9e263e1100",
     "salts-sdk-manifest.txt"),
    ("utils", "qigao/salts-utils", 38250938281, 623,
     "test/cnet-once-six-rid-sdk-1143", UTILS,
     "salts-utils-native-nuget", "SaltsUtils.Native",
     f"4.3.0-cnet.once.sha{UTILS}.salts.sha{SALTS}",
     "9449f11acde09f55a70d0855af297079bda1003271d573f1533a7bb88fec3bb1",
     "salts-utils-sdk-manifest.txt"),
)
RIDS = (
    "linux-x64", "linux-arm64", "windows-x64", "macos-arm64",
    "android-arm64-v8a", "ios-arm64",
)


def gh(*args: str) -> str:
    """Bound retries to network/transient GitHub 5xx failures only."""
    for attempt in range(1, 4):
        proc = subprocess.run(
            ["gh", *args], text=True, capture_output=True, check=False,
        )
        if proc.returncode == 0:
            return proc.stdout
        error = proc.stderr + "\n" + proc.stdout
        if attempt == 3 or re.search(r"HTTP\s+(?:502|503|504)\b", error) is None:
            raise RuntimeError(f"GitHub artifact read failed: {' '.join(args[:4])}: "
                               f"{error.strip()[:800]}")
        time.sleep(2 * attempt)
    raise AssertionError("unreachable")


def check_producer(item: tuple) -> None:
    label, repo, run_id, pr, branch, sha, *_ = item
    run = json.loads(gh("api", f"repos/{repo}/actions/runs/{run_id}"))
    expected = {
        "head_sha": sha, "head_branch": branch, "event": "pull_request",
        "path": ".github/workflows/cnet-once-six-rid-candidate.yml",
        "status": "completed", "conclusion": "success",
    }
    for key, value in expected.items():
        if run.get(key) != value:
            raise ValueError(f"{label}: producer {key}={run.get(key)!r}, "
                             f"expected {value!r}")
    if run.get("repository", {}).get("full_name") != repo or not any(
        p.get("number") == pr and p.get("head", {}).get("sha") == sha
        for p in run.get("pull_requests", [])
    ):
        raise ValueError(f"{label}: producer repository/PR identity mismatch")


def archive_identity(item: tuple, pkg: Path) -> zipfile.ZipFile:
    label, _, _, _, _, sha, _, package_id, version, expected_sha256, manifest_name = item
    sha256 = hashlib.sha256(pkg.read_bytes()).hexdigest()
    if sha256 != expected_sha256:
        raise ValueError(f"{label}: expected package SHA256 {expected_sha256}; got {sha256}")
    z = zipfile.ZipFile(pkg)
    names = set(z.namelist())
    expected_manifests = {f"sdk/{rid}/{manifest_name}" for rid in RIDS}
    actual_manifests = {n for n in names if n.endswith("/" + manifest_name)}
    if actual_manifests != expected_manifests:
        raise ValueError(f"{label}: missing/extra RID manifests "
                         f"{actual_manifests ^ expected_manifests}")
    nuspecs = [n for n in names if n.endswith(".nuspec")]
    if len(nuspecs) != 1:
        raise ValueError(f"{label}: NuGet identity metadata missing or duplicated")
    fields = {
        tag.tag.rsplit("}", 1)[-1]: tag.text
        for tag in ElementTree.fromstring(z.read(nuspecs[0])).iter()
        if tag.tag.rsplit("}", 1)[-1] in ("id", "version")
    }
    if (fields.get("id"), fields.get("version")) != (package_id, version):
        raise ValueError(f"{label}: NuGet ID/version mismatch {fields}")
    for rid in RIDS:
        p = f"sdk/{rid}/"
        manifest = dict(line.split("=", 1)
                        for line in z.read(p + manifest_name)
                            .decode("utf-8-sig").splitlines() if "=" in line)
        expected = {"version": version, "commit": sha, "profile": "release"}
        if label == "utils":
            expected["salts_version"] = "candidate-" + SALTS
        for field, want in expected.items():
            if manifest.get(field) != want:
                raise ValueError(f"{label}/{rid}: {field}={manifest.get(field)!r} != {want!r}")
        config = ("Salts" if label == "salts" else "SaltsUtils")
        if p + f"lib/cmake/{config}/{config}Config.cmake" not in names:
            raise ValueError(f"{label}/{rid}: installed CMake config missing")
        if label == "salts" and rid != "windows-x64":
            if b"pthread_once_t native;" not in z.read(p + "include/salts/thread.h"):
                raise ValueError(f"{rid}: expected POSIX pthread_once ABI missing")
        if label == "utils" and rid in ("linux-x64", "linux-arm64", "macos-arm64"):
            tool = p + "bin/salts-idlc"
            mode = (z.getinfo(tool).external_attr >> 16) & 0o777
            if mode & 0o111 != 0o111:
                raise ValueError(f"{label}/{rid}: host salts-idlc is not executable")
    print(f"Validated {package_id}={version} SHA256={sha256}", flush=True)
    return z


def extract_rids(archive: zipfile.ZipFile, root: Path, needed: set[str]) -> None:
    names = {f"sdk/{rid}/" for rid in needed}
    for info in archive.infolist():
        prefix = next((pre for pre in names if info.filename.startswith(pre)), None)
        if prefix is None:
            continue
        relative = Path(info.filename[len(prefix):])
        if not relative.parts:
            continue
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"unsafe NuGet archive member {info.filename!r}")
        target = root / prefix / relative
        if info.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        mode = (info.external_attr >> 16)
        if stat.S_ISLNK(mode):
            raise ValueError(f"symlink archive member not admitted: {info.filename}")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(archive.read(info.filename))
        permissions = mode & 0o777
        if os.name != "nt" and permissions:
            target.chmod(permissions)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rid", required=True, choices=RIDS)
    ap.add_argument("--host-rid", required=True, choices=RIDS)
    ns = ap.parse_args()
    required_env = ("RUNNER_TEMP", "GITHUB_ENV", "GITHUB_PATH",
                    "GH_TOKEN", "GITHUB_WORKSPACE")
    for key in required_env:
        if not os.getenv(key):
            raise RuntimeError(f"{key} missing")
    if ns.host_rid not in ("linux-x64", "linux-arm64", "macos-arm64", "windows-x64"):
        raise ValueError("Unsupported non-host compiler RID")
    temp = Path(os.environ["RUNNER_TEMP"]) / "chttp-cnet-once-candidate-nupkgs"
    if temp.exists():
        raise ValueError(f"Refusing existing upstream candidate root {temp}")
    temp.mkdir(parents=True)
    dist = temp / "dist"
    dist.mkdir()
    facts = [f"rid={ns.rid}", f"host_rid={ns.host_rid}",
             f"salts_source={SALTS}", f"utils_source={UTILS}",
             "used_original_install_trees=false", "published=false"]
    for item in PRODUCERS:
        label, repo, run_id, _, _, _, artifact, package_id, version, _, _ = item
        check_producer(item)
        downloaded = dist / label
        downloaded.mkdir()
        gh("run", "download", str(run_id), "--repo", repo,
           "--name", artifact, "--dir", str(downloaded))
        path = downloaded / f"{package_id}.{version}.nupkg"
        if not path.is_file():
            raise ValueError(f"Exact upstream NuGet artifact missing: {path}")
        with archive_identity(item, path) as archive:
            extract_rids(archive, temp / label, {ns.rid, ns.host_rid})
        facts.append(f"{label}_version={version}")
        facts.append(f"{label}_sha256={hashlib.sha256(path.read_bytes()).hexdigest()}")
    salts = (temp / "salts/sdk" / ns.rid).resolve()
    utils = (temp / "utils/sdk" / ns.rid).resolve()
    host_salts = (temp / "salts/sdk" / ns.host_rid).resolve()
    host_utils = (temp / "utils/sdk" / ns.host_rid).resolve()
    for f in [
        salts / "lib/cmake/Salts/SaltsConfig.cmake",
        utils / "lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake",
        host_salts / "lib/cmake/Salts/SaltsConfig.cmake",
        utils / "include/data_bind_native_binding.h",
    ]:
        if not f.is_file():
            raise ValueError(f"Missing extracted SDK member: {f}")
    tool = host_utils / "bin" / ("salts-idlc.exe" if os.name == "nt" else "salts-idlc")
    if not tool.is_file():
        raise ValueError(f"Missing exact host IDL compiler: {tool}")
    if os.name != "nt" and not os.access(tool, os.X_OK):
        raise ValueError(f"IDL compiler lost executable mode: {tool}")
    env = {
        "SALTS_ROOT": str(salts),
        "SALTS_UTILS_ROOT": str(utils),
        "DATABIND_ROOT": str(utils),
        "SALTS_HOST_ROOT": str(host_salts),
        "SALTS_UTILS_HOST_ROOT": str(host_utils),
        "EXACT_SALTS_IDLC": str(tool),
    }
    with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8") as f:
        for key, value in env.items():
            f.write(f"{key}={value}\n")
        if os.name != "nt":
            prefix = os.pathsep.join(str(path) for path in
                (utils / "lib", utils / "bin", salts / "lib",
                 host_utils / "lib", host_salts / "lib"))
            for var in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
                f.write(f"{var}={prefix}:{os.getenv(var, '')}\n")
    with open(os.environ["GITHUB_PATH"], "a", encoding="utf-8") as f:
        for path in (utils / "bin", salts / "bin", host_utils / "bin", host_salts / "bin"):
            f.write(f"{path}\n")
    results = Path(os.environ["GITHUB_WORKSPACE"]) / "build/upstream-cnet-once-candidate"
    results.mkdir(parents=True, exist_ok=True)
    (results / f"provenance-{ns.rid}.txt").write_text(
        "\n".join(facts) + "\n", encoding="utf-8")
    print(f"Installed exact unpublished SDK pair for {ns.rid}, host {ns.host_rid}")


if __name__ == "__main__":
    main()
