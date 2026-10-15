"""Offline, standard-library verification and safe restoration for the NPC bundle.

These tools verify files, not Unreal gameplay. Windows and Linux are supported
for restoration; the separately reported game checks use Windows UE 5.4.4.
"""

from __future__ import annotations

import ctypes
import errno
import hashlib
import json
import os
import re
import stat
import sys
import tempfile
import unicodedata
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, BinaryIO

ORIGINAL_ARCHIVE_SHA256 = (
    "ba8e4ba770461e5272ff6cfaeaf4be9fe0219ec3fa630a404572c627e8080bfe"
)
MAX_FILES = 100_000
MAX_FILE_BYTES = 2 * 1024**3
MAX_TOTAL_BYTES = 8 * 1024**3
SOURCE_ROOTS = frozenset({"Content", "Config", "Source", "Plugins"})
GENERATED_ROOTS = frozenset(
    {
        "Binaries",
        "Intermediate",
        "Saved",
        "DerivedDataCache",
        ".git",
        ".vs",
        ".vscode",
        "__pycache__",
        ".pytest_cache",
    }
)
GENERATED_FOLDED = frozenset(name.casefold() for name in GENERATED_ROOTS)
SHA256 = re.compile(r"[0-9a-f]{64}")
VERSION = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
RESERVED = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"} | {
    f"{prefix}{number}" for prefix in ("COM", "LPT") for number in range(1, 10)
}


class BundleError(RuntimeError):
    """An input cannot be verified or safely restored."""

    def __init__(self, message: str, *, partial_directory: Path | None = None) -> None:
        super().__init__(message)
        self.partial_directory = partial_directory


@dataclass(frozen=True)
class FileRecord:
    path: str
    bytes: int
    sha256: str


@dataclass(frozen=True)
class OverlayRecord:
    path: str
    payload: str
    bytes: int
    sha256: str
    base_sha256: str | None


@dataclass(frozen=True)
class Bundle:
    root: Path
    source_version: str
    engine_version: str
    archive_sha256: str
    archive_bytes: int
    original: tuple[FileRecord, ...]
    overlay: tuple[OverlayRecord, ...]
    final: tuple[FileRecord, ...]
    verified_bundle_files: int


def sha256_file(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def portable_path(value: Any) -> str:
    """Validate the same relative filename rules on Windows and Linux."""
    if (
        not isinstance(value, str)
        or not value
        or "\\" in value
        or value.startswith("/")
    ):
        raise BundleError("Paths must be nonempty relative names with forward slashes")
    try:
        length = len(value.encode("utf-16-le")) // 2
    except UnicodeError as error:
        raise BundleError("A path contains invalid Unicode") from error
    if length > 220:
        raise BundleError(f"Relative path is too long: {value}")
    parts = value.split("/")
    for part in parts:
        if part in ("", ".", "..") or part.endswith((" ", ".")):
            raise BundleError(f"Unsafe path component: {value}")
        if any(ord(character) < 32 or character in '<>:"|?*' for character in part):
            raise BundleError(f"Nonportable path: {value}")
        if part.split(".", 1)[0].upper() in RESERVED:
            raise BundleError(f"Reserved Windows filename: {value}")
    if PurePosixPath(value).is_absolute():
        raise BundleError(f"Absolute path is not allowed: {value}")
    return value


def _source_path(value: str) -> str:
    value = portable_path(value)
    if value != "NPC.uproject" and (
        value.split("/", 1)[0] not in SOURCE_ROOTS or "/" not in value
    ):
        raise BundleError(f"File is outside the NPC source roots: {value}")
    if _generated(value):
        raise BundleError(f"Generated files are not source overlay targets: {value}")
    return value


def _generated(value: str) -> bool:
    parts = value.split("/")
    return (
        parts[0].casefold() in GENERATED_FOLDED
        or (len(parts) == 1 and value.lower().endswith(".sln"))
        or (
            parts[0] == "Plugins"
            and any(part.casefold() in GENERATED_FOLDED for part in parts[1:])
        )
    )


def _register_case_path(names: dict[str, str], path: str) -> None:
    parts = path.split("/")
    for length in range(1, len(parts) + 1):
        prefix = "/".join(parts[:length])
        folded = unicodedata.normalize("NFC", prefix).casefold()
        previous = names.setdefault(folded, prefix)
        if previous != prefix:
            raise BundleError(f"Case or Unicode normalization collision: {prefix}")


def inside(root: Path, relative: str) -> Path:
    relative = portable_path(relative)
    candidate = root / relative
    current = root
    for part in relative.split("/"):
        current /= part
        if current.is_symlink():
            raise BundleError(f"Symbolic links are not allowed: {relative}")
    try:
        candidate.resolve().relative_to(root.resolve())
    except ValueError as error:
        raise BundleError(f"Path escapes its allowed root: {relative}") from error
    return candidate


def _integer(value: Any, label: str) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or not 0 <= value <= MAX_FILE_BYTES
    ):
        raise BundleError(f"Invalid byte size: {label}")
    return value


