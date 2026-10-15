"""Synthetic release-authoring checks; these do not execute Unreal or gameplay."""

from __future__ import annotations

import hashlib
import json
import os
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

import build_release_metadata as author
import bundle_lib as bl


class ReleaseMetadataTests(unittest.TestCase):
    def setUp(self) -> None:
        # Windows sandbox temp ACLs are restrictive; use this private workspace.
        # Linux uses its native temp filesystem because NTFS/DrvFS may reject
        # renameat2(RENAME_NOREPLACE), which must never become an overwrite fallback.
        self.fixture_parent = (
            Path(__file__).resolve().parents[1]
            if os.name == "nt"
            else Path(tempfile.gettempdir()).resolve()
        )
        self.temporary = tempfile.TemporaryDirectory(
            prefix="release-fixture-", dir=self.fixture_parent
        )
        self.root = Path(self.temporary.name).resolve()
        self.assertTrue(self.root.is_relative_to(self.fixture_parent))
        self.addCleanup(self.temporary.cleanup)
        self.archive = self.root / "Project.zip"
        self.project_root = self.root / "project"
        self.bundle_root = self.root / "release"
        self.project_root.mkdir()
        self.bundle_root.mkdir()
        self.originals = {
            "NPC.uproject": b'{"EngineAssociation":"5.4"}\n',
            "Config/DefaultEngine.ini": b"[Engine]\nInitial=True\n",
            "Content/Example.uasset": b"original synthetic asset bytes\n",
            "Source/NPC/NPC.cpp": b"// original synthetic source\n",
        }
        with zipfile.ZipFile(
            self.archive, "w", compression=zipfile.ZIP_DEFLATED
        ) as archive:
            for name, data in self.originals.items():
                archive.writestr(name, data)
            archive.writestr("NPC.sln", b"generated solution, not game source\n")
        self.archive_sha = bl.sha256_file(self.archive)
        for name, data in self.originals.items():
            path = self.project_root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.write_project(
            "Config/DefaultEngine.ini", b"[Engine]\nInitial=True\nAudit=True\n"
        )
        self.write_project("Plugins/NpcAudit/NpcAudit.uplugin", b'{"FileVersion":3}\n')
        (self.bundle_root / "README.md").write_text(
            "Synthetic release fixture\n", encoding="utf-8"
        )
        history = {
            "docs/project-report.pdf": b"historical report, retain in repository",
            "docs/presentation.pptx": b"historical presentation",
            "downloads/npc-core-assets.zip": b"historical nested archive",
            "unreal-assets/old.uasset": b"original public asset history",
            ".git/config": b"private git metadata, never package",
        }
        for name, data in history.items():
            path = self.bundle_root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

    def write_project(self, relative: str, data: bytes) -> None:
        path = self.project_root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def build(self, **kwargs: object) -> dict:
        return author.build_release(
            self.archive,
            self.project_root / "NPC.uproject",
            self.bundle_root,
            "1",
            expected_archive_sha256=self.archive_sha,
            **kwargs,
        )

    def test_real_bytes_produce_complete_graph_and_exact_declared_zip(self) -> None:
        self.write_project(
            "Plugins/NpcAudit/Binaries/Win64/NpcAudit.dll", b"excluded generated DLL"
        )
        self.write_project(
            "Plugins/NpcAudit/Intermediate/objects.obj", b"excluded generated object"
        )
        output = self.root / "release.zip"
        result = self.build(zip_output=output)
        self.assertEqual(result["original_source_files"], 4)
        self.assertEqual(result["final_source_files"], 5)
        self.assertEqual(result["overlay_files"], 2)
        self.assertEqual(result["replaced_source_files"], 1)
        self.assertEqual(result["added_source_files"], 1)
        bundle = bl.load_bundle(
            self.bundle_root, expected_archive_sha256=self.archive_sha
        )
        config = next(
            row for row in bundle.overlay if row.path == "Config/DefaultEngine.ini"
        )
        self.assertEqual(
            config.base_sha256, hashlib.sha256(self.originals[config.path]).hexdigest()
        )
        plugin = next(row for row in bundle.overlay if row.path.endswith(".uplugin"))
        self.assertIsNone(plugin.base_sha256)
        descriptor = json.loads(
            (self.bundle_root / "bundle.json").read_text(encoding="utf-8")
        )
        declared = {row["path"] for row in descriptor["files"]}
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(set(archive.namelist()), declared | {"bundle.json"})
            self.assertTrue(
                all(
                    info.date_time == (1980, 1, 1, 0, 0, 0)
                    for info in archive.infolist()
                )
            )
        self.assertNotIn("docs/project-report.pdf", declared)
        self.assertNotIn("downloads/npc-core-assets.zip", declared)
        self.assertFalse(
            any(name.startswith(("unreal-assets/", ".git/")) for name in declared)
        )
        self.assertTrue((self.bundle_root / "docs/project-report.pdf").is_file())
        self.assertTrue((self.bundle_root / "unreal-assets/old.uasset").is_file())
        self.assertEqual(bl.sha256_file(self.archive), self.archive_sha)

    def test_missing_original_source_is_rejected_before_publication(self) -> None:
        (self.project_root / "Content/Example.uasset").unlink()
        with self.assertRaisesRegex(bl.BundleError, "must be preserved"):
            self.build()
        self.assertFalse((self.bundle_root / "bundle.json").exists())
        self.assertFalse((self.bundle_root / "overlay").exists())

    def test_wrong_archive_identity_is_rejected_without_publication(self) -> None:
        with self.assertRaisesRegex(bl.BundleError, "pinned"):
            author.build_release(
                self.archive, self.project_root / "NPC.uproject", self.bundle_root, "1"
            )
        self.assertFalse((self.bundle_root / "manifests").exists())

    def test_existing_metadata_is_never_overwritten(self) -> None:
        marker = self.bundle_root / "bundle.json"
        marker.write_text("keep prior metadata", encoding="utf-8")
        with self.assertRaisesRegex(bl.BundleError, "overwrite"):
            self.build()
        self.assertEqual(marker.read_text(encoding="utf-8"), "keep prior metadata")

    def test_archive_case_collision_is_rejected(self) -> None:
        with zipfile.ZipFile(self.archive, "a") as archive:
            archive.writestr("Content/example.uasset", b"case collision")
        self.archive_sha = bl.sha256_file(self.archive)
        with self.assertRaisesRegex(bl.BundleError, "collision"):
            self.build()
        self.assertFalse((self.bundle_root / "bundle.json").exists())

    def test_overlay_tampering_fails_verification(self) -> None:
        self.build()
        path = self.bundle_root / "overlay/Config/DefaultEngine.ini"
        path.write_bytes(b"tampered")
        with self.assertRaisesRegex(bl.BundleError, "checksum"):
            bl.load_bundle(self.bundle_root, expected_archive_sha256=self.archive_sha)

    def test_zip_budget_failure_retains_stage_without_publishing_metadata(self) -> None:
        output = self.root / "too-small.zip"
        with self.assertRaises(bl.BundleError) as captured:
            self.build(zip_output=output, max_zip_bytes=1)
        self.assertIsNotNone(captured.exception.partial_directory)
        stage = captured.exception.partial_directory
        self.assertTrue(stage.is_relative_to(self.root))
        self.assertTrue((stage / "bundle.json").is_file())
        self.assertTrue(output.is_file())
        self.assertFalse((self.bundle_root / "bundle.json").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
