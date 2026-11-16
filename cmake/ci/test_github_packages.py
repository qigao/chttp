#!/usr/bin/env python3
"""Offline admission tests for the curl-only GitHub Packages restore path."""
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import unittest
from unittest.mock import patch
import zipfile

import github_packages as packages


class Feed:
    def __init__(self):
        self.documents = {}
        feed = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                expected = "Basic " + base64.b64encode(b"qigao:test-token").decode()
                if self.headers.get("Authorization") != expected:
                    self.send_error(401)
                    return
                if self.path not in feed.documents:
                    self.send_error(404)
                    return
                value = feed.documents[self.path]
                if not isinstance(value, bytes):
                    value = json.dumps(value).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(value)))
                self.end_headers()
                self.wfile.write(value)

            def log_message(self, *_):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.base = "http://127.0.0.1:" + str(self.server.server_port)
        self.documents["/index.json"] = {
            "resources": [
                {
                    "@id": self.base + "/flat/",
                    "@type": "PackageBaseAddress/3.0.0",
                }
            ]
        }
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()

    def add(self, package, version, files, spec_id=None):
        identity = package.lower()
        index = self.documents.setdefault(
            "/flat/" + identity + "/index.json", {"versions": []}
        )
        index["versions"].append(version)
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w") as archive:
            archive.writestr(
                identity + ".nuspec",
                '<package xmlns="urn:nuget"><metadata><id>'
                + (spec_id or package)
                + "</id><version>" + version
                + "</version></metadata></package>",
            )
            archive.writestr("[Content_Types].xml", "metadata")
            for name, value in files.items():
                archive.writestr(name, value)
        path = (
            "/flat/" + identity + "/" + version.lower() + "/"
            + identity + "." + version.lower() + ".nupkg"
        )
        self.documents[path] = output.getvalue()


