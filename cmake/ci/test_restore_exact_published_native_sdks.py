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
            salts_version="2.3.0-rc.15",
            utils_version="4.3.0-rc.10",
            salts_sha="a" * 40, utils_sha="b" * 40,
            salts_digest="c" * 64, utils_digest="d" * 64,
        )
        fields.update(changes)
        return argparse.Namespace(**fields)

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

    def test_nuget_graph_selection_is_exact_and_unique(self):
        with tempfile.TemporaryDirectory() as temp:
            packages = Path(temp)
            (packages / "salts.native/2.3.0-rc.15").mkdir(parents=True)
            assets = {"libraries": {
                "Salts.Native/2.3.0-rc.15": {"path": "salts.native/2.3.0-rc.15"}}}
            selected = module["select_package"](
                assets, "Salts.Native", "2.3.0-rc.15", packages)
            self.assertEqual(selected, packages / "salts.native/2.3.0-rc.15")
            with self.assertRaisesRegex(ValueError, "not exactly"):
                module["select_package"](
                    assets, "Salts.Native", "2.3.0-rc.16", packages)
            extra = {"libraries": dict(assets["libraries"],
                                      **{"Salts.Native/2.3.0-rc.14": {
                                          "path": "salts.native/2.3.0-rc.14"}})}
            with self.assertRaisesRegex(ValueError, "not exactly"):
                module["select_package"](
                    extra, "Salts.Native", "2.3.0-rc.15", packages)

    @staticmethod
    def synthetic_archive(
        package: Path, identity: str, version: str, commit: str,
        salts_version: str, bad_abi=False
    ):
        rid = "linux-x64"
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
                z.writestr(prefix + "include/salts/thread.h",
                           "old custom once" if bad_abi else "pthread_once_t native;")
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
            version = "2.3.0-rc.15"
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

    def test_utils_package_must_record_exact_salts_dependency(self):
        with tempfile.TemporaryDirectory() as temp:
            pkg = Path(temp)
            version = "4.3.0-rc.10"
            archive = self.synthetic_archive(
                pkg, "SaltsUtils.Native", version, "b" * 40, "2.3.0-rc.15")
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            module["verify_package"](
                pkg, "SaltsUtils.Native", version, "b" * 40,
                digest, "2.3.0-rc.15")
            with self.assertRaisesRegex(ValueError, "salts_version"):
                module["verify_package"](
                    pkg, "SaltsUtils.Native", version, "b" * 40,
                    digest, "2.3.0-rc.14")

    def test_published_tags_must_match_real_commit_and_asset(self):
        def fake_gh(*argv):
            if "/releases/tags/" in argv[-1]:
                return json.dumps({
                    "tag_name": "v2.3.0-rc.15", "draft": False,
                    "prerelease": True,
                    "assets": [{"name": "Salts.Native.2.3.0-rc.15.nupkg"}],
                })
            return json.dumps({"sha": "a" * 40})
        with mock.patch.dict(module["verify_release_metadata"].__globals__, {"run": fake_gh}):
            module["verify_release_metadata"](
                "Salts.Native", "qigao/salts", "2.3.0-rc.15", "a" * 40)
            with self.assertRaisesRegex(ValueError, "mismatch"):
                module["verify_release_metadata"](
                    "Salts.Native", "qigao/salts", "2.3.0-rc.15", "b" * 40)


if __name__ == "__main__":
    unittest.main()
