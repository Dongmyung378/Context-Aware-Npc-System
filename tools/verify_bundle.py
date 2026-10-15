"""Verify this source bundle and overlay offline, without running Unreal."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from bundle_lib import BundleError, load_bundle


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--bundle-root", type=Path, default=Path(__file__).resolve().parents[1]
    )
    parser.add_argument("--expected-source-version")
    args = parser.parse_args()
    try:
        bundle = load_bundle(
            args.bundle_root, expected_source_version=args.expected_source_version
        )
    except (BundleError, OSError) as error:
        print(f"Bundle verification failed: {error}", file=sys.stderr)
        return 2
    print(
        json.dumps(
            {
                "status": "passed_bundle_file_verification",
                "source_version": bundle.source_version,
                "engine_version": bundle.engine_version,
                "verified_declared_bundle_files": bundle.verified_bundle_files,
                "original_source_files": len(bundle.original),
                "overlay_files": len(bundle.overlay),
                "final_source_files": len(bundle.final),
                "original_archive_sha256": bundle.archive_sha256,
                "archive_or_game_executed": False,
                "scope": "Declared source, metadata and overlay hashes only. This is not an Unreal gameplay test.",
            },
            ensure_ascii=False,
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
