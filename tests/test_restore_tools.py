"""Synthetic archive checks for the offline restoration tools, not game tests."""

from __future__ import annotations

import hashlib
import json
import stat
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

import bundle_lib
from bundle_lib import BundleError, load_bundle, restore_project, sha256_file


class RestoreToolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        self.assertEqual(self.root.parent, Path(tempfile.gettempdir()).resolve())
        self.bundle_root = self.root / "bundle"
        self.bundle_root.mkdir()
        self.archive_path = self.root / "Project.zip"
        self.output = self.root / "RestoredNpc"
        self.originals = {
            "NPC.uproject": b'{"EngineAssociation":"5.4"}\n',
            "Config/DefaultGame.ini": b"[Project]\nInitial=True\n",
            "Content/Characters/Example.uasset": b"synthetic blueprint bytes\n",
            "Content/\uce90\ub9ad\ud130.uasset": b"Unicode filename fixture\n",
            "Source/NPC/NPC.cpp": b"// synthetic module\n",
        }
        self.changes = {
            "Config/DefaultGame.ini": b"[Project]\nInitial=True\nAudit=True\n",
            "Plugins/NpcAudit/NpcAudit.uplugin": b'{"FileVersion":3}\n',
            "Plugins/NpcAudit/Source/NpcAudit.cpp": b"// synthetic audit module\n",
        }

    def tearDown(self) -> None:
        # Only this test's verified temporary directory is recursively cleaned.
        self.assertEqual(self.root.parent, Path(tempfile.gettempdir()).resolve())
        self.temporary.cleanup()

    @staticmethod
    def record(name: str, data: bytes) -> dict:
        return {
            "path": name,
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        }

    def write_json(self, relative: str, document: dict) -> None:
        path = self.bundle_root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )

    def repin(self) -> None:
        descriptor = json.loads(
            (self.bundle_root / "bundle.json").read_text(encoding="utf-8")
        )
        descriptor["files"] = [
            self.record(
                path.relative_to(self.bundle_root).as_posix(), path.read_bytes()
            )
            for path in sorted(self.bundle_root.rglob("*"))
            if path.is_file() and path.name != "bundle.json"
        ]
        self.write_json("bundle.json", descriptor)

    def fixture(
        self, extra_entries: list[tuple[str | zipfile.ZipInfo, bytes]] | None = None
    ):
        with zipfile.ZipFile(
            self.archive_path, "w", compression=zipfile.ZIP_DEFLATED
        ) as archive:
            for name, data in self.originals.items():
                archive.writestr(name, data)
            archive.writestr(
                "NPC.sln", b"generated IDE solution excluded from source restore\n"
            )
            archive.writestr("Saved/Excluded.log", b"not part of source\n")
            for name, data in extra_entries or []:
                archive.writestr(name, data)
        original_rows = [
            self.record(name, data) for name, data in self.originals.items()
        ]
        final_files = self.originals | self.changes
        self.write_json(
            "manifests/original-project.json",
            {
                "schema_version": 1,
                "source_version": "1",
                "files": original_rows,
            },
        )
        self.write_json(
            "manifests/final-project.json",
            {
                "schema_version": 1,
                "source_version": "1",
                "files": [
                    self.record(name, data) for name, data in final_files.items()
                ],
            },
        )
        overlay_rows = []
        for name, data in self.changes.items():
            payload = self.bundle_root / "overlay" / name
            payload.parent.mkdir(parents=True, exist_ok=True)
            payload.write_bytes(data)
            overlay_rows.append(
                {
                    **self.record(name, data),
                    "payload": f"overlay/{name}",
                    "base_sha256": hashlib.sha256(self.originals[name]).hexdigest()
                    if name in self.originals
                    else None,
                }
            )
        self.write_json(
            "manifests/overlay.json",
            {
                "schema_version": 1,
                "source_version": "1",
                "files": overlay_rows,
            },
        )
        placeholder = self.bundle_root / "tools" / "placeholder.py"
        placeholder.parent.mkdir()
        placeholder.write_bytes(b"# pinned public source fixture\n")
        self.archive_sha256 = sha256_file(self.archive_path)
        self.write_json(
            "bundle.json",
            {
                "schema_version": 1,
                "source_version": "1",
                "engine_version": "5.4.4",
                "original_archive": {
                    "bytes": self.archive_path.stat().st_size,
                    "sha256": self.archive_sha256,
                },
                "original_manifest": "manifests/original-project.json",
                "overlay_manifest": "manifests/overlay.json",
                "final_manifest": "manifests/final-project.json",
                "files": [],
            },
        )
        self.repin()
        return self.load_fixture()

    def load_fixture(self, **kwargs):
        return load_bundle(
            self.bundle_root, expected_archive_sha256=self.archive_sha256, **kwargs
        )

    def assert_not_extracted(self) -> None:
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.root.glob(".npc-restore-*")), [])

    def test_valid_restore_applies_exact_overlay_and_excludes_generated_files(
        self,
    ) -> None:
        bundle = self.fixture()
        report = restore_project(self.archive_path, self.output, bundle)
        self.assertEqual(report["status"], "passed_source_restoration")
        self.assertFalse(report["gameplay_executed"])
        expected = self.originals | self.changes
        actual = {
            path.relative_to(self.output).as_posix(): path.read_bytes()
            for path in self.output.rglob("*")
            if path.is_file()
        }
        self.assertEqual(actual, expected)
        self.assertFalse((self.output / "NPC.sln").exists())
        self.assertFalse((self.output / "Saved").exists())
        self.assertEqual(list(self.root.glob(".npc-restore-*")), [])

    def test_unicode_output_path_and_asset_names_work(self) -> None:
        bundle = self.fixture()
        output = self.root / "\uc0c8 NPC \ud504\ub85c\uc81d\ud2b8"
        restore_project(self.archive_path, output, bundle)
        self.assertEqual(
            (output / "Content/\uce90\ub9ad\ud130.uasset").read_bytes(),
            self.originals["Content/\uce90\ub9ad\ud130.uasset"],
        )

    def test_existing_output_is_refused_without_altering_user_files(self) -> None:
        bundle = self.fixture()
        self.output.mkdir()
        sentinel = self.output / "UserChanges.txt"
        sentinel.write_bytes(b"keep my project")
        with self.assertRaisesRegex(BundleError, "already exists"):
            restore_project(self.archive_path, self.output, bundle)
        self.assertEqual(sentinel.read_bytes(), b"keep my project")
        self.assertEqual(list(self.output.iterdir()), [sentinel])

    def test_even_an_existing_empty_output_is_refused(self) -> None:
        bundle = self.fixture()
        self.output.mkdir()
        with self.assertRaisesRegex(BundleError, "already exists"):
            restore_project(self.archive_path, self.output, bundle)
        self.assertEqual(list(self.output.iterdir()), [])

    def test_archive_checksum_failure_has_no_extraction_side_effects(self) -> None:
        bundle = self.fixture()
        with self.archive_path.open("ab") as stream:
            stream.write(b"tampered archive")
        with self.assertRaisesRegex(BundleError, "SHA-256"):
            restore_project(self.archive_path, self.output, bundle)
        self.assert_not_extracted()

    def test_path_traversal_archive_member_is_rejected_before_writes(self) -> None:
        bundle = self.fixture([("../outside.txt", b"must never escape")])
        with self.assertRaisesRegex(BundleError, "path"):
            restore_project(self.archive_path, self.output, bundle)
        self.assertFalse((self.root.parent / "outside.txt").exists())
        self.assert_not_extracted()

    def test_symlink_member_is_rejected_even_in_an_excluded_directory(self) -> None:
        link = zipfile.ZipInfo("Saved/UnsafeLink")
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        bundle = self.fixture([(link, b"../../outside")])
        with self.assertRaisesRegex(BundleError, "links"):
            restore_project(self.archive_path, self.output, bundle)
        self.assert_not_extracted()

    def test_case_colliding_parent_directories_are_rejected(self) -> None:
        bundle = self.fixture([("Content/characters/Other.uasset", b"collision")])
        with self.assertRaisesRegex(BundleError, "collision"):
            restore_project(self.archive_path, self.output, bundle)
        self.assert_not_extracted()

    def test_archive_file_directory_collision_is_rejected(self) -> None:
        bundle = self.fixture(
            [("Content/Blocked", b"file"), ("Content/Blocked/Child.uasset", b"child")]
        )
        with self.assertRaisesRegex(BundleError, "collides"):
            restore_project(self.archive_path, self.output, bundle)
        self.assert_not_extracted()

    def test_reserved_windows_filename_is_rejected_on_every_os(self) -> None:
        bundle = self.fixture([("Content/AUX.uasset", b"device name")])
        with self.assertRaisesRegex(BundleError, "Reserved"):
            restore_project(self.archive_path, self.output, bundle)
        self.assert_not_extracted()

    def test_overlong_path_is_rejected_before_any_extraction(self) -> None:
        bundle = self.fixture([("Content/" + "x" * 230 + ".uasset", b"long")])
        with self.assertRaisesRegex(BundleError, "too long"):
            restore_project(self.archive_path, self.output, bundle)
        self.assert_not_extracted()

    def test_unknown_project_root_is_not_extracted(self) -> None:
        bundle = self.fixture(
            [("DifferentProject/Content/Other.uasset", b"unexpected")]
        )
        with self.assertRaisesRegex(BundleError, "outside"):
            restore_project(self.archive_path, self.output, bundle)
        self.assert_not_extracted()

    def test_bundle_source_version_must_match_the_requested_version(self) -> None:
        self.fixture()
        with self.assertRaisesRegex(BundleError, "version"):
            self.load_fixture(expected_source_version="other-version")

    def test_production_loader_only_accepts_the_audited_original_archive(self) -> None:
        self.fixture()
        with self.assertRaisesRegex(BundleError, "audited original"):
            load_bundle(self.bundle_root)

    def test_pinned_public_source_tampering_is_detected(self) -> None:
        self.fixture()
        (self.bundle_root / "tools/placeholder.py").write_bytes(
            b"unexpected source edit"
        )
        with self.assertRaisesRegex(BundleError, "checksum mismatch"):
            self.load_fixture()

    def test_unlisted_overlay_payload_is_rejected(self) -> None:
        self.fixture()
        (self.bundle_root / "overlay/Unexpected.txt").write_bytes(b"unlisted")
        with self.assertRaisesRegex(BundleError, "unlisted"):
            self.load_fixture()

    def test_overlay_tampered_after_verification_leaves_partial_without_publishing(
        self,
    ) -> None:
        bundle = self.fixture()
        (self.bundle_root / "overlay/Config/DefaultGame.ini").write_bytes(
            b"tampered after verification"
        )
        with self.assertRaises(BundleError) as captured:
            restore_project(self.archive_path, self.output, bundle)
        self.assertFalse(self.output.exists())
        partial = captured.exception.partial_directory
        self.assertIsNotNone(partial)
        self.assertTrue(partial.is_dir())
        self.assertEqual(partial.parent, self.root)

    def test_concurrent_new_output_is_not_overwritten_during_final_publish(
        self,
    ) -> None:
        bundle = self.fixture()
        publish = bundle_lib._publish_without_overwrite

        def another_process_created_output(source: Path, destination: Path) -> None:
            destination.mkdir()
            (destination / "UserFile.txt").write_bytes(b"concurrent user's data")
            publish(source, destination)

        with (
            patch(
                "bundle_lib._publish_without_overwrite",
                side_effect=another_process_created_output,
            ),
            self.assertRaises(BundleError) as captured,
        ):
            restore_project(self.archive_path, self.output, bundle)
        self.assertEqual(
            (self.output / "UserFile.txt").read_bytes(), b"concurrent user's data"
        )
        self.assertTrue(captured.exception.partial_directory.is_dir())


if __name__ == "__main__":
    unittest.main()
