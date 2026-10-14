"""Offline negative/positive tests for manual exact-published SDK cutover gates."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest import mock
import zipfile

module = runpy.run_path(
    str(Path(__file__).with_name("restore-exact-published-native-sdks.py")),
    run_name="exact_native_restore_unit"
)


class ExactPublishedNativeRestoreTest(unittest.TestCase):
    def inputs(self, **changes):
        fields = dict(
            salts_version="2.3.0-rc.16",
            utils_version="4.3.0-rc.11",
            salts_sha="a" * 40, utils_sha="b" * 40,
            salts_digest="c" * 64, utils_digest="d" * 64,
        )
        fields.update(changes)
        return argparse.Namespace(**fields)

    def test_nuget_package_reference_uses_exact_bracket_range(self):
        from xml.etree import ElementTree
        project = ElementTree.fromstring(module["exact_nuget_project"](
            "2.3.0-rc.16", "4.3.0-rc.11"))
        refs = {item.attrib["Include"]: item.attrib["Version"]
                for item in project.iter("PackageReference")}
        self.assertEqual(refs, {
            "Salts.Native": "[2.3.0-rc.16]",
            "SaltsUtils.Native": "[4.3.0-rc.11]",
        })
        self.assertEqual(len(refs), 2)
        self.assertNotIn("2.3.0-*", ElementTree.tostring(project).decode())

    def test_exact_versions_source_shas_and_digests_required(self):
        with mock.patch.dict(os.environ, {
            "SALTS_CANDIDATE_ROOT": "", "SALTS_UTILS_CANDIDATE_ROOT": ""
        }):
            module["validate_inputs"](self.inputs())
            module["validate_inputs"](
                self.inputs(salts_version="2.3.0", utils_version="4.3.0"))
            for overrides in (
                {"salts_version": "2.3.0-*"},
                {"utils_version": "4.3.0-*"},
                {"salts_version": "2.3.0-rc.0"},
                {"utils_version": "4.3.0-rc.00"},
                {"salts_version": "2.3.0-rc.14"},
                {"utils_version": "4.3.0-rc.9"},
                {"salts_version": "3.0.0"},
                {"salts_sha": "x" * 40},
                {"utils_sha": "B" * 40},
                {"salts_digest": "?" * 64},
                {"utils_digest": ""},
            ):
                with self.subTest(overrides=overrides):
                    with self.assertRaises(ValueError):
                        module["validate_inputs"](self.inputs(**overrides))

    def test_never_mix_draft_candidate_and_published_versions(self):
        with mock.patch.dict(os.environ, {"SALTS_CANDIDATE_ROOT": "/tmp/old"}):
            with self.assertRaisesRegex(ValueError, "Draft candidate"):
                module["validate_inputs"](self.inputs())
        with mock.patch.dict(os.environ, {"SALTS_UTILS_CANDIDATE_ROOT": "/tmp/old"}):
            with self.assertRaisesRegex(ValueError, "Draft candidate"):
                module["validate_inputs"](self.inputs())

    def test_cross_platform_target_host_rid_pairs_fail_closed(self):
        valid = module["HOST_RIDS"]
        self.assertEqual(len(valid), 6)
        self.assertEqual(valid["android-arm64-v8a"], "linux-x64")
        self.assertEqual(valid["ios-arm64"], "macos-arm64")
        self.assertEqual(set(valid), set(module["RIDS"]))
        for rid, host in valid.items():
            with self.subTest(rid=rid, host=host):
                module["validate_rid_pair"](rid, host)
                for other in module["RIDS"]:
                    if other != host:
                        with self.assertRaisesRegex(ValueError, "target/host"):
                            module["validate_rid_pair"](rid, other)
        with self.assertRaisesRegex(ValueError, "target/host"):
            module["validate_rid_pair"]("linux-amd64", "linux-x64")

    def test_each_native_rid_requires_expected_once_abi(self):
        for rid in module["RIDS"]:
            with self.subTest(rid=rid), tempfile.TemporaryDirectory() as tmp:
                pkg = Path(tmp)
                version, sha = "2.3.0-rc.16", "a" * 40
                archive = self.synthetic_archive(pkg, "Salts.Native",
                                                 version, sha, version, rid=rid)
                digest = hashlib.sha256(archive.read_bytes()).hexdigest()
                root, got = module["verify_package"](
                    pkg, "Salts.Native", version, sha, digest, version, rid)
                self.assertEqual(root, pkg / "sdk" / rid)
                self.assertEqual(got, digest)

    def test_windows_once_abi_rejects_old_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            pkg = Path(tmp)
            version, sha = "2.3.0-rc.16", "a" * 40
            archive = self.synthetic_archive(
                pkg, "Salts.Native", version, sha, version,
                bad_abi=True, rid="windows-x64")
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            with self.assertRaisesRegex(ValueError, "Windows Salts"):
                module["verify_package"](
                    pkg, "Salts.Native", version, sha, digest, version,
                    "windows-x64")

    def test_nuget_graph_selection_is_exact_and_unique(self):
        with tempfile.TemporaryDirectory() as temp:
            packages = Path(temp)
            (packages / "salts.native/2.3.0-rc.16").mkdir(parents=True)
            assets = {"libraries": {
                "Salts.Native/2.3.0-rc.16": {"path": "salts.native/2.3.0-rc.16"}}}
            selected = module["select_package"](
                assets, "Salts.Native", "2.3.0-rc.16", packages)
            self.assertEqual(selected, packages / "salts.native/2.3.0-rc.16")
            with self.assertRaisesRegex(ValueError, "not exactly"):
                module["select_package"](
                    assets, "Salts.Native", "2.3.0-rc.15", packages)
            extra = {"libraries": dict(assets["libraries"],
                                      **{"Salts.Native/2.3.0-rc.14": {
                                          "path": "salts.native/2.3.0-rc.14"}})}
            with self.assertRaisesRegex(ValueError, "not exactly"):
                module["select_package"](
                    extra, "Salts.Native", "2.3.0-rc.16", packages)

    @staticmethod
    def synthetic_archive(
        package: Path, identity: str, version: str, commit: str,
        salts_version: str, bad_abi=False, rid="linux-x64"
    ):
        is_salts = identity == "Salts.Native"
        manifest = ("salts-sdk-manifest.txt" if is_salts
                    else "salts-utils-sdk-manifest.txt")
        facts = (f"version={version}\ncommit={commit}\nprofile=release\n"
                 + ("" if is_salts else f"salts_version={salts_version}\n"))
        prefix = f"sdk/{rid}/"
        nupkg = package / (identity + "." + version + ".nupkg")
        with zipfile.ZipFile(nupkg, "w") as z:
            z.writestr(prefix + manifest, facts)
            z.writestr(
                prefix + ("lib/cmake/Salts/SaltsConfig.cmake" if is_salts
                          else "lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake"),
                "set(dummy ok)\n")
            if is_salts:
                native_header = ("volatile int state;\nSALTS_ONCE_INIT {0}"
                                 if rid == "windows-x64" else "pthread_once_t native;")
                z.writestr(prefix + "include/salts/thread.h",
                           "old custom once" if bad_abi else native_header)
            z.writestr(identity + ".nuspec",
                '<package><metadata><id>' + identity + '</id><version>'
                + version + '</version></metadata></package>')
        root = package / "sdk" / rid / "lib" / "cmake" / (
            "Salts" if is_salts else "SaltsUtils")
        root.mkdir(parents=True)
        (root / (root.name + "Config.cmake")).write_text("set(dummy ok)\n")
        return nupkg

    def test_real_zip_nuspec_manifest_source_abi_and_digest(self):
        with tempfile.TemporaryDirectory() as temp:
            pkg = Path(temp)
            version = "2.3.0-rc.16"
            src = "a" * 40
            archive = self.synthetic_archive(pkg, "Salts.Native", version,
                                             src, version)
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            root, got = module["verify_package"](
                pkg, "Salts.Native", version, src, digest, version)
            self.assertEqual(root, pkg / "sdk/linux-x64")
            self.assertEqual(got, digest)
            with self.assertRaisesRegex(ValueError, "digest mismatch"):
                module["verify_package"](
                    pkg, "Salts.Native", version, src, "0" * 64, version)
            with self.assertRaisesRegex(ValueError, "commit"):
                module["verify_package"](
                    pkg, "Salts.Native", version, "b" * 40, digest, version)

    def test_packaged_salts_rejects_old_native_once_abi(self):
        with tempfile.TemporaryDirectory() as temp:
            pkg = Path(temp)
            version, sha = "2.3.0-rc.16", "a" * 40
            archive = self.synthetic_archive(
                pkg, "Salts.Native", version, sha, version, bad_abi=True)
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            with self.assertRaisesRegex(ValueError, "pthread_once ABI"):
                module["verify_package"](
                    pkg, "Salts.Native", version, sha, digest, version)

    def test_utils_package_must_record_exact_salts_dependency(self):
        with tempfile.TemporaryDirectory() as temp:
            pkg = Path(temp)
            version = "4.3.0-rc.11"
            archive = self.synthetic_archive(
                pkg, "SaltsUtils.Native", version, "b" * 40, "2.3.0-rc.16")
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            module["verify_package"](
                pkg, "SaltsUtils.Native", version, "b" * 40,
                digest, "2.3.0-rc.16")
            with self.assertRaisesRegex(ValueError, "salts_version"):
                module["verify_package"](
                    pkg, "SaltsUtils.Native", version, "b" * 40,
                    digest, "2.3.0-rc.14")

    def test_published_tags_must_match_real_commit_and_asset(self):
        digest = "d" * 64
        release = {
            "tag_name": "v2.3.0-rc.16", "draft": False,
            "prerelease": True,
            "assets": [{
                "name": "Salts.Native.2.3.0-rc.16.nupkg",
                "digest": "sha256:" + digest, "state": "uploaded", "size": 1058,
            }],
        }
        def fake_gh(*argv):
            if "/releases/tags/" in argv[-1]:
                return json.dumps(release)
            return json.dumps({"sha": "a" * 40})
        with mock.patch.dict(module["verify_release_metadata"].__globals__, {"run": fake_gh}):
            proof = module["verify_release_metadata"](
                "Salts.Native", "qigao/salts", "2.3.0-rc.16",
                "a" * 40, digest)
            self.assertEqual(proof["asset_sha256"], digest)
            with self.assertRaisesRegex(ValueError, "mismatch"):
                module["verify_release_metadata"](
                    "Salts.Native", "qigao/salts", "2.3.0-rc.16",
                    "b" * 40, digest)
            with self.assertRaisesRegex(ValueError, "digest/state mismatch"):
                module["verify_release_metadata"](
                    "Salts.Native", "qigao/salts", "2.3.0-rc.16",
                    "a" * 40, "0" * 64)
            # Release asset integrity, not just NuGet identity, is a hard gate.
            for overrides in (
                {"digest": None},
                {"digest": "sha256:" + "0" * 64},
                {"state": "new"},
                {"size": 0},
                {"size": None},
            ):
                with self.subTest(overrides=overrides):
                    release["assets"][0].update(overrides)
                    with self.assertRaisesRegex(ValueError, "digest/state mismatch"):
                        module["verify_release_metadata"](
                            "Salts.Native", "qigao/salts", "2.3.0-rc.16",
                            "a" * 40, digest)
                    release["assets"][0] = {
                        "name": "Salts.Native.2.3.0-rc.16.nupkg",
                        "digest": "sha256:" + digest, "state": "uploaded",
                        "size": 1058,
                    }
            release["assets"].append(dict(release["assets"][0]))
            with self.assertRaisesRegex(ValueError, "mismatch"):
                module["verify_release_metadata"](
                    "Salts.Native", "qigao/salts", "2.3.0-rc.16",
                    "a" * 40, digest)
            release["assets"].pop()
            release["prerelease"] = False
            with self.assertRaisesRegex(ValueError, "release channel"):
                module["verify_release_metadata"](
                    "Salts.Native", "qigao/salts", "2.3.0-rc.16",
                    "a" * 40, digest)


# This is a second genuine (synthetic) CMake SDK graph: it proves the
# installed-consumer attestor checks all RIDs, provenance, IDL host tool and
# removal of the independent RpcService public target before release.
attest = runpy.run_path(
    str(Path(__file__).with_name("attest-exact-published-consumer.py")),
    run_name="exact_published_consumer_unit",
)


class ExactPublishedInstalledConsumerAttestationTest(unittest.TestCase):
    SALTS_SHA = "a" * 40
    UTILS_SHA = "b" * 40
    CHTTP_SHA = "c" * 40

    def fixture(self, base: Path, rid: str, family: str):
        repo = base / "repo"
        build = repo / ("build/android-native-sdk" if family == "android"
                        else "build/" + rid if family == "ios"
                        else "build/native-sdk")
        root = repo / "stage/sdk" / rid / "lib/cmake/Chttp"
        root.mkdir(parents=True)
        (root / "ChttpConfig.cmake").write_text("set(CHttp_FOUND TRUE)\n")
        (root / "ChttpTargets.cmake").write_text("add_library(CHttp::Server INTERFACE)\n")
        salts = base / "salts"
        utils = base / "utils"
        (salts / "lib/cmake/Salts").mkdir(parents=True)
        (utils / "lib/cmake/SaltsUtils").mkdir(parents=True)
        (utils / "bin").mkdir()
        tool = utils / "bin" / "salts-idlc"
        tool.write_text("tool")
        (salts / "salts-sdk-manifest.txt").write_text(
            f"version=2.3.0-rc.16\ncommit={self.SALTS_SHA}\nprofile=release\n"
        )
        (utils / "salts-utils-sdk-manifest.txt").write_text(
            f"version=4.3.0-rc.11\ncommit={self.UTILS_SHA}\n"
            "profile=release\nsalts_version=2.3.0-rc.16\n"
        )
        build.mkdir(parents=True)
        (build / "CMakeCache.txt").write_text(
            f"Salts_DIR:PATH={salts / 'lib/cmake/Salts'}\n"
            f"SaltsUtils_DIR:PATH={utils / 'lib/cmake/SaltsUtils'}\n"
            f"SaltsUtils_IDLC_EXECUTABLE:FILEPATH={tool}\n"
        )
        env = {
            "GITHUB_WORKSPACE": str(repo),
            "EXPECTED_CHTTP": self.CHTTP_SHA,
            "EXPECTED_SALTS": self.SALTS_SHA,
            "EXPECTED_UTILS": self.UTILS_SHA,
            "EXPECTED_SALTS_VERSION": "2.3.0-rc.16",
            "EXPECTED_UTILS_VERSION": "4.3.0-rc.11",
            "EXPECTED_SALTS_DIGEST": "d" * 64,
            "EXPECTED_UTILS_DIGEST": "e" * 64,
            "SALTS_ROOT": str(salts),
            "SALTS_UTILS_ROOT": str(utils),
            "EXACT_SALTS_IDLC": str(tool),
        }
        return repo, env, root, utils

    def invoke(self, rid: str, family: str, env: dict):
        with mock.patch.dict(os.environ, env, clear=True), \
             mock.patch("sys.argv", ["attest", "--rid", rid, "--family", family]), \
             mock.patch("subprocess.check_output", return_value=self.CHTTP_SHA + "\n"):
            attest["main"]()

    def test_all_six_rids_attest_actual_installed_cmake_and_sdk_graph(self):
        for rid, family in (
            ("linux-x64", "linux"), ("linux-arm64", "linux"),
            ("windows-x64", "windows"), ("macos-arm64", "macos"),
            ("android-arm64-v8a", "android"), ("ios-arm64", "ios"),
        ):
            with self.subTest(rid=rid), tempfile.TemporaryDirectory() as tmp:
                repo, env, targets, utils = self.fixture(Path(tmp), rid, family)
                self.invoke(rid, family, env)
                proof = json.loads((repo / "build/published-cutover"
                                    / f"provenance-{rid}.json").read_text())
                self.assertEqual(proof["salts_source"], self.SALTS_SHA)
                self.assertEqual(proof["utils_source"], self.UTILS_SHA)
                self.assertTrue(proof["published_release_files_only"])
                self.assertTrue(proof["no_silent_fallback"])
                self.assertTrue((targets.parents[2] /
                                 "salts-dependency-manifest.txt").is_file())

    def test_reject_mixed_upstream_saltsutils_manifest_and_old_rpc_target(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo, env, target, utils = self.fixture(Path(tmp), "linux-x64", "linux")
            manifest = utils / "salts-utils-sdk-manifest.txt"
            manifest.write_text(manifest.read_text().replace(
                "salts_version=2.3.0-rc.16", "salts_version=2.3.0-rc.14"))
            with self.assertRaisesRegex(SystemExit, "not built against exact"):
                self.invoke("linux-x64", "linux", env)
            manifest.write_text(manifest.read_text().replace(
                "salts_version=2.3.0-rc.14", "salts_version=2.3.0-rc.16"))
            (target / "ChttpTargets.cmake").write_text("CHttp::RpcService")
            with self.assertRaisesRegex(SystemExit, "independent RpcService"):
                self.invoke("linux-x64", "linux", env)


if __name__ == "__main__":
    unittest.main()
