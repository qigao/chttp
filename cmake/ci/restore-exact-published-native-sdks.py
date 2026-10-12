#!/usr/bin/env python3
"""Fail-closed restoration of two EXACT, formally published native SDKs.

Used only by the manual CHttp cutover preflight. No wildcard NuGet versions,
candidate-source fallback, package publishing, or automatic release selection.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from xml.sax.saxutils import escape
import zipfile

SALTS_VERSION_RE = re.compile(r"2\.3\.0(?:-rc\.[1-9][0-9]*)?\Z")
UTILS_VERSION_RE = re.compile(r"4\.3\.0(?:-rc\.[1-9][0-9]*)?\Z")
SOURCE_RE = re.compile(r"[0-9a-f]{40}\Z")
SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")
RIDS = (
    "linux-x64", "linux-arm64", "windows-x64", "macos-arm64",
    "android-arm64-v8a", "ios-arm64",
)
# The IDL generator executes on the CI host, not on the cross-compiled target.
HOST_RIDS = {
    "linux-x64": "linux-x64", "linux-arm64": "linux-arm64",
    "windows-x64": "windows-x64", "macos-arm64": "macos-arm64",
    "android-arm64-v8a": "linux-x64", "ios-arm64": "macos-arm64",
}
RID = "linux-x64"  # Preserve direct helper-test compatibility.



def validate_inputs(args: argparse.Namespace) -> None:
    for name, value, pattern in (
        ("Salts version", args.salts_version, SALTS_VERSION_RE),
        ("SaltsUtils version", args.utils_version, UTILS_VERSION_RE),
        ("Salts source", args.salts_sha, SOURCE_RE),
        ("SaltsUtils source", args.utils_sha, SOURCE_RE),
        ("Salts package digest", args.salts_digest, SHA256_RE),
        ("SaltsUtils package digest", args.utils_digest, SHA256_RE),
    ):
        if pattern.fullmatch(value) is None:
            raise ValueError(f"{name} must be explicit and immutable: {value!r}")
    # rc.14 and rc.9 are the previously released, pre-pthread_once graph.
    # Do not accidentally "qualify" that known incompatible ABI epoch.
    if args.salts_version.startswith("2.3.0-rc.") and (
        int(args.salts_version.rsplit(".", 1)[1]) <= 14
    ):
        raise ValueError("Salts pre-pthread_once rc.14-or-earlier is ineligible")
    if args.utils_version.startswith("4.3.0-rc.") and (
        int(args.utils_version.rsplit(".", 1)[1]) <= 9
    ):
        raise ValueError("SaltsUtils pre-cutover rc.9-or-earlier is ineligible")
    if any(os.environ.get(name) for name in (
        "SALTS_CANDIDATE_ROOT", "SALTS_UTILS_CANDIDATE_ROOT"
    )):
        raise ValueError("Published package preflight cannot inherit Draft candidate roots")


def run(*argv: str) -> str:
    p = subprocess.run(argv, check=True, capture_output=True, text=True)
    return p.stdout.strip()


def verify_release_metadata(
    package_id: str, repo: str, version: str, expected_commit: str,
    expected_digest: str,
) -> dict:
    tag = "v" + version
    release = json.loads(run("gh", "api", f"repos/{repo}/releases/tags/{tag}"))
    commit = json.loads(run("gh", "api", f"repos/{repo}/commits/{tag}"))
    expected_asset = f"{package_id}.{version}.nupkg"
    assets = [entry for entry in release.get("assets", [])
              if entry.get("name") == expected_asset]
    if (release.get("tag_name") != tag or release.get("draft") is not False
            or len(assets) != 1 or commit.get("sha") != expected_commit):
        raise ValueError(f"{repo}: release/tag/source/asset mismatch for {tag}")
    if release.get("prerelease") != ("-rc." in version):
        raise ValueError(f"{repo}: incorrect release channel for {tag}")
    # Package-source bytes and the official release attachment must be the
    # SAME immutable payload; the caller-supplied digest alone is insufficient.
    # GitHub's release-asset digest is available for both rc.14 and rc.9.
    asset = assets[0]
    if (asset.get("digest") != "sha256:" + expected_digest or
            asset.get("state") != "uploaded" or
            not isinstance(asset.get("size"), int) or
            asset["size"] <= 0):
        raise ValueError(f"{repo}: release asset digest/state mismatch for {tag}")
    return {
        "repository": repo, "tag": tag, "commit": expected_commit,
        "asset": expected_asset, "asset_sha256": expected_digest,
        "release_url": release.get("html_url", ""),
    }


def select_package(assets: dict, package_id: str, version: str, packages: Path) -> Path:
    actual = [(key, data["path"]) for key, data in assets["libraries"].items()
              if key.lower().startswith(package_id.lower() + "/")]
    if len(actual) != 1 or actual[0][0].lower() != (
        package_id + "/" + version).lower():
        raise ValueError(f"{package_id}: resolved NuGet graph is not exactly {version}: {actual}")
    package = packages / actual[0][1]
    if not package.is_dir() or package.resolve() != package.absolute():
        raise ValueError(f"{package_id}: invalid restored package path {package}")
    return package


def verify_package(
    package: Path, package_id: str, version: str, source: str,
    expected_digest: str, salts_version: str, rid: str = RID,
) -> tuple[Path, str]:
    if rid not in RIDS:
        raise ValueError(f"Unsupported native RID {rid!r}")
    name = f"{package_id}.{version}.nupkg".lower()
    archives = [p for p in package.glob("*.nupkg") if p.name.lower() == name]
    if len(archives) != 1:
        raise ValueError(f"{package_id}: expected one restored .nupkg, got {archives}")
    digest = hashlib.sha256(archives[0].read_bytes()).hexdigest()
    if digest != expected_digest:
        raise ValueError(f"{package_id}: published package digest mismatch: {digest}")
    with zipfile.ZipFile(archives[0]) as archive:
        entries = set(archive.namelist())
        expected_prefix = f"sdk/{rid}/"
        manifest_name = ("salts-sdk-manifest.txt" if package_id == "Salts.Native"
                         else "salts-utils-sdk-manifest.txt")
        manifest_path = expected_prefix + manifest_name
        if manifest_path not in entries:
            raise ValueError(f"{package_id}: missing installed RID manifest")
        manifests = dict(line.split("=", 1) for line in
                         archive.read(manifest_path).decode("utf-8-sig").splitlines()
                         if "=" in line)
        required = {"version": version, "commit": source, "profile": "release"}
        if package_id == "SaltsUtils.Native":
            required["salts_version"] = salts_version
        for key, value in required.items():
            if manifests.get(key) != value:
                raise ValueError(f"{package_id}: {key}={manifests.get(key)!r} != {value!r}")
        spec = [p for p in entries if p.endswith(".nuspec")]
        if len(spec) != 1:
            raise ValueError(f"{package_id}: missing NuGet manifest")
        # Verify the NuGet identity of the actual downloaded payload as well.
        from xml.etree import ElementTree
        meta = {e.tag.rsplit("}", 1)[-1]: e.text
                for e in ElementTree.fromstring(archive.read(spec[0])).iter()
                if e.tag.rsplit("}", 1)[-1] in ("id", "version")}
        if (meta.get("id"), meta.get("version")) != (package_id, version):
            raise ValueError(f"{package_id}: NuGet metadata mismatch")
        if package_id == "Salts.Native":
            header = archive.read(expected_prefix + "include/salts/thread.h")
            if rid == "windows-x64":
                if b"volatile int state;" not in header or b"SALTS_ONCE_INIT {0}" not in header:
                    raise ValueError("Published Windows Salts lacks native once ABI")
            elif b"pthread_once_t native;" not in header:
                raise ValueError("Published native Salts lacks qualified pthread_once ABI")
    root = package / "sdk" / rid
    config = ("Salts" if package_id == "Salts.Native" else "SaltsUtils")
    if not (root / "lib" / "cmake" / config / f"{config}Config.cmake").is_file():
        raise ValueError(f"{package_id}: missing installed CMake exports")
    return root, digest


def validate_rid_pair(rid: str, host_rid: str) -> None:
    if rid not in HOST_RIDS or HOST_RIDS[rid] != host_rid:
        raise ValueError(f"Unsupported target/host SDK pair: {rid}/{host_rid}")


def append_env(path: Path, name: str, value: str) -> None:
    with path.open("a") as stream:
        stream.write(f"{name}={value}\n")


def exact_nuget_project(salts_version: str, utils_version: str) -> str:
    """NuGet requires bracketed [version] to pin a single exact version.

    Unbracketed PackageReference versions are minimum-range selectors;
    post-restore asset auditing alone is too late to prevent resolution drift.
    """
    return (
        '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>'
        '<TargetFramework>net8.0</TargetFramework>'
        '<RestorePackagesWithLockFile>false</RestorePackagesWithLockFile>'
        '</PropertyGroup><ItemGroup>'
        f'<PackageReference Include="Salts.Native" Version="[{escape(salts_version)}]"/>'
        f'<PackageReference Include="SaltsUtils.Native" Version="[{escape(utils_version)}]"/>'
        '</ItemGroup></Project>\n'
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--salts-version", required=True)
    parser.add_argument("--utils-version", required=True)
    parser.add_argument("--salts-sha", required=True)
    parser.add_argument("--utils-sha", required=True)
    parser.add_argument("--salts-digest", required=True)
    parser.add_argument("--utils-digest", required=True)
    parser.add_argument("--rid", choices=RIDS, default=RID)
    parser.add_argument("--host-rid", choices=RIDS, default=RID)
    args = parser.parse_args()
    validate_rid_pair(args.rid, args.host_rid)
    validate_inputs(args)
    for name in ("GITHUB_TOKEN", "RUNNER_TEMP", "GITHUB_ENV", "GITHUB_PATH"):
        if not os.environ.get(name):
            raise ValueError(f"Missing CI environment: {name}")
    release_proof = [
        verify_release_metadata("Salts.Native", "qigao/salts",
                                args.salts_version, args.salts_sha,
                                args.salts_digest),
        verify_release_metadata("SaltsUtils.Native", "qigao/salts-utils",
                                args.utils_version, args.utils_sha,
                                args.utils_digest),
    ]
    work = Path(os.environ["RUNNER_TEMP"]) / "chttp-exact-published-cutover"
    if work.exists():
        raise ValueError("Refusing a preexisting package restore directory")
    work.mkdir()
    project = work / "exact-native-graph.csproj"
    project.write_text(exact_nuget_project(args.salts_version, args.utils_version))
    root = Path(__file__).resolve().parents[2]
    packages = work / "packages"
    run("dotnet", "restore", str(project), "--packages", str(packages),
        "--configfile", str(root / "cmake/vcpkg-cache.nuget.config"),
        "--no-cache", "--force-evaluate")
    assets = json.loads((work / "obj/project.assets.json").read_text())
    salts_package = select_package(
        assets, "Salts.Native", args.salts_version, packages)
    utils_package = select_package(
        assets, "SaltsUtils.Native", args.utils_version, packages)
    salts_root, salts_digest = verify_package(
        salts_package, "Salts.Native", args.salts_version, args.salts_sha,
        args.salts_digest, args.salts_version, args.rid)
    utils_root, utils_digest = verify_package(
        utils_package, "SaltsUtils.Native", args.utils_version, args.utils_sha,
        args.utils_digest, args.salts_version, args.rid)
    host_salts, _ = verify_package(
        salts_package, "Salts.Native", args.salts_version, args.salts_sha,
        args.salts_digest, args.salts_version, args.host_rid)
    host_utils, _ = verify_package(
        utils_package, "SaltsUtils.Native", args.utils_version, args.utils_sha,
        args.utils_digest, args.salts_version, args.host_rid)
    idlc = host_utils / "bin" / (
        "salts-idlc.exe" if args.host_rid == "windows-x64" else "salts-idlc")
    if not idlc.is_file() or (
        os.name != "nt" and not os.access(idlc, os.X_OK)
    ):
        raise ValueError(f"Published host salts-idlc missing or nonexecutable: {idlc}")
    for name, path in (
        ("SALTS_ROOT", salts_root),
        ("SALTS_UTILS_ROOT", utils_root),
        ("DATABIND_ROOT", utils_root),
        ("SALTS_HOST_ROOT", host_salts),
        ("SALTS_UTILS_HOST_ROOT", host_utils),
        ("EXACT_SALTS_IDLC", idlc),
    ):
        append_env(Path(os.environ["GITHUB_ENV"]), name, str(path))
    if os.name != "nt":
        prefix = [utils_root / "lib", utils_root / "bin", salts_root / "lib",
                  host_utils / "lib", host_salts / "lib"]
        for name in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
            append_env(Path(os.environ["GITHUB_ENV"]), name, os.pathsep.join(
                [*(str(p) for p in prefix), os.environ.get(name, "")]
            ))
    with Path(os.environ["GITHUB_PATH"]).open("a") as out:
        for prefix in (utils_root / "bin", salts_root / "bin",
                       host_utils / "bin", host_salts / "bin"):
            out.write(str(prefix) + "\n")
    record = {
        "source_mode": "exact_published_sdk", "rid": args.rid,
        "host_rid": args.host_rid,
        "packages": release_proof,
        "digests": {"Salts.Native": salts_digest,
                    "SaltsUtils.Native": utils_digest},
        "publisher_status": "already_published_read_only",
        "restore_fallback": False,
    }
    out = root / "build/published-cutover"
    out.mkdir(parents=True, exist_ok=True)
    (out / "provenance.json").write_text(json.dumps(record, sort_keys=True, indent=2) + "\n")
    print(json.dumps(record, sort_keys=True, indent=2))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.CalledProcessError, zipfile.BadZipFile) as exc:
        print(f"Exact published native SDK preflight rejected: {exc}", file=sys.stderr)
        sys.exit(1)
