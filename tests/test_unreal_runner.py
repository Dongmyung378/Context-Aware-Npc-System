"""Exercise Unreal report acceptance using synthetic report files, never launch UE.

These tests validate the public runner's report handling, not gameplay or rendering.
"""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import unittest
from copy import deepcopy
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

RUNNER_PATH = Path(__file__).resolve().parents[1] / "tools" / "run_unreal_checks.py"
SPEC = importlib.util.spec_from_file_location("npc_unreal_runner_under_test", RUNNER_PATH)
assert SPEC is not None and SPEC.loader is not None
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)

TEST_NAME = "NPC.Gameplay.NavigationContracts"


def successful_report() -> dict:
    """Minimal report matching the fields emitted by actual UE 5.4 automation."""
    return {
        "succeeded": 1,
        "succeededWithWarnings": 0,
        "failed": 0,
        "notRun": 0,
        "inProcess": 0,
        "tests": [{"fullTestPath": TEST_NAME, "state": "Success"}],
    }


class AutomationReportTests(unittest.TestCase):
    def setUp(self) -> None:
        fixture_root = Path(__file__).resolve().parent
        self.workspace = TemporaryDirectory(prefix="npc-unreal-report-", dir=fixture_root)
        self.report_directory = Path(self.workspace.name)
        self.assertTrue(self.report_directory.resolve().is_relative_to(fixture_root))
        self.addCleanup(self.workspace.cleanup)

    def write_report(self, payload: object, *, bom: bool = False) -> None:
        encoding = "utf-8-sig" if bom else "utf-8"
        (self.report_directory / "index.json").write_text(
            json.dumps(payload), encoding=encoding
        )

    def assert_rejected(self, payload: object) -> None:
        self.write_report(payload)
        with self.assertRaises(RuntimeError):
            RUNNER.check_automation(self.report_directory, TEST_NAME)

    def test_accepts_exact_success_report_and_returns_counts(self) -> None:
        self.write_report(successful_report())
        counts = RUNNER.check_automation(self.report_directory, TEST_NAME)
        self.assertEqual(counts["succeeded"], 1)
        self.assertEqual(counts["succeededWithWarnings"], 0)
        self.assertEqual(counts["failed"], 0)
        self.assertEqual(counts["notRun"], 0)

    def test_accepts_success_with_warning_and_utf8_bom(self) -> None:
        report = successful_report()
        report.update(succeeded=0, succeededWithWarnings=1)
        self.write_report(report, bom=True)
        counts = RUNNER.check_automation(self.report_directory, TEST_NAME)
        self.assertEqual(counts["succeededWithWarnings"], 1)

    def test_failed_test_rejected_even_if_engine_has_exited_zero(self) -> None:
        # Unreal can exit zero after a failed test. This gate must inspect the
        # saved result rather than assuming that a completed process passed.
        report = successful_report()
        report.update(succeeded=0, failed=1)
        report["tests"][0]["state"] = "Fail"
        self.assert_rejected(report)

    def test_failure_total_overrides_success_record(self) -> None:
        report = successful_report()
        report["failed"] = 1
        self.assert_rejected(report)

    def test_zero_success_total_is_not_a_completed_run(self) -> None:
        report = successful_report()
        report["succeeded"] = 0
        self.assert_rejected(report)

    def test_unfinished_or_not_run_totals_rejected(self) -> None:
        for key in ("notRun", "inProcess"):
            with self.subTest(key=key):
                report = successful_report()
                report[key] = 1
                self.assert_rejected(report)

    def test_missing_or_wrong_test_rejected(self) -> None:
        for tests in ([], [{"fullTestPath": TEST_NAME + "Other", "state": "Success"}]):
            with self.subTest(tests=tests):
                report = successful_report()
                report["tests"] = tests
                self.assert_rejected(report)

    def test_duplicate_matching_test_rejected(self) -> None:
        report = successful_report()
        report["tests"].append(deepcopy(report["tests"][0]))
        self.assert_rejected(report)

    def test_unexpected_extra_test_rejected(self) -> None:
        report = successful_report()
        report["tests"].append({"fullTestPath": "NPC.Other", "state": "Fail"})
        self.assert_rejected(report)

    def test_missing_counter_rejected(self) -> None:
        for key in ("succeeded", "succeededWithWarnings", "failed", "notRun", "inProcess"):
            with self.subTest(key=key):
                report = successful_report()
                del report[key]
                self.assert_rejected(report)

    def test_non_integer_or_negative_totals_rejected(self) -> None:
        for value in (False, True, "0", 0.0, -1, None):
            with self.subTest(value=value):
                report = successful_report()
                report["notRun"] = value
                self.assert_rejected(report)

    def test_malformed_report_shapes_rejected_with_runtime_error(self) -> None:
        for payload in ([], None, {}, {"tests": None}, {"tests": [None]}):
            with self.subTest(payload=payload):
                self.assert_rejected(payload)

    def test_missing_state_rejected(self) -> None:
        report = successful_report()
        del report["tests"][0]["state"]
        self.assert_rejected(report)

    def test_missing_index_file_is_not_success(self) -> None:
        with self.assertRaises(OSError):
            RUNNER.check_automation(self.report_directory, TEST_NAME)

    def test_invalid_json_is_not_success(self) -> None:
        (self.report_directory / "index.json").write_text("{broken", encoding="utf-8")
        with self.assertRaises(ValueError):
            RUNNER.check_automation(self.report_directory, TEST_NAME)