def _sha256(value: Any, label: str) -> str:
    if not isinstance(value, str) or SHA256.fullmatch(value) is None:
        raise BundleError(f"Invalid SHA-256: {label}")
    return value


def _json(path: Path) -> dict[str, Any]:
    try:
        if path.stat().st_size > 32 * 1024**2:
            raise BundleError("A bundle metadata file is unexpectedly large")
        result = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as error:
        raise BundleError(f"Cannot read valid JSON metadata: {path.name}") from error
    if not isinstance(result, dict):
        raise BundleError(f"Metadata must be an object: {path.name}")
    return result


def _records(rows: Any, *, source: bool) -> tuple[FileRecord, ...]:
    if not isinstance(rows, list) or not rows or len(rows) > MAX_FILES:
        raise BundleError("A file manifest must contain a bounded nonempty list")
    records = []
    seen: set[str] = set()
    case_names: dict[str, str] = {}
    for row in rows:
        if not isinstance(row, dict):
            raise BundleError("A manifest file record must be an object")
        name = portable_path(row.get("path"))
        if source:
            _source_path(name)
        if name in seen:
            raise BundleError(f"Duplicate manifest file: {name}")
        _register_case_path(case_names, name)
        seen.add(name)
        records.append(
            FileRecord(
                name, _integer(row.get("bytes"), name), _sha256(row.get("sha256"), name)
            )
        )
    if sum(record.bytes for record in records) > MAX_TOTAL_BYTES:
        raise BundleError("The declared file total exceeds the restoration limit")
    for name in seen:
        if any(
            "/".join(name.split("/")[:length]) in seen
            for length in range(1, len(name.split("/")))
        ):
            raise BundleError(
                f"A manifest file collides with a parent directory: {name}"
            )
    return tuple(records)


def _file_matches(path: Path, record: FileRecord) -> bool:
    return (
        path.is_file()
        and path.stat().st_size == record.bytes
        and sha256_file(path) == record.sha256
    )


