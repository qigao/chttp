#!/usr/bin/env python3
"""Attest CHttp installed SDK against exact published Salts/SaltsUtils releases."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise SystemExit(reason)


def main() -> None:
    salts = os.environ.get("EXPECTED_SALTS", "")
    utils = os.environ.get("EXPECTED_UTILS", "")
    p = argparse.ArgumentParser()
    p.add_argument("--rid", required=True)
    p.add_argument("--family", required=True)
    a = p.parse_args()
    repo = Path(os.environ["GITHUB_WORKSPACE"])
    exact_chttp = os.environ["EXPECTED_CHTTP"]
    require(len(salts) == 40 and len(utils) == 40, "Missing source commits")
    salts_version = os.environ["EXPECTED_SALTS_VERSION"]
    utils_version = os.environ["EXPECTED_UTILS_VERSION"]
    require(a.rid in ("linux-x64", "linux-arm64", "windows-x64", "macos-arm64",
                      "android-arm64-v8a", "ios-arm64"),
            "Installed native RID outside the qualified release graph")
    head = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], text=True, cwd=repo).strip()
    require(head == exact_chttp, "CHttp source identity changed during SDK build")
    installed = repo / "stage" / "sdk" / a.rid
    config = installed / "lib/cmake/Chttp/ChttpConfig.cmake"
    targets = installed / "lib/cmake/Chttp/ChttpTargets.cmake"
    require(config.is_file() and targets.is_file(),
            f"{a.rid}: installed native CHttp CMake SDK missing")
    require("CHttp::RpcService" not in targets.read_text(errors="replace"),
            "Deleted independent RpcService owner unexpectedly exported")
    if a.family == "android":
        build = repo / "build/android-native-sdk"
    elif a.family == "ios":
        build = repo / "build" / a.rid
    else:
        build = repo / "build/native-sdk"
    cache = build / "CMakeCache.txt"
    require(cache.is_file(), f"{a.rid}: CMakeCache missing")
    entries = {}
    for line in cache.read_text(errors="replace").splitlines():
        if "=" in line and not line.startswith(("#", "//")):
            name, value = line.split("=", 1)
            entries[name] = value.replace("\\", "/").rstrip("/")
    for package, root in (
        ("Salts", os.environ["SALTS_ROOT"]),
        ("SaltsUtils", os.environ["SALTS_UTILS_ROOT"]),
    ):
        key = f"{package}_DIR:PATH"
        want = (Path(root) / "lib" / "cmake" / package).as_posix().replace("\\", "/")
        actual = entries.get(key)
        require(actual == want, f"{a.rid}: {key} is {actual!r}; expected {want!r}")
    idlc = Path(os.environ["EXACT_SALTS_IDLC"])
    idlc_setting = entries.get("SaltsUtils_IDLC_EXECUTABLE:FILEPATH", "")
    require(idlc_setting == idlc.as_posix().replace("\\", "/"),
            f"{a.rid}: IDLC compiler came from another SDK")
    for source, name, sha, version in (
        (Path(os.environ["SALTS_ROOT"]) / "salts-sdk-manifest.txt",
         "salts-dependency-manifest.txt", salts, salts_version),
        (Path(os.environ["SALTS_UTILS_ROOT"]) / "salts-utils-sdk-manifest.txt",
         "saltsutils-dependency-manifest.txt", utils, utils_version),
    ):
        require(source.is_file(), f"{a.rid}: candidate dependency manifest missing")
        info = dict(line.split("=", 1) for line in
                    source.read_text(encoding="utf-8-sig").splitlines()
                    if "=" in line)
        require(info.get("commit") == sha and info.get("version") == version
                and info.get("profile") == "release",
                f"{a.rid}: exact released SDK/version mismatch for {name}")
        if name == "saltsutils-dependency-manifest.txt":
            require(info.get("salts_version") == salts_version,
                    f"{a.rid}: SaltsUtils was not built against exact published Salts")
        shutil.copyfile(source, installed / name)
    result = repo / "build/published-cutover"
    result.mkdir(parents=True, exist_ok=True)
    proof = {
        "chttp_source": exact_chttp, "rid": a.rid, "family": a.family,
        "salts_source": salts, "salts_version": salts_version,
        "salts_sha256": os.environ["EXPECTED_SALTS_DIGEST"],
        "utils_source": utils, "utils_version": utils_version,
        "utils_sha256": os.environ["EXPECTED_UTILS_DIGEST"],
        "owner_target": "CHttp::Server/Client",
        "legacy_rpc_owner_exported": False,
        "published_release_files_only": True,
        "no_silent_fallback": True,
    }
    (result / f"provenance-{a.rid}.json").write_text(
        json.dumps(proof, indent=2, sort_keys=True) + "\n"
    )
    print(f"{a.rid}: exact published Salts/Utils, host IDLC and installed CHttp qualified")


if __name__ == "__main__":
    main()