@unittest.skipUnless(os.name == "nt", "The actual engine runner is explicitly Windows-only")
class RunnerOrchestrationTests(unittest.TestCase):
    """Exercise CLI orchestration using inert files and a mocked child process."""

    def setUp(self) -> None:
        fixture_root = Path(__file__).resolve().parent
        self.workspace = TemporaryDirectory(prefix="npc-runner-cli-", dir=fixture_root)
        self.root = Path(self.workspace.name)
        self.assertTrue(self.root.resolve().is_relative_to(fixture_root))
        self.addCleanup(self.workspace.cleanup)
        self.engine = self.root / "engine with spaces"
        self.project = self.root / "restored project" / "NPC.uproject"
        self.output = self.root / "evidence"
        self.project.parent.mkdir()
        self.project.write_text("{}", encoding="utf-8")
        self.dotnet = self.engine / "Engine/Binaries/ThirdParty/DotNet/6.0.302/windows/dotnet.exe"
        self.ubt = self.engine / "Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.dll"
        self.editor = self.engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
        for filename in (self.dotnet, self.ubt, self.editor):
            filename.parent.mkdir(parents=True, exist_ok=True)
            filename.write_bytes(b"")
        version = self.engine / "Engine/Build/Build.version"
        version.parent.mkdir(parents=True)
        version.write_text(
            json.dumps({"MajorVersion": 5, "MinorVersion": 4, "PatchVersion": 4}),
            encoding="utf-8",
        )
        self.argv = [
            str(RUNNER_PATH), "--engine", str(self.engine), "--project", str(self.project),
            "--output", str(self.output), "--suite", "navigation",
        ]

    def fake_success(self, command: list[str], **kwargs: object) -> subprocess.CompletedProcess:
        self.assertEqual(kwargs["cwd"], self.project.parent.resolve())
        self.assertNotIn("shell", kwargs)
        self.assertEqual(kwargs["stderr"], subprocess.STDOUT)
        if command[0] == str(self.dotnet.resolve()):
            self.assertEqual(command[1], str(self.ubt.resolve()))
            self.assertEqual(command[2:5], ["NPCEditor", "Win64", "Development"])
            self.assertIn(f"-Project={self.project.resolve()}", command)
            self.assertEqual(kwargs["timeout"], 1800)
        else:
            self.assertEqual(command[0], str(self.editor.resolve()))
            self.assertEqual(command[1], str(self.project.resolve()))
            self.assertEqual(kwargs["timeout"], 240)
            self.assertIn(f"-NpcNavigationReport={self.output / 'navigation.json'}", command)
            self.assertIn("-nullrhi", command)
            report = self.output / "navigation-automation"
            report.mkdir()
            (report / "index.json").write_text(json.dumps(successful_report()), encoding="utf-8")
            (self.output / "navigation.json").write_text(
                json.dumps({"assertions_passed": True}), encoding="utf-8"
            )
        return subprocess.CompletedProcess(command, 0)

    def invoke(self, effect: object) -> tuple[int, object]:
        with patch.object(RUNNER.sys, "argv", self.argv), patch.object(
            RUNNER.subprocess, "run", side_effect=effect
        ) as mocked:
            result = RUNNER.main()
        return result, mocked

    def test_source_build_uses_bundled_dotnet_absolute_arguments_and_project_cwd(self) -> None:
        self.argv.extend(["--build", "--compiler-version", "14.38.33130"])
        result, mocked = self.invoke(self.fake_success)
        self.assertEqual(result, 0)
        self.assertEqual(mocked.call_count, 2)
        self.assertIn("-CompilerVersion=14.38.33130", mocked.call_args_list[0].args[0])
        summary = json.loads((self.output / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["status"], "passed")
        self.assertEqual([row["name"] for row in summary["results"]], ["source-build", "navigation"])

    def test_exit_zero_with_failed_automation_json_sets_failed_summary(self) -> None:
        def failed_report(command: list[str], **kwargs: object) -> subprocess.CompletedProcess:
            completed = self.fake_success(command, **kwargs)
            report = successful_report()
            report.update(succeeded=0, failed=1)
            report["tests"][0]["state"] = "Fail"
            (self.output / "navigation-automation/index.json").write_text(
                json.dumps(report), encoding="utf-8"
            )
            return completed

        result, _ = self.invoke(failed_report)
        self.assertEqual(result, 1)
        summary = json.loads((self.output / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["status"], "failed")
        self.assertEqual(summary["results"][0]["exit_code"], 0)
        self.assertIn("did not pass", summary["error"])

    def test_engine_timeout_is_reported_as_failure(self) -> None:
        result, mocked = self.invoke(subprocess.TimeoutExpired("inert-engine-fixture", 240))
        self.assertEqual(result, 1)
        self.assertEqual(mocked.call_args.kwargs["timeout"], 240)
        summary = json.loads((self.output / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["status"], "failed")
        self.assertIn("timed out", summary["error"])

    def test_existing_evidence_directory_is_never_overwritten(self) -> None:
        self.output.mkdir()
        marker = self.output / "keep.txt"
        marker.write_text("existing evidence", encoding="utf-8")
        with patch.object(RUNNER.sys, "argv", self.argv), patch.object(
            RUNNER.subprocess, "run"
        ) as mocked, self.assertRaises(FileExistsError):
            RUNNER.main()
        mocked.assert_not_called()
        self.assertEqual(marker.read_text(encoding="utf-8"), "existing evidence")
        self.assertFalse((self.output / "summary.json").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
