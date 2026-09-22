# Context-Aware NPC Interaction System

[English](README.md) | [한국어](README.ko.md)

An Unreal Engine 5 prototype exploring NPCs that respond to game state, follow daily routines, and connect relationships to gameplay.

**Engine:** Unreal Engine 5.4.4<br>
**Focus:** Blueprint systems, AI behavior, data-driven gameplay, and interaction design

## Project Overview

Many game NPCs repeat the same dialogue regardless of the player's condition, remain in one place throughout the day, and treat relationships as a number with little gameplay impact. This project addresses those limitations with a state-aware interaction system built around schedules, dialogue priorities, trust, quests, trading, and companion behavior.

The playable project uses deterministic, data-driven dialogue rules. A separate LLM feasibility experiment was conducted during development, but it was not integrated because the tested model did not follow gameplay constraints consistently.

## Key Features

| System | Implementation | Player-facing result |
| --- | --- | --- |
| Daily NPC routines | A 1,440-minute game clock controls NPC arrival, departure, and patrol behavior through Behavior Trees and Blackboards. | NPCs appear and move according to their schedules. |
| State-priority dialogue | Dialogue checks health, money, and trust before selecting a response. Critical states such as health at or below 30 or no money take priority over greetings. | Conversations reflect the player's current situation. |
| Trust and companions | Dialogue and quest outcomes update NPC-specific trust thresholds. | A trusted NPC can leave its patrol and follow the player. |
| Data-driven inventory and trading | Data Tables define items, quests, NPCs, and dialogue. Rare gathering items use a target spawn probability of 10%. | Content can be tuned without rebuilding the interaction logic. |
| Interaction targeting | Camera direction and a dot-product comparison select the intended target when several NPCs are nearby. | The player can interact more reliably in crowded spaces. |
| Player systems | Sprinting, stamina, first-/third-person switching, item pickup, inventory, and shop interfaces are included. | The NPC features are demonstrated in a complete gameplay loop. |

## System Flow

```text
Game clock ──> NPC schedule ──> Behavior Tree ──> spawn / patrol / follow
Player state ────────────────> dialogue priority ──> contextual response
Trust + quest state ─────────> trade / quest / companion availability
Data Tables ─────────────────> NPC, item, dialogue, and UI configuration
```

## Demo and Full Project

- [Download the complete playable project](https://drive.google.com/file/d/1qsdGAzKU3myKQVk04MfW-EngDHKKAqmW/view?usp=sharing)
- [View screencasts and supporting material](https://drive.google.com/drive/folders/1NyVitLN8_N_IztialbqUIZ3nomCIZ69q?usp=sharing)

To run the project:

1. Download and extract the complete project.
2. Open `NPC.uproject` in Unreal Engine 5.4.4.
3. Select **Yes** if Unreal Engine asks to rebuild missing modules.

The repository also includes a [compact package of the core assets](downloads/npc-core-assets.zip). It is provided for inspection and is not a standalone playable build.

## Controls

| Input | Action |
| --- | --- |
| `W` `A` `S` `D` | Move |
| `Left Shift` | Sprint |
| `V` | Toggle first-/third-person view |
| `E` | Interact, pick up an item, or close dialogue |
| `I` | Open inventory |
| `R` | Continue a conversation |
| `T` | Open the shop |
| `F` | Ask an NPC to follow or stop following |

## Validation

The project was checked with feature-level tests, scenario-based integration tests, and repeated probability measurements. Across 100 trials, quests appeared in 11% of observations and rare items in 14%; both were treated as plausible observations for systems configured with a 10% target probability. Test conditions and limitations are documented in Chapter 5 of the project report.

## Repository Guide

```text
.
├── docs/
│   ├── project-report.pdf
│   └── project-presentation.pptx
├── downloads/
│   └── npc-core-assets.zip
├── unreal-assets/
│   ├── Characters/
│   ├── Files/
│   ├── README.md
│   └── README.ko.md
├── .gitignore
├── README.md
└── README.ko.md
```

- [Project report](docs/project-report.pdf) — design rationale, system architecture, implementation, and testing
- [Presentation](docs/project-presentation.pptx) — final project presentation and Q&A material
- [Unreal asset index](unreal-assets/README.md) — a categorized guide to the included Blueprint and data assets

Daily notes, internal experiment records, generated Unreal files, and local configuration are intentionally excluded through `.gitignore` so the public repository stays focused on portfolio material.

## Scope and Next Steps

The current dialogue system is rule-based and designed for predictable gameplay behavior. A future version could evaluate a fine-tuned model or retrieval-augmented generation while retaining deterministic validation for health, money, quests, trust, and item transactions.

## Author

Dongmyung Park<br>
BSc Artificial Intelligence · Third Year Project · 2026
