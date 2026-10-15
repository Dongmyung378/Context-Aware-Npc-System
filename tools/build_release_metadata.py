"""Build an offline restoration overlay from the pinned original and frozen source.

This release-author tool hashes real input bytes; it does not build or execute UE.
It preserves the repository's historical files and never writes to the project or
original archive. Use an existing bundle root containing the reviewed public docs,
tools and reports, without previously generated overlay/manifests/bundle.json.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
import sys
import tempfile
import zipfile
from dataclasses import asdict
from pathlib import Path
from typing import Any

import bundle_lib as bl

RESERVED_OUTPUTS = ("overlay", "manifests", "bundle.json")
EXCLUDED_ROOTS = frozenset({"downloads", "unreal-assets", "overlay", "manifests"})
HISTORICAL_SUFFIXES = frozenset({".pdf", ".ppt", ".pptx", ".zip", ".7z", ".rar"})
ENGINE_VERSION = "5.4.4"


def _linked(path: Path) -> bool:
    attributes = getattr(path.lstat(), "st_file_attributes", 0)
    return path.is_symlink() or bool(
        attributes & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    )


def _record(path: Path, relative: str) -> bl.FileRecord:
    bl.portable_path(relative)
    size = path.stat().st_size
    if not 0 <= size <= bl.MAX_FILE_BYTES:
        raise bl.BundleError(
            f"Source or payload exceeds the per-file limit: {relative}"
        )
    return bl.FileRecord(relative, size, bl.sha256_file(path))


def _walk(root: Path, *, payload: bool) -> list[Path]:
    result: list[Path] = []
    for directory, names, files in os.walk(root, followlinks=False):
        current = Path(directory)
        for name in list(names):
            relative = (current / name).relative_to(root).as_posix()
            parts = relative.split("/")
            excluded = any(
                part.casefold() in bl.GENERATED_FOLDED for part in parts
            ) or (payload and parts[0].casefold() in EXCLUDED_ROOTS)
            if excluded:
                names.remove(name)
            elif _linked(current / name):
                raise bl.BundleError(
                    f"Linked directories are not release source: {relative}"
                )
        for name in files:
            path = current / name
            relative = path.relative_to(root).as_posix()
            if payload and (
                name == "bundle.json" or path.suffix.casefold() in HISTORICAL_SUFFIXES
            ):
                continue
            if any(
                part.casefold() in bl.GENERATED_FOLDED for part in relative.split("/")
            ):
                continue
            if _linked(path) or not stat.S_ISREG(path.lstat().st_mode):
                raise bl.BundleError(
                    f"Linked or special files are not release source: {relative}"
                )
            bl.inside(root, relative)
            result.append(path)
    return sorted(result, key=lambda path: path.relative_to(root).as_posix())


def original_manifest(
    archive_path: Path, version: str, archive_sha256: str
) -> list[bl.FileRecord]:
    """Read every original source member, after the shared safe ZIP preflight."""
    with zipfile.ZipFile(archive_path) as archive:
        # The shared preflight needs membership/size records; their placeholder
        # digests are never emitted. Real member hashes are computed below.
        preliminary = tuple(
            bl.FileRecord(entry.filename, entry.file_size, "0" * 64)
            for entry in archive.infolist()
            if not entry.is_dir() and not bl._generated(entry.filename)
        )
        inspection = bl.Bundle(
            root=archive_path.parent,
            source_version=version,
            engine_version=ENGINE_VERSION,
            archive_sha256=archive_sha256,
            archive_bytes=archive_path.stat().st_size,
            original=preliminary,
            overlay=(),
            final=(),
            verified_bundle_files=0,
        )
        selected = bl._validate_archive(archive, inspection)
        records = []
        for name, entry in sorted(selected.items()):
            digest = hashlib.sha256()
            count = 0
            with archive.open(entry) as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    count += len(chunk)
                    digest.update(chunk)
            if count != entry.file_size:
                raise bl.BundleError(f"Original archive member size changed: {name}")
            records.append(bl.FileRecord(name, count, digest.hexdigest()))
    return list(bl._records([asdict(record) for record in records], source=True))


def project_manifest(project: Path) -> tuple[Path, list[bl.FileRecord]]:
    project = project.resolve(strict=True)
    if not project.is_file() or project.name != "NPC.uproject" or _linked(project):
        raise bl.BundleError("--project must identify a regular NPC.uproject file")
    root = project.parent
    records = [_record(project, "NPC.uproject")]
    for name in sorted(bl.SOURCE_ROOTS):
        source = root / name
        if not source.exists():
            continue
        if not source.is_dir() or _linked(source):
            raise bl.BundleError(
                f"A project source root is not a regular directory: {name}"
            )
        for path in _walk(source, payload=False):
            relative = path.relative_to(root).as_posix()
            if not bl._generated(relative):
                bl._source_path(relative)
                records.append(_record(path, relative))
    records.sort(key=lambda record: record.path)
    return root, list(bl._records([asdict(record) for record in records], source=True))


def _write_json(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(document, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def _copy_checked(source: Path, destination: Path, record: bl.FileRecord) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as reader, destination.open("xb") as writer:
        shutil.copyfileobj(reader, writer, 1024 * 1024)
    if (
        destination.stat().st_size != record.bytes
        or bl.sha256_file(destination) != record.sha256
    ):
        raise bl.BundleError(
            f"A frozen source or payload changed during the build: {record.path}"
        )


def create_declared_zip(
    bundle_root: Path,
    output: Path,
    *,
    max_bytes: int,
    expected_archive_sha256: str = bl.ORIGINAL_ARCHIVE_SHA256,
) -> dict[str, Any]:
    """Include exactly bundle.json and its pinned files, with deterministic headers."""
    bundle = bl.load_bundle(
        bundle_root, expected_archive_sha256=expected_archive_sha256
    )
    description = json.loads((bundle_root / "bundle.json").read_text(encoding="utf-8"))
    names = sorted(["bundle.json", *(row["path"] for row in description["files"])])
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise bl.BundleError(
            "ZIP output must not exist and its parent must already exist"
        )
    with zipfile.ZipFile(
        output, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        for name in names:
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = (stat.S_IFREG | 0o644) << 16
            with (
                bl.inside(bundle_root, name).open("rb") as source,
                archive.open(info, "w") as target,
            ):
                shutil.copyfileobj(source, target, 1024 * 1024)
    size = output.stat().st_size
    if size > max_bytes:
        raise bl.BundleError(
            f"ZIP is {size} bytes, above the explicit {max_bytes}-byte budget; retained at {output}"
        )
    return {
        "bytes": size,
        "sha256": bl.sha256_file(output),
        "declared_members": len(names),
        "source_version": bundle.source_version,
    }


def build_release(
    archive: Path,
    project: Path,
    bundle_root: Path,
    source_version: str,
    *,
    zip_output: Path | None = None,
    max_zip_bytes: int = 6_800_000,
    expected_archive_sha256: str = bl.ORIGINAL_ARCHIVE_SHA256,
) -> dict[str, Any]:
    """Author metadata from actual files; the CLI always binds the audited archive.

    expected_archive_sha256 is an internal fixture seam, not a CLI override.
    Existing payload/history is preserved. Generated metadata targets must be new.
    """
    if bl.VERSION.fullmatch(source_version) is None:
        raise bl.BundleError("Invalid source-version identifier")
    if not 1 <= max_zip_bytes <= 256 * 1024**2:
        raise bl.BundleError("ZIP budget must be between 1 byte and 256 MiB")
    archive = archive.resolve(strict=True)
    bundle_root = bundle_root.resolve(strict=True)
    if not archive.is_file() or not bundle_root.is_dir():
        raise bl.BundleError("Archive and existing bundle-root directory are required")
    for name in RESERVED_OUTPUTS:
        destination = bundle_root / name
        if destination.exists() or destination.is_symlink():
            raise bl.BundleError(
                f"Refusing to overwrite existing generated release content: {name}"
            )
    if zip_output is not None:
        zip_output = zip_output.absolute()
        if (
            zip_output.exists()
            or zip_output.is_symlink()
            or not zip_output.parent.is_dir()
        ):
            raise bl.BundleError(
                "Choose a new ZIP output with an existing parent directory"
            )
    archive_before = archive.stat()
    identity = bl.sha256_file(archive)
    if identity != expected_archive_sha256:
        raise bl.BundleError(
            "Original archive SHA-256 differs from the pinned Project.zip"
        )
    originals = original_manifest(archive, source_version, identity)
    archive_after = archive.stat()
    if (archive_before.st_size, archive_before.st_mtime_ns) != (
        archive_after.st_size,
        archive_after.st_mtime_ns,
    ):
        raise bl.BundleError("The original archive changed while its manifest was read")
    project_root, finals = project_manifest(project)
    if (
        project_root == bundle_root
        or project_root.is_relative_to(bundle_root)
        or bundle_root.is_relative_to(project_root)
    ):
        raise bl.BundleError("Project and release bundle roots must be separate trees")
    original_by_name = {record.path: record for record in originals}
    final_by_name = {record.path: record for record in finals}
    missing = sorted(set(original_by_name) - set(final_by_name))
    if missing:
        raise bl.BundleError(
            f"Original source files must be preserved; missing: {missing[0]}"
        )
    changes = [
        record for record in finals if original_by_name.get(record.path) != record
    ]
    payloads = [
        _record(path, path.relative_to(bundle_root).as_posix())
        for path in _walk(bundle_root, payload=True)
    ]
    if not payloads:
        raise bl.BundleError(
            "Bundle root must already contain reviewed public tools and documentation"
        )
    # Validate every existing payload name/case before the first publication write.
    bl._records([asdict(record) for record in payloads], source=False)
    stage = Path(
        tempfile.mkdtemp(prefix=".npc-release-stage-", dir=bundle_root.parent)
    ).resolve()
    try:
        for record in payloads:
            _copy_checked(
                bl.inside(bundle_root, record.path),
                bl.inside(stage, record.path),
                record,
            )
        overlay_rows = []
        for record in changes:
            payload = f"overlay/{record.path}"
            _copy_checked(
                bl.inside(project_root, record.path), bl.inside(stage, payload), record
            )
            base = original_by_name.get(record.path)
            overlay_rows.append(
                {
                    **asdict(record),
                    "payload": payload,
                    "base_sha256": base.sha256 if base else None,
                }
            )
        common = {"schema_version": 1, "source_version": source_version}
        _write_json(
            stage / "manifests/original-project.json",
            {**common, "files": [asdict(record) for record in originals]},
        )
        _write_json(
            stage / "manifests/final-project.json",
            {**common, "files": [asdict(record) for record in finals]},
        )
        _write_json(stage / "manifests/overlay.json", {**common, "files": overlay_rows})
        declared = [
            _record(path, path.relative_to(stage).as_posix())
            for path in _walk(stage, payload=False)
        ]
        description = {
            **common,
            "engine_version": ENGINE_VERSION,
            "original_archive": {"bytes": archive_before.st_size, "sha256": identity},
            "original_manifest": "manifests/original-project.json",
            "overlay_manifest": "manifests/overlay.json",
            "final_manifest": "manifests/final-project.json",
            "files": [asdict(record) for record in declared],
        }
        _write_json(stage / "bundle.json", description)
        verified = bl.load_bundle(
            stage,
            expected_source_version=source_version,
            expected_archive_sha256=expected_archive_sha256,
        )
        zip_report = None
        if zip_output is not None:
            zip_report = create_declared_zip(
                stage,
                zip_output,
                max_bytes=max_zip_bytes,
                expected_archive_sha256=expected_archive_sha256,
            )
        # Only these newly generated targets are published. Source docs/history
        # remain in place; no broad copy, overwrite, or recursive delete occurs.
        for name in RESERVED_OUTPUTS:
            source = stage / name
            if source.exists():
                bl._publish_without_overwrite(source, bundle_root / name)
        bl.load_bundle(
            bundle_root,
            expected_source_version=source_version,
            expected_archive_sha256=expected_archive_sha256,
        )
        return {
            "status": "release_metadata_verified",
            "source_version": source_version,
            "original_archive_sha256": identity,
            "original_source_files": len(originals),
            "final_source_files": len(finals),
            "overlay_files": len(changes),
            "replaced_source_files": sum(
                record.path in original_by_name for record in changes
            ),
            "added_source_files": sum(
                record.path not in original_by_name for record in changes
            ),
            "verified_declared_bundle_files": verified.verified_bundle_files,
            "zip": zip_report,
            "retained_build_stage": str(stage),
            "scope": "Actual archive/source/payload hashes and manifest graph only; no Unreal build or gameplay execution",
        }
    except (OSError, ValueError, RuntimeError, zipfile.BadZipFile) as error:
        raise bl.BundleError(
            f"Release preparation failed: {error}", partial_directory=stage
        ) from error


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--archive",
        required=True,
        type=Path,
        help="Existing pinned original Project.zip",
    )
    parser.add_argument(
        "--project",
        required=True,
        type=Path,
        help="Frozen working project's NPC.uproject",
    )
    parser.add_argument(
        "--bundle-root",
        required=True,
        type=Path,
        help="Existing reviewed public release files; no generated metadata yet",
    )
    parser.add_argument("--source-version", default="1")
    parser.add_argument(
        "--zip-output",
        type=Path,
        help="Optional new ZIP containing exactly declared files",
    )
    parser.add_argument("--max-zip-bytes", type=int, default=6_800_000)
    args = parser.parse_args()
    try:
        report = build_release(
            args.archive,
            args.project,
            args.bundle_root,
            args.source_version,
            zip_output=args.zip_output,
            max_zip_bytes=args.max_zip_bytes,
        )
    except (bl.BundleError, OSError, ValueError, zipfile.BadZipFile) as error:
        print(f"Release metadata build failed: {error}", file=sys.stderr)
        if isinstance(error, bl.BundleError) and error.partial_directory:
            print(f"Retained build stage: {error.partial_directory}", file=sys.stderr)
        return 2
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