class PackagesTests(unittest.TestCase):
    def setUp(self):
        self.feed = Feed()
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.packages = packages.Packages(
            self.feed.base + "/index.json", token="test-token"
        )

    def tearDown(self):
        self.feed.close()
        self.temp.cleanup()

    def test_version_selection_matches_restore_policy(self):
        for version in (
            "2.3.0-rc.16", "2.3.0-rc.17", "2.2.9", "2.3.0", "2.4.0-rc.1"
        ):
            self.feed.add(
                "Salts.Native", version, {"sdk/linux-x64/file": "x"}
            )
        self.assertEqual(
            self.packages.select("Salts.Native"), "2.4.0-rc.1"
        )
        self.assertEqual(
            self.packages.select("Salts.Native", stable=True), "2.3.0"
        )

    def test_identity_and_archive_path_are_validated(self):
        self.feed.add(
            "Good.Native", "1.0.0", {"sdk/linux-x64/config": "okay"}
        )
        root = self.packages.extract("Good.Native", "1.0.0", self.root)
        self.assertEqual(
            (root / "sdk/linux-x64/config").read_text(), "okay"
        )
        self.feed.add("Bad.Native", "1.0.0", {"../escape": "bad"})
        with self.assertRaisesRegex(ValueError, "unsafe"):
            self.packages.extract("Bad.Native", "1.0.0", self.root)
        self.assertFalse((self.root / "escape").exists())

    def test_binary_cache_requires_exact_abi_evidence(self):
        evidence = b"compiler exact\\nport exact\\n"
        abi = hashlib.sha256(evidence).hexdigest()
        self.feed.add(
            "test_x64-linux",
            "1.0.0-vcpkg" + abi,
            {
                "share/test/vcpkg_abi_info.txt": evidence,
                "lib/libtest.a": b"archive",
                "_rels/.rels": "NuGet metadata",
            },
        )
        cache = packages.BinaryCache(self.packages, self.root / "cache")
        result = cache.get("test", "x64-linux", abi)
        with zipfile.ZipFile(result) as archive:
            self.assertEqual(archive.read("lib/libtest.a"), b"archive")
            self.assertFalse(
                any(
                    name.startswith(("_rels/", "package/"))
                    or name.endswith(".nuspec")
                    for name in archive.namelist()
                )
            )
        with self.assertRaises(packages.PackageMissing):
            cache.get("test", "x64-linux", "0" * 64)

    @patch.dict(os.environ, {"GITHUB_TOKEN": "secret-token"}, clear=False)
    def test_token_is_passed_on_stdin_not_argv(self):
        destination = self.root / "response"
        with patch.object(packages.subprocess, "run") as run:
            def fake(args, **kwargs):
                self.assertNotIn("secret-token", " ".join(args))
                self.assertIn(
                    'user = "qigao:secret-token"', kwargs["input"]
                )
                Path(args[args.index("--output") + 1]).write_text("{}")

                class Result:
                    returncode = 0
                return Result()

            run.side_effect = fake
            client = packages.Packages(
                "https://nuget.pkg.github.com/qigao/index.json"
            )
            client.download(
                "https://nuget.pkg.github.com/qigao/index.json",
                destination,
            )
        self.assertEqual(destination.read_text(), "{}")

    def test_export_writes_real_github_environment_lines(self):
        environment = self.root / "env"
        with patch.dict(
            os.environ, {"GITHUB_ENV": str(environment)}, clear=False
        ):
            packages.export({"FIRST": "one", "SECOND": "two"})
        self.assertEqual(
            environment.read_text().splitlines(),
            ["FIRST=one", "SECOND=two"],
        )
        self.assertNotIn("\\n", environment.read_text())

    def test_environment_is_atomic_on_missing_rid(self):
        dependencies = (
            ("Salts.Native", "Salts", "2.3.0-rc.17"),
            ("SaltsUtils.Native", "SaltsUtils", "4.3.0-rc.11"),
            ("CHttp.Native", "Chttp", "2.1.0-rc.6"),
        )
        for package, cmake, version in dependencies:
            self.feed.add(
                package,
                version,
                {
                    "sdk/linux-x64/lib/cmake/" + cmake + "/"
                    + cmake + "Config.cmake": ""
                },
            )
        self.feed.add(
            "Qigao.Re2c.Binary",
            "4.3.1",
            {"tools/linux-x64/bin/re2c": "#!/bin/sh\nexit 0\n"},
        )
        environment = self.root / "env"
        with patch.object(
            packages, "Packages", return_value=self.packages
        ), patch.dict(
            os.environ,
            {
                "GITHUB_ENV": str(environment),
                "RUNNER_OS": "Linux",
                "RUNNER_ARCH": "X64",
            },
            clear=False,
        ):
            with self.assertRaisesRegex(ValueError, "does not provide SDK"):
                packages.restore(
                    "android-arm64-v8a", self.root / "restore"
                )
        self.assertFalse(environment.exists())

    def test_restore_does_not_require_disabled_database_sdk(self):
        versions = {
            "Salts.Native": "2.3.0",
            "SaltsUtils.Native": "4.3.0",
            "CHttp.Native": "2.1.0",
            "Qigao.Re2c.Binary": "4.6.7",
        }
        roots = {name: self.root / name for name in versions}
        for name, cmake in (
            ("Salts.Native", "Salts"),
            ("SaltsUtils.Native", "SaltsUtils"),
            ("CHttp.Native", "Chttp"),
        ):
            config = roots[name] / "sdk/linux-x64/lib/cmake" / cmake / (cmake + "Config.cmake")
            config.parent.mkdir(parents=True)
            config.write_text("", encoding="utf-8")
        host = "windows-x64" if os.name == "nt" else "linux-x64"
        executable = roots["Qigao.Re2c.Binary"] / "tools" / host / "bin" / (
            "re2c.exe" if os.name == "nt" else "re2c"
        )
        executable.parent.mkdir(parents=True)
        executable.write_text("", encoding="utf-8")
        environment = self.root / "restored-env"
        with patch.object(packages, "Packages") as constructor, patch.object(
            packages, "host_rid", return_value=host
        ), patch.object(packages.subprocess, "run"), patch.dict(
            os.environ, {"GITHUB_ENV": str(environment)}, clear=False
        ):
            feed = constructor.return_value
            feed.select.side_effect = lambda name, stable=False: versions[name]
            feed.extract.side_effect = lambda name, version, destination: roots[name]
            packages.restore("linux-x64", self.root / "restore")
        self.assertIn("SALTS_ROOT=", environment.read_text())
        self.assertNotIn("TURBODB", environment.read_text())


if __name__ == "__main__":
    unittest.main()
