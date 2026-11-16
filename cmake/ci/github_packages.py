#!/usr/bin/env python3
"""Restore TurboScript native SDK dependencies from GitHub Packages with curl.

Native dependency downloads intentionally avoid NuGet/Mono/.NET restore.
NuGet v3 metadata is used only for version selection and payload URLs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import socketserver
import stat
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import quote, unquote, urlsplit
import xml.etree.ElementTree as ET
import zipfile

FEED = "https://nuget.pkg.github.com/qigao/index.json"
IDENTIFIER = re.compile(r"^[A-Za-z0-9_.-]+$")
VERSION = re.compile(
    r"^(\d+)\.(\d+)\.(\d+)(?:\.([0-9]+))?"
    r"(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$"
)


class PackageMissing(RuntimeError):
    pass


def version_key(version):
    match = VERSION.fullmatch(version)
    if not match:
        raise ValueError("unsupported package version: " + version)
    major, minor, patch, revision, prerelease = match.groups()
    suffix = tuple(
        (0, int(part)) if part.isdigit() else (1, part.lower())
        for part in (prerelease or "").split(".")
    )
    return (
        int(major), int(minor), int(patch), int(revision or 0),
        prerelease is None, suffix,
    )


def safe_members(archive):
    seen = set()
    for member in archive.infolist():
        name = member.filename
        path = PurePosixPath(name)
        folded = name.casefold().rstrip("/")
        if (
            "\\" in name
            or ":" in name
            or path.is_absolute()
            or any(part in ("", ".", "..") for part in name.rstrip("/").split("/"))
            or folded in seen
        ):
            raise ValueError("unsafe or duplicate package member: " + name)
        seen.add(folded)
        if stat.S_ISLNK(member.external_attr >> 16):
            raise ValueError("symlink package member is unsupported: " + name)
        yield member


def verify_package(archive, package, version):
    specs = [
        name for name in archive.namelist()
        if "/" not in name and name.lower().endswith(".nuspec")
    ]
    if len(specs) != 1:
        raise ValueError("package must contain exactly one root nuspec")
    root = ET.fromstring(archive.read(specs[0]))
    metadata = {
        element.tag.rsplit("}", 1)[-1]: element.text
        for element in root.iter()
    }
    if (
        (metadata.get("id") or "").lower() != package.lower()
        or (metadata.get("version") or "").lower() != version.lower()
    ):
        raise ValueError(
            "downloaded package identity/version mismatch: "
            + package + "/" + version
        )


class Packages:
    def __init__(self, feed=FEED, token=None):
        self.feed = feed
        self.token = os.environ.get("GITHUB_TOKEN", "") if token is None else token
        if not self.token or any(char in self.token for char in '\r\n"\\'):
            raise ValueError(
                "GITHUB_TOKEN with read:packages permission is required"
            )
        self.resources = None
        self.catalogs = {}
        self.lock = threading.RLock()

    def download(self, url, destination):
        parsed = urlsplit(url)
        origin = urlsplit(self.feed)
        if (parsed.scheme, parsed.netloc) != (origin.scheme, origin.netloc):
            raise ValueError("package URL does not belong to the configured feed")
        loopback = parsed.hostname == "127.0.0.1"
        if parsed.scheme != "https" and not loopback:
            raise ValueError("package feed requires HTTPS")
        destination = Path(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
            partial = Path(temporary) / "download"
            args = [
                "curl", "--disable", "--config", "-", "--fail", "--silent",
                "--show-error", "--location", "--retry", "3",
                "--connect-timeout", "20", "--max-time", "300",
                "--output", str(partial), url,
            ]
            if not loopback:
                args[7:7] = ["--proto", "=https", "--proto-redir", "=https"]
            result = subprocess.run(
                args,
                input='user = "qigao:' + self.token + '"\n',
                text=True,
                capture_output=True,
            )
            if result.returncode == 22:
                raise PackageMissing("package request failed: " + parsed.path)
            if result.returncode:
                raise RuntimeError(
                    "curl package download failed (exit "
                    + str(result.returncode) + ")"
                )
            partial.replace(destination)

    def read_json(self, url):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "metadata.json"
            self.download(url, path)
            return json.loads(path.read_text(encoding="utf-8-sig"))

    @staticmethod
    def resource_matches(resource, kind):
        value = resource.get("@type", "")
        values = [value] if isinstance(value, str) else value
        return any(
            item == kind or item.startswith(kind + "/") for item in values
        )

    def versions(self, package):
        if not IDENTIFIER.fullmatch(package):
            raise ValueError("invalid package ID")
        package = package.lower()
        with self.lock:
            if package in self.catalogs:
                return self.catalogs[package]
            if self.resources is None:
                self.resources = self.read_json(self.feed)["resources"]
            bases = [
                resource["@id"] for resource in self.resources
                if self.resource_matches(resource, "PackageBaseAddress")
            ]
            if bases:
                base = bases[0].rstrip("/") + "/" + package + "/"
                catalog = {
                    version: (
                        base + quote(version.lower()) + "/" + package + "."
                        + quote(version.lower()) + ".nupkg"
                    )
                    for version in self.read_json(base + "index.json")["versions"]
                }
            else:
                registrations = [
                    resource["@id"] for resource in self.resources
                    if self.resource_matches(resource, "RegistrationsBaseUrl")
                ]
                if not registrations:
                    raise ValueError(
                        "feed has no NuGet v3 package download metadata"
                    )
                index = self.read_json(
                    registrations[0].rstrip("/")
                    + "/" + package + "/index.json"
                )
                catalog = {}
                for page in index["items"]:
                    if "items" not in page:
                        page = self.read_json(page["@id"])
                    for leaf in page["items"]:
                        entry = leaf["catalogEntry"]
                        if isinstance(entry, str):
                            entry = self.read_json(entry)
                        if entry.get("listed", True):
                            catalog[entry["version"]] = (
                                leaf.get("packageContent")
                                or entry["packageContent"]
                            )
            self.catalogs[package] = catalog
            return catalog

    def select(self, package, stable=False):
        candidates = [
            version for version in self.versions(package)
            if not stable or "-" not in version
        ]
        if not candidates:
            raise PackageMissing("no matching version for " + package)
        return max(candidates, key=version_key)

    def package(self, package, version, destination):
        catalog = self.versions(package)
        if version not in catalog:
            raise PackageMissing(
                "package version is absent: " + package + "/" + version
            )
        destination = Path(destination)
        self.download(catalog[version], destination)
        with zipfile.ZipFile(destination) as archive:
            list(safe_members(archive))
            verify_package(archive, package, version)
        return destination

    def extract(self, package, version, root):
        root = Path(root)
        target = root / package.lower() / version
        target.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=target.parent) as temporary:
            temporary = Path(temporary)
            archive_path = self.package(
                package, version, temporary / "package.nupkg"
            )
            unpacked = temporary / "unpacked"
            unpacked.mkdir()
            with zipfile.ZipFile(archive_path) as archive:
                members = list(safe_members(archive))
                for member in members:
                    archive.extract(member, unpacked)
                    mode = member.external_attr >> 16
                    if mode and not member.is_dir():
                        (unpacked / member.filename).chmod(mode & 0o777)
            if target.exists():
                shutil.rmtree(target)
            unpacked.rename(target)
        print("Resolved " + package + "/" + version, flush=True)
        return target



class BinaryCache:
    def __init__(self, packages, cache):
        self.packages = packages
        self.cache = Path(cache)
        self.lock = threading.Lock()

    def get(self, name, triplet, abi):
        if (
            not IDENTIFIER.fullmatch(name)
            or not IDENTIFIER.fullmatch(triplet)
            or not re.fullmatch(r"[0-9a-f]{64}", abi)
        ):
            raise ValueError("invalid vcpkg cache identity")
        destination = self.cache / abi[:2] / (abi + ".zip")
        with self.lock:
            if destination.is_file():
                return destination
            package = name + "_" + triplet
            candidates = [
                version
                for version in self.packages.versions(package)
                if version.endswith("-vcpkg" + abi)
            ]
            if len(candidates) != 1:
                raise PackageMissing("no unique binary package for exact ABI")
            with tempfile.TemporaryDirectory() as temporary:
                source = self.packages.package(
                    package, candidates[0], Path(temporary) / "binary.nupkg"
                )
                destination.parent.mkdir(parents=True, exist_ok=True)
                partial = destination.with_suffix(".tmp")
                try:
                    with zipfile.ZipFile(source) as archive, zipfile.ZipFile(
                        partial, "w", zipfile.ZIP_DEFLATED
                    ) as output:
                        members = list(safe_members(archive))
                        evidence = archive.read(
                            "share/" + name + "/vcpkg_abi_info.txt"
                        )
                        if hashlib.sha256(evidence).hexdigest() != abi:
                            raise ValueError(
                                "binary package ABI evidence mismatch"
                            )
                        for member in members:
                            filename = member.filename
                            lower = filename.lower()
                            if (
                                lower == "[content_types].xml"
                                or lower == ".signature.p7s"
                                or (lower.endswith(".nuspec") and "/" not in lower)
                                or lower.startswith(("_rels/", "package/"))
                            ):
                                continue
                            with archive.open(member) as source_file:
                                output.writestr(member, source_file.read())
                    partial.replace(destination)
                finally:
                    partial.unlink(missing_ok=True)
            print(
                "curl restored " + package + " ABI " + abi,
                flush=True,
            )
            return destination


class LoopbackServer(ThreadingHTTPServer):
    def server_bind(self):
        # Numeric loopback avoids reverse-DNS stalls on macOS runners.
        socketserver.TCPServer.server_bind(self)
        self.server_name = "127.0.0.1"
        self.server_port = self.server_address[1]


def cache_server(cache):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.respond(True)

        def do_HEAD(self):
            self.respond(False)

        def respond(self, body):
            parts = [
                unquote(part)
                for part in urlsplit(self.path).path.strip("/").split("/")
            ]
            try:
                if len(parts) != 4:
                    raise ValueError("invalid cache request")
                name, triplet, _version, abi = parts
                path = cache.get(name, triplet, abi)
                self.send_response(200)
                self.send_header("Content-Length", str(path.stat().st_size))
                self.end_headers()
                if body:
                    with path.open("rb") as source:
                        shutil.copyfileobj(source, self.wfile)
            except PackageMissing:
                self.send_error(404, "exact ABI is not cached")
            except Exception as error:
                print(
                    type(error).__name__ + ": binary cache request failed",
                    flush=True,
                )
                self.send_error(502, "binary cache download failed")

        def log_message(self, *_):
            pass

    return LoopbackServer(("127.0.0.1", 0), Handler)


def serve_cache(package_root, ready):
    server = cache_server(BinaryCache(Packages(), package_root))
    ready = Path(ready)
    partial = ready.with_suffix(".tmp")
    partial.write_text(
        json.dumps({"port": server.server_port, "pid": os.getpid()}),
        encoding="utf-8",
    )
    partial.replace(ready)
    server.serve_forever()

def host_rid():
    runner_os = os.environ.get("RUNNER_OS")
    runner_arch = os.environ.get("RUNNER_ARCH")
    if runner_os and runner_arch:
        os_name = {
            "Linux": "linux", "macOS": "macos", "Windows": "windows"
        }[runner_os]
        arch = {"X64": "x64", "ARM64": "arm64"}[runner_arch]
        return os_name + "-" + arch
    os_name = {
        "Linux": "linux", "Darwin": "macos", "Windows": "windows"
    }[platform.system()]
    machine = platform.machine().lower()
    arch = "arm64" if machine in ("arm64", "aarch64") else "x64"
    return os_name + "-" + arch


def export(values, path=None):
    for value in values.values():
        if "\n" in str(value) or "\r" in str(value):
            raise ValueError("invalid environment value")
    github_env = os.environ.get("GITHUB_ENV")
    if github_env:
        with open(github_env, "a", encoding="utf-8") as output:
            for key, value in values.items():
                output.write(key + "=" + str(value) + "\n")
    if path and os.environ.get("GITHUB_PATH"):
        with open(os.environ["GITHUB_PATH"], "a", encoding="utf-8") as output:
            output.write(str(path) + "\n")
    for key, value in values.items():
        print(key + "=" + str(value), flush=True)

def restore(rid, package_root):
    feed = Packages()
    dependencies = [
        ("SALTS_ROOT", "SALTS_SDK_RELEASE", "Salts.Native", "Salts", False),
        (
            "SALTS_UTILS_ROOT", "SALTS_UTILS_SDK_RELEASE",
            "SaltsUtils.Native", "SaltsUtils", False,
        ),
        ("CHTTP_ROOT", "CHTTP_SDK_RELEASE", "CHttp.Native", "Chttp", False),
    ]

    values = {}
    for root_var, version_var, package, cmake_name, stable in dependencies:
        version = feed.select(package, stable=stable)
        root = feed.extract(package, version, package_root) / "sdk" / rid
        config = (
            root / "lib" / "cmake" / cmake_name
            / (cmake_name + "Config.cmake")
        )
        if not config.is_file():
            raise ValueError(
                package + " does not provide SDK for " + rid + ": " + str(config)
            )
        values[root_var] = root.resolve()
        values[version_var] = version

    re2c_version = feed.select("Qigao.Re2c.Binary", stable=True)
    re2c_root = (
        feed.extract("Qigao.Re2c.Binary", re2c_version, package_root)
        / "tools" / host_rid()
    )
    executable = re2c_root / "bin" / ("re2c.exe" if os.name == "nt" else "re2c")
    if not executable.is_file():
        raise ValueError("Qigao.Re2c.Binary is incomplete: " + str(executable))
    if os.name != "nt":
        executable.chmod(executable.stat().st_mode | 0o111)
    subprocess.run([str(executable), "--version"], check=True)

    values["RE2C_ROOT"] = re2c_root.resolve()
    values["RE2C_PACKAGE_VERSION"] = re2c_version

    # Fail closed: publish roots only after every selected RID payload validates.
    export(values, re2c_root.resolve() / "bin")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    command = subparsers.add_parser("restore")
    command.add_argument(
        "--rid",
        required=True,
        choices=(
            "windows-x64", "linux-x64",
            "macos-arm64", "android-arm64-v8a",
        ),
    )
    command.add_argument("--packages", type=Path, required=True)
    serve = subparsers.add_parser("serve-cache")
    serve.add_argument("--packages", type=Path, required=True)
    serve.add_argument("--ready", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "restore":
        restore(args.rid, args.packages)
    else:
        serve_cache(args.packages, args.ready)


if __name__ == "__main__":
    main()