def load_bundle(
    root: Path,
    *,
    expected_source_version: str | None = None,
    expected_archive_sha256: str = ORIGINAL_ARCHIVE_SHA256,
) -> Bundle:
    """Verify payload and metadata before any restoration writes.

    The CLI always binds the audited original archive. Unit tests pass their
    explicitly constructed small archive identity through this internal API.
    """
    root = root.resolve()
    if not root.is_dir():
        raise BundleError("The bundle root must be an existing directory")
    description = _json(inside(root, "bundle.json"))
    if description.get("schema_version") != 1:
        raise BundleError("Unsupported bundle schema version")
    version = description.get("source_version")
    if not isinstance(version, str) or VERSION.fullmatch(version) is None:
        raise BundleError(
            "source_version must be a nonempty portable version identifier"
        )
    if expected_source_version is not None and version != expected_source_version:
        raise BundleError(
            "The bundle source version does not match the requested version"
        )
    engine = description.get("engine_version")
    if not isinstance(engine, str) or not engine.strip() or len(engine) > 64:
        raise BundleError("The engine version must be explicitly declared")
    archive = description.get("original_archive")
    if (
        not isinstance(archive, dict)
        or archive.get("sha256") != expected_archive_sha256
    ):
        raise BundleError(
            "The bundle does not identify the audited original Project.zip"
        )
    archive_bytes = archive.get("bytes")
    if (
        isinstance(archive_bytes, bool)
        or not isinstance(archive_bytes, int)
        or not 0 < archive_bytes <= MAX_TOTAL_BYTES
    ):
        raise BundleError("Invalid original archive byte size")
    listed = _records(description.get("files"), source=False)
    by_name = {record.path: record for record in listed}
    if "bundle.json" in by_name:
        raise BundleError("bundle.json cannot recursively hash itself")
    for record in listed:
        if not _file_matches(inside(root, record.path), record):
            raise BundleError(
                f"Bundle checksum mismatch or missing file: {record.path}"
            )
    metadata = {}
    for key in ("original_manifest", "overlay_manifest", "final_manifest"):
        name = portable_path(description.get(key))
        if name not in by_name:
            raise BundleError(f"Metadata is not pinned in bundle.json: {key}")
        data = _json(inside(root, name))
        if data.get("schema_version") != 1 or data.get("source_version") != version:
            raise BundleError(f"Metadata source/schema version mismatch: {key}")
        metadata[key] = data
    original = _records(metadata["original_manifest"].get("files"), source=True)
    final = _records(metadata["final_manifest"].get("files"), source=True)
    originals = {record.path: record for record in original}
    finals = {record.path: record for record in final}
    rows = metadata["overlay_manifest"].get("files")
    if not isinstance(rows, list) or len(rows) > MAX_FILES:
        raise BundleError("Overlay files must be a bounded list")
    overlay = []
    targets: set[str] = set()
    payloads: set[str] = set()
    cases: dict[str, str] = {}
    for row in rows:
        if not isinstance(row, dict):
            raise BundleError("An overlay record must be an object")
        name = _source_path(row.get("path"))
        payload = portable_path(row.get("payload"))
        if not payload.startswith("overlay/"):
            raise BundleError("Overlay payloads must be inside overlay/")
        if name in targets or payload in payloads:
            raise BundleError("Duplicate overlay target or payload")
        targets.add(name)
        payloads.add(payload)
        _register_case_path(cases, name)
        size = _integer(row.get("bytes"), name)
        fingerprint = _sha256(row.get("sha256"), name)
        base = row.get("base_sha256")
        expected_base = originals[name].sha256 if name in originals else None
        if base != expected_base:
            raise BundleError(f"Overlay base hash mismatch: {name}")
        record = FileRecord(payload, size, fingerprint)
        if by_name.get(payload) != record:
            raise BundleError(f"Overlay payload is not pinned correctly: {payload}")
        if finals.get(name) != FileRecord(name, size, fingerprint):
            raise BundleError(
                f"Final manifest does not identify overlay content: {name}"
            )
        overlay.append(OverlayRecord(name, payload, size, fingerprint, base))
    if set(finals) != set(originals) | targets:
        raise BundleError(
            "Final manifest must preserve all original source files and only add declared overlay files"
        )
    for name in set(originals) - targets:
        if originals[name] != finals[name]:
            raise BundleError(
                f"An undeclared source change appears in the final manifest: {name}"
            )
    overlay_root = root / "overlay"
    if overlay_root.is_dir():
        actual_payloads = {
            path.relative_to(root).as_posix()
            for path in overlay_root.rglob("*")
            if path.is_file()
        }
        if actual_payloads != payloads:
            raise BundleError("The overlay contains unlisted or missing payload files")
    return Bundle(
        root,
        version,
        engine,
        expected_archive_sha256,
        archive_bytes,
        original,
        tuple(overlay),
        final,
        len(listed),
    )


def _validate_archive(
    archive: zipfile.ZipFile, bundle: Bundle
) -> dict[str, zipfile.ZipInfo]:
    entries = archive.infolist()
    if len(entries) > MAX_FILES:
        raise BundleError("The archive contains too many entries")
    names: dict[str, str] = {}
    seen: set[str] = set()
    files: set[str] = set()
    directories: set[str] = set()
    selected = {}
    total = 0
    for entry in entries:
        raw = entry.filename
        name = portable_path(raw[:-1] if entry.is_dir() else raw)
        _register_case_path(names, name)
        if name in seen:
            raise BundleError(f"Duplicate archive member: {name}")
        seen.add(name)
        mode = entry.external_attr >> 16
        file_type = stat.S_IFMT(mode)
        if stat.S_ISLNK(mode) or file_type not in (0, stat.S_IFREG, stat.S_IFDIR):
            raise BundleError(
                f"Archive links or special files are not supported: {name}"
            )
        if entry.flag_bits & 1:
            raise BundleError("Encrypted archive members are not supported")
        _integer(entry.file_size, name)
        total += entry.file_size
        if total > MAX_TOTAL_BYTES:
            raise BundleError("The archive exceeds the total uncompressed size limit")
        parts = name.split("/")
        directories.update("/".join(parts[:length]) for length in range(1, len(parts)))
        if entry.is_dir():
            directories.add(name)
            continue
        files.add(name)
        if _generated(name):
            continue
        _source_path(name)
        selected[name] = entry
    if files & directories:
        raise BundleError("An archive file collides with a directory")
    expected = {record.path: record for record in bundle.original}
    if set(selected) != set(expected):
        raise BundleError(
            "Archive source membership differs from the original manifest"
        )
    for name, entry in selected.items():
        if entry.file_size != expected[name].bytes:
            raise BundleError(f"Archive member size mismatch: {name}")
    return selected


