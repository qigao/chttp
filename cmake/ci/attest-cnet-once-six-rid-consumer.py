#!/usr/bin/env python3
"""Attest CHttp installed SDK against the exact unpublished CNet once NuGet pair."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess

SALTS = "21bdb1145fbb2b39ee3fa644173ec037adba7b87"
UTILS = "01cbc2f0e9566927331e783c1246e6a6"


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise SystemExit(reason)


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--rid", required=True)
    p.add_argument("--family", required=True)
    a = p.parse_args()
    repo = Path(os.environ["GITHUB_WORKSPACE"])
    exact_chttp = os.environ["EXPECTED_CHTTP"]
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
    for source, name, sha in (
        (Path(os.environ["SALTS_ROOT"]) / "salts-sdk-manifest.txt",
         "salts-dependency-manifest.txt", SALTS),
        (Path(os.environ["SALTS_UTILS_ROOT"]) / "salts-utils-sdk-manifest.txt",
         "saltsutils-dependency-manifest.txt", UTILS),
    ):
        require(source.is_file(), f"{a.rid}: candidate dependency manifest missing")
        info = dict(line.split("=", 1) for line in
                    source.read_text(encoding="utf-8-sig").splitlines()
                    if "=" in line)
        require(info.get("commit") == sha,
                f"{a.rid}: exact upstream SDK source mismatch for {name}")
        shutil.copyfile(source, installed / name)
    print(f"{a.rid}: installed CHttp SDK and exact Salts/Utils/host IDLC qualified")


if __name__ == "__main__":
    main()
