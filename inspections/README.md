# Blueprint inspection files

[English](README.md) | [한국어](README.ko.md)

These are engine-exported code-review materials for binary Unreal assets. They expose actual graph nodes, pins, function references, defaults and links without replacing the game with a separate implementation. They are not personal development notes and do not execute the game.

Start with the [inspection guide](../docs/blueprint-inspection.md), [file hashes and provenance](index.json), and [NPC execution-link comparison](blueprints/npc-execution-changes.json).

| Folder | Contents |
| --- | --- |
| `blueprints/initial/` | All 18 named initial graph export files, representing 16 resolved Blueprint assets. |
| `blueprints/maintained/` | NPC graph after the retained dialogue-priority and schedule-interaction corrections. |
| `tables/initial/` | Original NPC, item and quest rows with their asset bindings. |
| `behavior-tree/` | Actual BT_NPC task-instance speed settings and separate class-default values. |
| `vendor/initial/` and `vendor/maintained/` | Original and corrected butterfly graph exports. |

`BP_Mushroom.json` resolves to `BP_Item_Master`; `WBP_DialogueEnd.json` resolves to `WBP_Dialogue`. The duplicated resolved exports preserve the initial filenames and are not separate algorithms.

Original export files are copied byte for byte unless a local filesystem path must be redacted. `index.json` records both export and published hashes and any such redaction. Unreal `/Game/` and `/Script/` paths are preserved because they identify assets and engine types. The original full archive and final source manifests remain the authoritative restoration inputs; graph JSON cannot restore a playable `.uasset`.

The initial baseline is the project restored from the pinned `Project.zip`.
