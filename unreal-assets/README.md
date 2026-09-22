# Unreal Asset Guide

[English](README.md) | [한국어](README.ko.md) | [Project overview](../README.md)

This directory exposes the core Unreal Engine 5.4.4 assets from the submitted project package. The assets are included to make the implemented systems and naming structure visible in the portfolio repository; they are not a standalone playable project.

## Asset Groups

| Group | Representative assets | Purpose |
| --- | --- | --- |
| Characters | `BP_NPC_Standard`, `BP_ThirdPersonCharacter` | NPC behavior and player interaction state |
| AI | `BB_NPC`, `BT_NPC`, `BTTask_*`, `BTD_RandomChance`, `My_AIController` | Blackboard state, patrol/follow tasks, and behavior selection |
| Gameplay | `BP_GameMode`, `BPI_Interaction`, `BP_Item_Master`, `BP_Mushroom` | Shared interaction, item, and game-state logic |
| Data | `DT_*`, `ST_*`, `E_*` | NPC, dialogue, quest, and item definitions |
| UI | `WBP_*` | Dialogue, inventory, shop, pickup, and HUD widgets |

## Notes

- The original directory layout is preserved from the submitted core-assets archive.
- Binary `.uasset` files require Unreal Engine and may depend on paths or assets from the complete project.
- Use the [complete project download](https://drive.google.com/file/d/1qsdGAzKU3myKQVk04MfW-EngDHKKAqmW/view?usp=sharing) for the playable version.
- The matching packaged archive is available at [`downloads/npc-core-assets.zip`](../downloads/npc-core-assets.zip).
