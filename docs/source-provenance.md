# Source provenance and preservation

[English](source-provenance.md) | [한국어](source-provenance.ko.md)

The initial project is identified by one pinned complete archive. The maintained project is reconstructed by applying the declared source overlay to that archive. Original review assets, maintained playable assets and recorded execution results have separate roles.

## Pinned initial project

| Field | Recorded value |
| --- | --- |
| Original complete archive | [Project.zip on Google Drive](https://drive.google.com/file/d/1qsdGAzKU3myKQVk04MfW-EngDHKKAqmW/view?usp=sharing) |
| Archive size | 2,257,866,122 bytes |
| Archive SHA-256 | `ba8e4ba770461e5272ff6cfaeaf4be9fe0219ec3fa630a404572c627e8080bfe` |
| Declared original source files | 2,349 |
| Original files under `Content/` | 2,334 |
| Initial repository commit | `a3a518e62ac576dad6b9f586f0aa4fbb21b671ac` |

This archive, rather than an unpinned local project directory, defines the initial full-project baseline. Generated binaries, editor caches and saved session state are outside the declared source set. The restoration tool rejects an archive whose size or hash differs.

The thirty original public core assets were checked against both the pinned complete archive and the preserved core ZIP: **30 of 30 match**. The smaller core package supports source review. It does not contain everything required to restore and run the complete village map.

## Preserved original material

These links identify the initial repository revision, so the historical material remains distinguishable from maintained source:

| Material | Purpose |
| --- | --- |
| [Original project report](https://github.com/Dongmyung378/Context-Aware-Npc-System/blob/a3a518e62ac576dad6b9f586f0aa4fbb21b671ac/docs/project-report.pdf) | Original design, experiments and limitations |
| [Original presentation](https://github.com/Dongmyung378/Context-Aware-Npc-System/blob/a3a518e62ac576dad6b9f586f0aa4fbb21b671ac/docs/project-presentation.pptx) | Original project presentation |
| [Original core asset ZIP](https://github.com/Dongmyung378/Context-Aware-Npc-System/blob/a3a518e62ac576dad6b9f586f0aa4fbb21b671ac/downloads/npc-core-assets.zip) | Preserved thirty-asset review package |
| [Original core asset tree](https://github.com/Dongmyung378/Context-Aware-Npc-System/tree/a3a518e62ac576dad6b9f586f0aa4fbb21b671ac/unreal-assets) | Individually inspectable initial assets |

The canonical repository keeps these original artifacts. The compact source bundle references them rather than embedding another copy of the report, presentation and original core ZIP. Review the original `unreal-assets` material as initial source; use the complete archive plus maintained overlay for current execution.

## Maintained source overlay

Source bundle version `1` declares **29 overlay files: 7 replacements and 22 additions**. Applying it to the 2,349-file original source set produces **2,371 source files**. The additions are the editor-only `NpcAudit` plugin and its verification source. No original source file is removed.

| Replaced source | Maintained scope |
| --- | --- |
| `Content/ThirdPerson/Blueprints/BP_NPC_Standard.uasset` | Critical ordinary-dialogue priority and the interaction guard on a new home intent |
| `Content/AdvancedVillagePack/Blueprints/BP_Butterfly.uasset` | Vendor Blueprint construction compatibility |
| `Content/ThirdPerson/Maps/ThirdPersonMap.umap` | Map state associated with the reviewed navigation repair |
| `Content/__ExternalActors__/ThirdPerson/Maps/ThirdPersonMap/6/Y8/B5W8N6X01MFMWF4ZO4QWYL.uasset` | Saved Recast navigation data for the map |
| `Config/DefaultEngine.ini` | Editor environment configuration, including disabling Android File Server network startup |
| `Config/DefaultGame.ini` | Disable repeated startup insertion of StarterContent |
| `NPC.uproject` | Enable the editor tools used for inspection and native verification |

The original Data Tables and `BT_NPC` remain byte-identical to the pinned archive. Their item values, NPC schedules, trust thresholds, quest rows and authored tree settings are preserved. The embedded chase task uses speed 150 even though its class default is 300; the verification oracle reads the authored task instance. No follow-speed adjustment is retained.

The reviewed lookup-consolidation candidate was declined. Its result and source tooling remain inspectable, while the maintained NPC asset retains the original lookup structure. [Blueprint inspection](blueprint-inspection.md) identifies the graph exports, table rows and task-instance evidence without treating a binary asset as a text diff.

The bundle's `manifests/original-project.json`, `manifests/overlay.json` and `manifests/final-project.json` provide file identity before and after restoration. The overlay records each payload hash and each replaced file's original hash. They are authoritative for the complete file set; this page explains the change categories.

## Restoration and execution evidence

Download the pinned full archive manually, then use the compact bundle's verification and restoration tools. [The reproduction guide](reproduce.md) gives the Windows Unreal Engine 5.4.4 build and native check commands. It restores into a new directory and verifies all 2,371 final source files before relying on generated project binaries.

The recorded fresh restoration was followed by an actual source build and native editor checks. [The fresh-restoration run](../reports/fresh-restoration/run.json) records the runner's build check, Blueprint preflight and seven gameplay suites. Read each linked report and its assertions; an editor process exit code alone does not establish gameplay success. Build timing depends on whether work is cold or already up to date, so the runner's later build-check duration should not be described as a cold compilation measurement.

The engine evidence comes from native Windows execution on the maintainer's machine. Python restoration-tool checks on Windows and Linux cover a different layer. Neither those tool tests nor a fresh directory establishes independent-machine certification or Linux Unreal gameplay support. [The report guide](../reports/README.md) explains the scenario boundaries and the distinction between file verification, game behavior and profiling.