def _write_stream(source: BinaryIO, target: Path, expected: FileRecord) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    count = 0
    with target.open("xb") as destination:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            count += len(chunk)
            if count > expected.bytes:
                raise BundleError(
                    f"Extracted content exceeds declared size: {expected.path}"
                )
            digest.update(chunk)
            destination.write(chunk)
        destination.flush()
        os.fsync(destination.fileno())
    if count != expected.bytes or digest.hexdigest() != expected.sha256:
        raise BundleError(f"Extracted content checksum mismatch: {expected.path}")


def _publish_without_overwrite(source: Path, destination: Path) -> None:
    if os.name == "nt":
        os.rename(source, destination)
        return
    if sys.platform.startswith("linux"):
        library = ctypes.CDLL(None, use_errno=True)
        rename = getattr(library, "renameat2", None)
        if rename is None:
            raise BundleError(
                "This Linux runtime cannot publish atomically without overwriting"
            )
        rename.argtypes = [
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_uint,
        ]
        rename.restype = ctypes.c_int
        if rename(-100, os.fsencode(source), -100, os.fsencode(destination), 1) != 0:
            code = ctypes.get_errno()
            if code == errno.EEXIST:
                raise FileExistsError(
                    code, "The output appeared during restoration", destination
                )
            raise OSError(code, os.strerror(code), destination)
        return
    raise BundleError(
        "Atomic restoration publication is supported on Windows and Linux only"
    )


def restore_project(archive_path: Path, output: Path, bundle: Bundle) -> dict[str, Any]:
    """Restore into a new directory; any partial staging tree is retained."""
    if os.path.lexists(output):
        raise BundleError(
            "Output already exists; choose a new directory, even if it is empty"
        )
    parent = output.absolute().parent.resolve()
    if not parent.is_dir():
        raise BundleError("The output parent directory must already exist")
    output = parent / output.name
    if not archive_path.is_file():
        raise BundleError("The original archive must be an existing local file")
    if (
        archive_path.stat().st_size != bundle.archive_bytes
        or sha256_file(archive_path) != bundle.archive_sha256
    ):
        raise BundleError(
            "The original archive size or SHA-256 does not match the audited source"
        )
    for record in bundle.final:
        if len(str(output / record.path).encode("utf-16-le")) // 2 > 240:
            raise BundleError(
                "The output path is too long for a portable Unreal project; choose a shorter parent"
            )
    staging = None
    try:
        with zipfile.ZipFile(archive_path) as archive:
            selected = _validate_archive(archive, bundle)
            # No filesystem writes occur before every member path is checked.
            staging = Path(tempfile.mkdtemp(prefix=".npc-restore-", dir=parent))
            for record in bundle.original:
                target = inside(staging, record.path)
                with archive.open(selected[record.path]) as stream:
                    _write_stream(stream, target, record)
        for record in bundle.overlay:
            target = inside(staging, record.path)
            if record.base_sha256 is not None:
                if not target.is_file() or sha256_file(target) != record.base_sha256:
                    raise BundleError(
                        f"Extracted base file differs before overlay: {record.path}"
                    )
            elif os.path.lexists(target):
                raise BundleError(
                    f"A new overlay target unexpectedly exists: {record.path}"
                )
            payload = inside(bundle.root, record.payload)
            # Replace only our own verified staging file, never user output.
            temporary = target.with_name(target.name + ".npc-overlay")
            with payload.open("rb") as stream:
                _write_stream(
                    stream,
                    temporary,
                    FileRecord(record.path, record.bytes, record.sha256),
                )
            os.replace(temporary, target)
        actual = {
            path.relative_to(staging).as_posix()
            for path in staging.rglob("*")
            if path.is_file()
        }
        if actual != {record.path for record in bundle.final}:
            raise BundleError(
                "The final restored source membership differs from the final manifest"
            )
        for record in bundle.final:
            if not _file_matches(inside(staging, record.path), record):
                raise BundleError(f"Final project checksum mismatch: {record.path}")
        _publish_without_overwrite(staging, output)
    except (BundleError, OSError, zipfile.BadZipFile, RuntimeError) as error:
        if staging is not None:
            raise BundleError(
                f"Restoration stopped: {error}", partial_directory=staging
            ) from error
        raise BundleError(f"Restoration stopped before extraction: {error}") from error
    return {
        "status": "passed_source_restoration",
        "source_version": bundle.source_version,
        "engine_version": bundle.engine_version,
        "original_archive_sha256": bundle.archive_sha256,
        "original_source_files": len(bundle.original),
        "overlay_files": len(bundle.overlay),
        "verified_final_source_files": len(bundle.final),
        "output": str(output),
        "gameplay_executed": False,
        "scope": "File identity and restoration only; Unreal gameplay requires separate engine execution.",
    }
