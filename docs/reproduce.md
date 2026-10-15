# Reproduce the project

[English](reproduce.md) | [한국어](reproduce.ko.md)

This guide restores the complete original project, applies source bundle version `1`, rebuilds the C++ modules and runs the real Unreal Engine checks. Use a separate project directory so the downloaded original archive and any existing work remain intact.

## What to download

| Download | Purpose |
| --- | --- |
| Current source bundle | Maintained overlay files, verification plugin source, restoration tools, manifests and documentation. Extract the bundle so `bundle.json` and `tools/` are at its root. |
| [Original complete `Project.zip`](https://drive.google.com/file/d/1qsdGAzKU3myKQVk04MfW-EngDHKKAqmW/view?usp=sharing) | The separate full-project dependency containing the map, external actor packages and content assets. Download it manually from Drive. |
| [Historical core asset ZIP](https://github.com/Dongmyung378/Context-Aware-Npc-System/blob/main/downloads/npc-core-assets.zip) | The preserved thirty-asset inspection package on GitHub. Keep it as original evidence; full restoration uses the complete project archive. |

The current source download is an overlay and tooling package. The approximately 2.26 GB original archive remains a separate download. The original report and presentation also remain available as historical material.

The exact audited full archive is:

```text
Filename: Project.zip
Bytes:    2257866122
SHA-256:  ba8e4ba770461e5272ff6cfaeaf4be9fe0219ec3fa630a404572c627e8080bfe
```

The restoration tool checks both archive size and hash, then verifies source files and the final overlay against the manifests. A different project snapshot is rejected rather than silently substituted.

## Requirements and recorded environment

| Requirement | Recorded configuration |
| --- | --- |
| Engine | Unreal Engine 5.4.4 |
| Actual game execution | Windows with the native Unreal Editor and command-line editor |
| C++ build | Visual Studio 2022 with the C++ toolchain |
| Compiler selection | MSVC installation folder `14.38.33130`; recorded compiler build `14.38.33145` |
| Windows SDK | `10.0.26100.0` |
| Restoration and runner | Python 3.10 or later, standard library only |
| Rendered checks and profiling | A working DirectX rendering environment; recorded profile configuration uses D3D12 at 1280 × 720 |

The MSVC folder identifier and compiler build identifier describe different parts of the installed toolchain. The runner's `--compiler-version` argument selects the installation folder. Use `14.38.33130` for the recorded installation.

Install the engine and C++ tools before starting the build. Allow additional disk space for generated binaries, shader data and editor caches. The procedure uses the native Unreal installation directly and does not require Docker. Generating optional documentation figures uses Matplotlib separately; restoration and game-check scripts need no external Python packages.

## Verify and restore into a new folder

The following PowerShell examples assume:

- The source bundle has been extracted to `D:/NPC-Source`.
- The original archive is at `D:/Downloads/Project.zip`.
- `D:/NPC-Verify` does not exist. Its parent `D:/` must already exist.

Open PowerShell at the source bundle root:

```powershell
Set-Location "D:/NPC-Source"
python --version
python -X utf8 -B tools/verify_bundle.py --expected-source-version 1
```

Bundle verification checks the declared source, metadata and overlay hashes. Its JSON explicitly records that it has not executed the archive or the game.

Restore the complete project:

```powershell
python -X utf8 -B tools/restore_project.py --archive "D:/Downloads/Project.zip" --output "D:/NPC-Verify" --expected-source-version 1
```

The tool validates the original archive, extracts the declared source into a sibling staging directory, applies the verified overlay, checks the complete final file set and publishes the new project directory. Successful output contains `status: passed_source_restoration`, file counts and the original archive hash. This establishes restored file identity; the engine checks below establish execution behavior.

An existing output is rejected even when empty. Choose another new path for another restoration. On a failed extraction or overlay check, a staging directory may be retained and reported for inspection. The tool does not overwrite an existing user project.

## Build and run the actual engine checks

Close the editor for this restored project before rebuilding it. From `D:/NPC-Source`, run:

```powershell
python -X utf8 -B tools/run_unreal_checks.py --engine "D:/Epic Games/UE_5.4" --project "D:/NPC-Verify/NPC.uproject" --output "D:/NPC-Checks" --build --compiler-version 14.38.33130
```

Change the engine path if your 5.4.4 installation is elsewhere. `D:/NPC-Checks` must be a new output directory. The runner builds `NPCEditor` from source using the engine's bundled .NET SDK and UnrealBuildTool. The default selection `all` then runs `inspect_blueprints.py` in the actual editor before the seven gameplay suites. It loads and compiles the 46 project Blueprints under `/Game` using `SkipSave`, requires zero errors and zero warnings, and writes `blueprints.json`. The selected gameplay suites subsequently run in the actual command-line editor.

| Runner selection | Native automation suite | Scope |
| --- | --- | --- |
| `world` | `NPC.Gameplay.WorldInventory` | Original map, possessed player and original NPC instances |
| `states` | `NPC.Gameplay.OriginalContracts` | Clock, dialogue boundaries, schedule, follow and related state contracts |
| `interactions` | `NPC.Gameplay.InteractionContracts` | Target selection matrix, actual UMG transactions, food and item-selection sampling |
| `quests` | `NPC.Gameplay.QuestContracts` | Actual quest requirements and reward state changes |
| `probability` | `NPC.Gameplay.QuestProbability` | Conditional quest-proposal calls with explicit fixture and sample count |
| `navigation` | `NPC.Gameplay.NavigationContracts` | The actual navigation and movement checks implemented by the released suite |
| `inventory-input` | `NPC.Gameplay.InventoryInputContracts` | Original inventory input and UI lifecycle checks |

The default game checks use NullRHI for state execution. Inspect the actual automation logs and JSON for results. The runner requires successful automation records and `assertions_passed: true`; an Unreal process returning exit code zero alone is insufficient. An incomplete report, failed assertion or failed process results in a failed runner summary.

The output contains `summary.json`, per-suite JSON, engine logs, console logs and automation export directories. These records distinguish successful setup from successful gameplay assertions. Keep the full output when reporting a failure.

The `inventory-input` suite includes a wait for the original 60-second item respawn, so that check can take over a minute. Selecting one gameplay suite skips the `all` Blueprint preflight; the complete default run includes it.

To rerun one suite after a completed build, select it and use a new output path:

```powershell
python -X utf8 -B tools/run_unreal_checks.py --engine "D:/Epic Games/UE_5.4" --project "D:/NPC-Verify/NPC.uproject" --output "D:/NPC-Checks-Interactions" --suite interactions
```

## Rendered checks and optional profiling

Use the actual rendering path for a separate rendered run:

```powershell
python -X utf8 -B tools/run_unreal_checks.py --engine "D:/Epic Games/UE_5.4" --project "D:/NPC-Verify/NPC.uproject" --output "D:/NPC-Checks-Rendered" --rendered
```

This uses offscreen rendered PIE at 1280 × 720. The actual recorded RHI and viewport values determine the rendering conditions; review them rather than inferring GPU execution from a state-test pass.

For the additional NPC workload measurements:

```powershell
python -X utf8 -B tools/run_unreal_checks.py --engine "D:/Epic Games/UE_5.4" --project "D:/NPC-Verify/NPC.uproject" --output "D:/NPC-Checks-Profile" --profile --repetitions 3
```

The profile path uses rendered execution, 10 seconds of warmup and 15 seconds of capture for 5, 20 and 50 NPCs, repeated three times. Review the recorded RHI, viewport, controller/brain counts and movement/path evidence. Some NPCs may be idle by design; movement observations have a stated denominator and do not imply that every NPC walks continuously.

World actor-tick spans include all actors and intervening world work. Editor end-frame intervals include editor, PIE, rendering waits and instrumentation. These metrics are not standalone packaged-game FPS. Report the three run medians and their variation, and compare runs only when their source, navigation state and workload match. A command description is a reproducible procedure, not a performance guarantee or a claim that a particular run passed.

## Open and inspect the full game

After restoration and a successful source build, open `D:/NPC-Verify/NPC.uproject` in Unreal Engine 5.4.4. Use `/Game/ThirdPerson/Maps/ThirdPersonMap` and start Play in Editor.

| Input | Action |
| --- | --- |
| `W` `A` `S` `D` | Move |
| `Left Shift` | Sprint |
| `V` | Switch view |
| `E` | Interact with an NPC, pick up an item or close dialogue as appropriate |
| `R` | Talk or continue the conversation |
| `T` | Open trade |
| `F` | Request or stop following when the NPC's conditions permit |
| `I` | Open or close the inventory |

This manual run complements automated state and delegate tests with direct inspection of the scene, camera, UI, animation and input. The original five-NPC loop, schedules, dialogue, trust, quests, trading and following remain the project features to inspect.

## What the environment evidence means

The restoration tools have been exercised with controlled archive and overlay fixtures on Windows and Linux, including Ubuntu under WSL. That evidence covers the Python tools' validation and restoration behavior; it does not mean the complete original archive and game were executed on Linux. It does not certify Linux Unreal gameplay, Linux graphics, or a Linux C++ game build. The game runner explicitly targets the recorded Windows Unreal 5.4.4 environment.

A fresh restoration directory and fresh source build on the maintainer's PC remove reliance on copied project binaries. They are still same-machine evidence, not certification from an independent computer or a complete virtual machine. The archive identity, manifests, source build logs and actual engine reports let another user repeat the procedure and inspect any differences in their own environment.

Version or hash mismatch, missing C++ tools, an engine version other than 5.4.4, missing game content, invalid navigation data and failed gameplay assertions are distinct failure categories. Preserve their logs and correct the identified prerequisite or source issue. Do not treat a file-verification pass as a substitute for the corresponding engine run.
