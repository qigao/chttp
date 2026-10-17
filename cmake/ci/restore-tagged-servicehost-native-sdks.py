#!/usr/bin/env python3
"""Resolve ONLY immutable published release tags, then reuse exact SDK restore.

The user selects Salts and SaltsUtils release tags, never commit SHAs or
floating NuGet versions. GitHub's actual release/tag identity and uploaded
nupkg SHA256 are fetched for verification, not used as selection fallbacks.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

SALTS_TAG = re.compile(r"v2\.3\.0(?:-rc\.([1-9][0-9]*))?\Z")
UTILS_TAG = re.compile(r"v4\.3\.0(?:-rc\.([1-9][0-9]*))?\Z")
HEX40 = re.compile(r"[0-9a-f]{40}\Z")
HEX64 = re.compile(r"[0-9a-f]{64}\Z")


def tag_version(tag: str, family: str) -> str:
    if family == "Salts":
        match = SALTS_TAG.fullmatch(tag)
        lower_bound = 17
    elif family == "SaltsUtils":
        match = UTILS_TAG.fullmatch(tag)
        lower_bound = 11
    else:
        raise ValueError("Unknown canonical SDK family")
    if match is None:
        raise ValueError(f"{family}: expected one explicit published release tag")
    if match.group(1) is not None and int(match.group(1)) <= lower_bound:
        raise ValueError(
            f"{family}: selected prerelease predates canonical ServiceHost")
    return tag[1:]


def gh_json(resource: str) -> dict:
    stdout = subprocess.run(
        ["gh", "api", resource], check=True, capture_output=True, text=True,
        encoding="utf-8").stdout
    value = json.loads(stdout)
    if not isinstance(value, dict):
        raise ValueError(f"Malformed GitHub API payload: {resource}")
    return value


def release_identity(
    repo: str, package_id: str, tag: str, version: str
) -> tuple[str, str]:
    release = gh_json(f"repos/{repo}/releases/tags/{tag}")
    commit = gh_json(f"repos/{repo}/commits/{tag}")
    filename = f"{package_id}.{version}.nupkg"
    assets = [asset for asset in release.get("assets", [])
              if asset.get("name") == filename]
    if (release.get("tag_name") != tag
            or release.get("draft") is not False
            or release.get("prerelease") != ("-rc." in version)
            or len(assets) != 1):
        raise ValueError(f"{repo}: no exactly matching published SDK release")
    sha = commit.get("sha", "")
    digest = assets[0].get("digest", "")
    if (HEX40.fullmatch(sha) is None or
            not isinstance(digest, str) or
            not digest.startswith("sha256:") or
            HEX64.fullmatch(digest[7:]) is None or
            assets[0].get("state") != "uploaded" or
            not isinstance(assets[0].get("size"), int) or
            assets[0]["size"] <= 0):
        raise ValueError(f"{repo}: no verifiable immutable SDK archive")
    return sha, digest[7:]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--salts-tag", required=True)
    ap.add_argument("--utils-tag", required=True)
    ap.add_argument("--rid", default="linux-x64")
    ap.add_argument("--host-rid", default="linux-x64")
    args = ap.parse_args()
    salts_version = tag_version(args.salts_tag, "Salts")
    utils_version = tag_version(args.utils_tag, "SaltsUtils")
    salts_sha, salts_digest = release_identity(
        "qigao/salts", "Salts.Native", args.salts_tag, salts_version)
    utils_sha, utils_digest = release_identity(
        "qigao/salts-utils", "SaltsUtils.Native",
        args.utils_tag, utils_version)
    exact_restore = Path(__file__).with_name(
        "restore-exact-published-native-sdks.py")
    subprocess.run([
        sys.executable, str(exact_restore),
        "--salts-version", salts_version,
        "--utils-version", utils_version,
        "--salts-sha", salts_sha,
        "--utils-sha", utils_sha,
        "--salts-digest", salts_digest,
        "--utils-digest", utils_digest,
        "--rid", args.rid,
        "--host-rid", args.host_rid
    ], check=True)
    print(
        f"Validated exact published SDK tags: "
        f"{args.salts_tag}, {args.utils_tag}; no source fallback")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError,
            json.JSONDecodeError) as exc:
        print(f"Tagged ServiceHost SDK admission rejected: {exc}",
              file=sys.stderr)
        sys.exit(1)
