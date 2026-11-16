#!/usr/bin/env python3
"""Bootstrap the pinned shared vcpkg contract without its Mono/NuGet action."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

from github_packages import export


def output(*args):
    return subprocess.check_output(args, text=True).strip()


def run(*args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("contract", type=Path)
    args = parser.parse_args()
    contract = args.contract.resolve()
    workspace = Path(os.environ["GITHUB_WORKSPACE"])
    temporary = Path(os.environ["RUNNER_TEMP"])

    scripts = (contract / "vcpkg-scripts-revision.txt").read_text().strip()
    tool = (contract / "vcpkg-tool-version.txt").read_text().strip()
    if not re.fullmatch(r"[0-9a-f]{40}", scripts):
        raise ValueError("invalid canonical vcpkg scripts revision")

    root = temporary / ("qigao-vcpkg-" + scripts)
    reusable = (root / ".git").is_dir()
    if reusable:
        reusable = (
            output("git", "-C", str(root), "rev-parse", "HEAD") == scripts
            and output(
                "git", "-C", str(root), "rev-parse", "--is-shallow-repository"
            ) == "false"
            and subprocess.run(
                ["git", "-C", str(root), "config", "--get", "remote.origin.promisor"],
                capture_output=True,
                text=True,
            ).stdout.strip() != "true"
        )
    if not reusable:
        shutil.rmtree(root, ignore_errors=True)
        root.mkdir(parents=True)
        run("git", "-C", str(root), "init")
        run(
            "git", "-C", str(root), "remote", "add", "origin",
            "https://github.com/microsoft/vcpkg.git",
        )
        for attempt in range(3):
            result = subprocess.run(
                [
                    "git", "-c", "protocol.version=2", "-C", str(root),
                    "fetch", "--no-tags", "origin", scripts,
                ]
            )
            if result.returncode == 0:
                break
            if attempt == 2:
                result.check_returncode()
            time.sleep(2 * (attempt + 1))
        run("git", "-C", str(root), "checkout", "--detach", "FETCH_HEAD")

    baselines = output(
        sys.executable,
        str(contract / "scripts/vcpkg-required-commits.py"),
        str(contract),
        str(workspace),
    ).splitlines()
    for baseline in baselines:
        if subprocess.run(
            ["git", "-C", str(root), "cat-file", "-e", baseline + "^{commit}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode:
            run("git", "-C", str(root), "fetch", "--no-tags", "origin", baseline)
        run(
            "git", "-C", str(root), "cat-file", "-e",
            baseline + ":versions/baseline.json",
        )

    if (
        output("git", "-C", str(root), "rev-parse", "HEAD") != scripts
        or output(
            "git", "-C", str(root), "rev-parse", "--is-shallow-repository"
        ) != "false"
        or subprocess.run(
            [
                "git", "-C", str(root), "config", "--get",
                "remote.origin.partialclonefilter",
            ],
            capture_output=True,
            text=True,
        ).stdout.strip()
    ):
        raise ValueError("vcpkg checkout violates the canonical full-history contract")

    if os.name == "nt":
        run("cmd", "/c", "bootstrap-vcpkg.bat", "-disableMetrics", cwd=root)
        executable = root / "vcpkg.exe"
    else:
        run("sh", "./bootstrap-vcpkg.sh", "-disableMetrics", cwd=root)
        executable = root / "vcpkg"

    if not output(str(executable), "version").splitlines()[0].endswith(tool):
        raise ValueError("canonical vcpkg tool version mismatch")

    version = (contract / "cache-contract-version.txt").read_text().strip()
    revision = output(
        sys.executable,
        str(contract / "scripts/cache-contract-revision.py"),
        str(contract),
    )
    canonical = temporary / (
        "qigao-vcpkg-contract-v" + version + "-" + revision
    )
    canonical.mkdir(exist_ok=True)
    for name in ("ports", "triplets"):
        shutil.copytree(
            contract / name,
            canonical / name,
            dirs_exist_ok=True,
        )
    (canonical / "cmake").mkdir(exist_ok=True)
    shutil.copy2(
        contract / "cmake/QigaoVcpkgToolchain.cmake",
        canonical / "cmake",
    )
    (canonical / ".qigao-cache-revision").write_text(revision)

    cache = workspace / "build/vcpkg-binary-cache"
    cache.mkdir(parents=True, exist_ok=True)
    ready = temporary / "turboscript-curl-cache-ready.json"
    ready.unlink(missing_ok=True)
    log = temporary / "turboscript-curl-cache.log"
    with log.open("w") as output_file:
        process = subprocess.Popen(
            [
                sys.executable,
                str(Path(__file__).with_name("github_packages.py")),
                "serve-cache",
                "--packages", str(cache),
                "--ready", str(ready),
            ],
            stdin=subprocess.DEVNULL,
            stdout=output_file,
            stderr=subprocess.STDOUT,
            start_new_session=os.name != "nt",
        )

    for _ in range(100):
        if ready.is_file():
            try:
                port = json.loads(ready.read_text())["port"]
                break
            except (ValueError, KeyError):
                pass
        if process.poll() is not None:
            raise RuntimeError(
                "curl binary-cache adapter failed; see " + str(log)
            )
        time.sleep(0.1)
    else:
        process.terminate()
        raise RuntimeError("curl binary-cache adapter did not start")

    values = {
        "VCPKG_ROOT": root,
        "VCPKG_TOOL_REVISION": tool,
        "VCPKG_SCRIPTS_REVISION": scripts,
        "VCPKG_CACHE_CONTRACT_VERSION": version,
        "VCPKG_CACHE_REVISION": revision,
        "VCPKG_CACHE_REPOSITORY_ROOT": canonical,
        "VCPKG_BINARY_CACHE": cache,
        "VCPKG_NUGET_REPOSITORY": "https://github.com/qigao/vcpkg-cache",
        "VCPKG_OVERLAY_PORTS": canonical / "ports",
        "VCPKG_OVERLAY_TRIPLETS": canonical / "triplets",
        "QIGAO_VCPKG_TOOLCHAIN_FILE": (
            canonical / "cmake/QigaoVcpkgToolchain.cmake"
        ),
        "QIGAO_VCPKG_WINDOWS_TRIPLET": "qigao-x64-windows-v145",
        "QIGAO_VCPKG_WINDOWS_HOST_TRIPLET": "qigao-x64-windows-v145",
        "VCPKG_BINARY_SOURCES": (
            "clear;files," + str(cache) + ",readwrite;"
            + "http,http://127.0.0.1:" + str(port)
            + "/{name}/{triplet}/{version}/{sha},read"
        ),
    }
    if (
        os.environ.get("RUNNER_OS") == "Linux"
        and os.environ.get("RUNNER_ARCH") == "ARM64"
    ):
        values.update(
            VCPKG_FORCE_SYSTEM_BINARIES="1",
            VCPKG_FORCE_DOWNLOADED_BINARIES="1",
        )
    export(values)


if __name__ == "__main__":
    main()
