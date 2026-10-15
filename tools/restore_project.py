"""Restore the original NPC project plus this verified improvement overlay."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from bundle_lib import BundleError, load_bundle, restore_project


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--archive",
        type=Path,
        required=True,
        help="Existing original Project.zip; downloaded manually",
    )
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="New nonexisting project directory; parent must exist",
    )
    parser.add_argument(
        "--overlay",
        "--bundle-root",
        dest="bundle_root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="Improvement bundle root containing bundle.json (default: this package)",
    )
    parser.add_argument(
        "--expected-source-version",
        help="Optionally require an exact declared bundle version",
    )
    args = parser.parse_args()
    try:
        bundle = load_bundle(
            args.bundle_root, expected_source_version=args.expected_source_version
        )
        report = restore_project(args.archive, args.output, bundle)
    except (BundleError, OSError) as error:
        print(f"Restore failed: {error}", file=sys.stderr)
        if isinstance(error, BundleError) and error.partial_directory is not None:
            print(
                f"Partial files were retained at: {error.partial_directory}",
                file=sys.stderr,
            )
        return 2
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
