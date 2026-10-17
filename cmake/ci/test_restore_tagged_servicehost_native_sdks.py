#!/usr/bin/env python3
"""Offline fail-closed selection test; does not fetch a tag or NuGet binary."""
import importlib.util
from pathlib import Path
from unittest import TestCase, main
from unittest.mock import patch

file = Path(__file__).with_name("restore-tagged-servicehost-native-sdks.py")
spec = importlib.util.spec_from_file_location("tagged_restore", file)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


class TagOnlyAdmission(TestCase):
    def test_accepted_future_explicit_tags(self):
        self.assertEqual(mod.tag_version("v2.3.0-rc.18", "Salts"), "2.3.0-rc.18")
        self.assertEqual(mod.tag_version("v4.3.0-rc.12", "SaltsUtils"), "4.3.0-rc.12")
        self.assertEqual(mod.tag_version("v2.3.0", "Salts"), "2.3.0")

    def test_reject_legacy_and_floating(self):
        for tag, family in (
            ("v2.3.0-rc.17", "Salts"),
            ("v2.3.0-rc.0", "Salts"),
            ("2.3.0-rc.18", "Salts"),
            ("v2.3.0-rc.*", "Salts"),
            ("master", "Salts"),
            ("deadbeef" * 5, "Salts"),
            ("v4.3.0-rc.11", "SaltsUtils"),
            ("v4.3.0-rc.012", "SaltsUtils"),
        ):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                mod.tag_version(tag, family)

    def test_missing_sdk_asset_must_fail(self):
        release = {
            "tag_name": "v2.3.0-rc.18", "draft": False, "prerelease": True,
            "assets": [],
        }
        with patch.object(mod, "gh_json", side_effect=[
            release, {"sha": "a" * 40},
        ]):
            with self.assertRaises(ValueError):
                mod.release_identity(
                    "qigao/salts", "Salts.Native",
                    "v2.3.0-rc.18", "2.3.0-rc.18")

    def test_exact_digest_and_release_identity(self):
        release = {
            "tag_name": "v2.3.0-rc.18", "draft": False, "prerelease": True,
            "assets": [{
                "name": "Salts.Native.2.3.0-rc.18.nupkg",
                "digest": "sha256:" + "b" * 64,
                "state": "uploaded", "size": 34,
            }],
        }
        with patch.object(mod, "gh_json", side_effect=[
            release, {"sha": "a" * 40},
        ]):
            self.assertEqual(mod.release_identity(
                "qigao/salts", "Salts.Native",
                "v2.3.0-rc.18", "2.3.0-rc.18"),
                ("a" * 40, "b" * 64))


if __name__ == "__main__":
    main()
