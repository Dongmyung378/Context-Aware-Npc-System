"""Run real Unreal tests and reject failed JSON results even if Unreal exits zero."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

SUITES = {
    "world": ("NPC.Gameplay.WorldInventory", "NpcAuditReport"),
    "states": ("NPC.Gameplay.OriginalContracts", "NpcAuditReport"),
    "interactions": ("NPC.Gameplay.InteractionContracts", "NpcInteractionReport"),
    "quests": ("NPC.Gameplay.QuestContracts", "NpcQuestReport"),
    "probability": ("NPC.Gameplay.QuestProbability", "NpcQuestProbabilityReport"),
    "navigation": ("NPC.Gameplay.NavigationContracts", "NpcNavigationReport"),
    "inventory-input": ("NPC.Gameplay.InventoryInputContracts", "NpcInventoryInputReport"),
}


def read_json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict):
        raise RuntimeError(f"Expected a JSON object in {path}")
    return value


def check_automation(report_dir: Path, expected_test: str) -> dict:
    """The engine's exit code alone does not reliably report automation failures."""
    report = read_json(report_dir / "index.json")
    tests = report.get("tests")
    counters = ("succeeded", "succeededWithWarnings", "failed", "notRun", "inProcess")
    valid_counters = all(type(report.get(key)) is int and report[key] >= 0 for key in counters)
    matching = ([test for test in tests if isinstance(test, dict) and test.get("fullTestPath") == expected_test]
                if isinstance(tests, list) else [])
    if (not valid_counters or not isinstance(tests, list) or len(tests) != 1 or len(matching) != 1
        or matching[0].get("state") != "Success"
        or report["succeeded"] + report["succeededWithWarnings"] != 1
        or any(report[key] != 0 for key in ("failed", "notRun", "inProcess"))):
        raise RuntimeError(f"Unreal automation did not pass {expected_test}; inspect {report_dir}")
    return {key: report.get(key) for key in ("succeeded", "succeededWithWarnings", "failed", "notRun")}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path, help="UE_5.4 installation root")
    parser.add_argument("--project", required=True, type=Path, help="Restored NPC.uproject")
    parser.add_argument("--output", required=True, type=Path, help="A new directory, never overwritten")
    parser.add_argument("--suite", choices=["all", *SUITES], default="all")
    parser.add_argument("--build", action="store_true", help="Compile from source before testing")
    parser.add_argument("--compiler-version", help="Optional installed MSVC toolset folder version")
    parser.add_argument("--rendered", action="store_true", help="Use DirectX rendering instead of NullRHI")
    parser.add_argument("--profile", action="store_true", help="Also profile 5, 20 and 50 actual NPCs")
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Actual game checks are supported and validated on Windows with UE 5.4.4")
    args.engine = args.engine.resolve(strict=True)
    args.project = args.project.resolve(strict=True)
    if args.project.name != "NPC.uproject":
        parser.error("Expected the restored NPC.uproject")
    version = read_json(args.engine / "Engine/Build/Build.version")
    if tuple(version.get(key) for key in ("MajorVersion", "MinorVersion", "PatchVersion")) != (5, 4, 4):
        parser.error("Use Unreal Engine 5.4.4 to match the recorded validation")
    editor = args.engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
    if not editor.is_file():
        parser.error("UnrealEditor-Cmd.exe is missing from the specified installation")
    if not 1 <= args.repetitions <= 5:
        parser.error("--repetitions must be between 1 and 5")
    args.output = args.output.absolute()
    args.output.mkdir(parents=True, exist_ok=False)
    summary = {"schema": "npc-unreal-check-run/v1", "engine_version": version,
               "scope": "Actual Windows Unreal source build and PIE checks; not an independent-machine certification",
               "project": str(args.project), "results": [], "status": "running"}

    def checkpoint():
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    def execute(name: str, command: list[str], timeout: int) -> dict:
        started = time.monotonic()
        print(f"Running {name}", flush=True)
        with (args.output / f"{name}-console.log").open("w", encoding="utf-8") as stream:
            process = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                     timeout=timeout, cwd=args.project.parent)
        row = {"name": name, "command": command, "exit_code": process.returncode,
               "elapsed_seconds": time.monotonic() - started}
        summary["results"].append(row)
        checkpoint()
        if process.returncode:
            raise RuntimeError(f"{name} exited {process.returncode}; inspect its console log")
        return row

    try:
        checkpoint()
        if args.build:
            dotnet = args.engine / "Engine/Binaries/ThirdParty/DotNet/6.0.302/windows/dotnet.exe"
            ubt = args.engine / "Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.dll"
            if not dotnet.is_file() or not ubt.is_file():
                raise RuntimeError("The UE 5.4.4 bundled .NET SDK or UnrealBuildTool is missing")
            command = [str(dotnet), str(ubt), "NPCEditor", "Win64", "Development",
                       f"-Project={args.project}", "-WaitMutex", "-NoHotReloadFromIDE"]
            if args.compiler_version:
                command.append(f"-CompilerVersion={args.compiler_version}")
            execute("source-build", command, 1800)
        if args.suite == "all":
            inspection_script = Path(__file__).with_name("inspect_blueprints.py").resolve(strict=True)
            report = args.output / "blueprints.json"
            row = execute("blueprints", [str(editor), str(args.project),
                          f"-ExecutePythonScript={inspection_script}", f"-NpcBlueprintReport={report}",
                          "-unattended", "-nullrhi", "-nosplash", "-nosound", "-nop4", "-stdout",
                          "-FullStdOutLogOutput", f"-abslog={args.output / 'blueprints.log'}"], 240)
            if read_json(report).get("assertions_passed") is not True:
                raise RuntimeError("Project Blueprint compilation failed; inspect blueprints.json")
            row["report"] = report.name
            checkpoint()
        selected = SUITES if args.suite == "all" else {args.suite: SUITES[args.suite]}
        for name, (suite, parameter) in selected.items():
            report = args.output / f"{name}.json"
            automation = args.output / f"{name}-automation"
            command = [str(editor), str(args.project), "-unattended", "-nosplash", "-nosound", "-nop4",
                       "-stdout", "-FullStdOutLogOutput", f"-ExecCmds=Automation RunTests {suite}",
                       "-TestExit=Automation Test Queue Empty", f"-{parameter}={report}",
                       f"-ReportExportPath={automation}", f"-abslog={args.output / (name + '.log')}"]
            command += ["-RenderOffscreen", "-windowed", "-ResX=1280", "-ResY=720", "-novsync"] if args.rendered else ["-nullrhi"]
            row = execute(name, command, 240)
            row["automation"] = check_automation(automation, suite)
            state = read_json(report)
            if state.get("assertions_passed") is not True:
                raise RuntimeError(f"{name} generated failed or incomplete gameplay assertions")
            row["report"] = report.name
            checkpoint()
        if args.profile:
            for repetition in range(1, args.repetitions + 1):
                counts = (5, 20, 50) if repetition % 2 else (50, 20, 5)
                for count in counts:
                    name = f"profile-{count}-r{repetition}"
                    report = args.output / f"{name}.json"
                    automation = args.output / f"{name}-automation"
                    command = [str(editor), str(args.project), "-unattended", "-RenderOffscreen", "-windowed",
                               "-ResX=1280", "-ResY=720", "-novsync", "-nosplash", "-nosound", "-nop4",
                               "-stdout", "-FullStdOutLogOutput",
                               "-ExecCmds=t.MaxFPS 0,Slate.bAllowThrottling 0,Automation RunTests NPC.Profile.Capture",
                               "-TestExit=Automation Test Queue Empty", f"-NpcProfileCount={count}",
                               "-NpcProfileWarmup=10", "-NpcProfileSeconds=15", f"-NpcProfileReport={report}",
                               f"-ReportExportPath={automation}", f"-abslog={args.output / (name + '.log')}"]
                    row = execute(name, command, 180)
                    row["automation"] = check_automation(automation, "NPC.Profile.Capture")
                    state = read_json(report)
                    if state.get("assertions_passed") is not True:
                        raise RuntimeError(f"{name} did not maintain its required active NPCs and samples")
                    if (state.get("actual_game_viewport_width"), state.get("actual_game_viewport_height")) != (1280, 720):
                        raise RuntimeError(f"{name} did not render at the declared viewport dimensions")
                    row["report"] = report.name
                    checkpoint()
        summary["status"] = "passed"
        checkpoint()
        print(f"Passed. Evidence: {args.output / 'summary.json'}")
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        summary["status"] = "failed"
        summary["error"] = str(exc)
        checkpoint()
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
