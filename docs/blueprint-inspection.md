# Inspect the Blueprint implementation

[English](blueprint-inspection.md) | [한국어](blueprint-inspection.ko.md)

The [inspection files](../inspections/README.md) expose the binary Unreal implementation for code review. They contain actual node GUIDs, function references, typed pins, defaults and connections read by Unreal Engine 5.4.4. They are native implementation records, rather than personal work logs or a second implementation of the NPC rules.

Use [reproduction](reproduce.md) to restore the complete game and [results](results.md) for executed checks. Static graph exports establish what was inspected; actual PIE reports establish which behavior was exercised.

## Files and provenance

| Material | Review use |
| --- | --- |
| [Index and hashes](../inspections/index.json) | Published and original export identity, stages and aliases. |
| [Initial NPC graph](../inspections/blueprints/initial/BP_NPC_Standard.json) | Initial interaction, dialogue, schedule and follow implementation. |
| [Maintained NPC graph](../inspections/blueprints/maintained/BP_NPC_Standard.json) | Retained dialogue-priority and schedule-interaction corrections. |
| [Execution-link comparison](../inspections/blueprints/npc-execution-changes.json) | Execution links added or removed, identified by stable node GUIDs and pin names. |
| [Player graph](../inspections/blueprints/initial/BP_ThirdPersonCharacter.json) | Input mapping, target selection and stamina handling. |
| [Item graph](../inspections/blueprints/initial/BP_Item_Master.json) | Rarity choice, overlap-enabled E input, inventory updates and respawn. |
| [Shop graph](../inspections/blueprints/initial/WBP_ShopMain.json) | Original catalog population and display filtering. |
| [BT task settings](../inspections/behavior-tree/BT_NPC-task-settings.json) | Actual task-instance settings, distinct from class defaults. |
| [Vendor comparison](../inspections/vendor/butterfly-construction-change.json) | Initial and corrected butterfly construction graphs and preservation checks. |

The initial folder preserves all 18 named exports, representing 16 resolved Blueprint assets. `BP_Mushroom` resolves to `BP_Item_Master`, and `WBP_DialogueEnd` resolves to `WBP_Dialogue`. Their identical exports retain the initial filenames and do not represent separate algorithms. These are selected core graphs, not every project or vendor Blueprint. The separately exported initial butterfly graph has its own before/after folders.

The initial complete-project dependency is pinned to:

```text
Archive: Project.zip
Bytes:   2257866122
SHA-256: ba8e4ba770461e5272ff6cfaeaf4be9fe0219ec3fa630a404572c627e8080bfe
```

Download it through the [restoration guide](reproduce.md#what-to-download). The release's `manifests/original-project.json` and `manifests/final-project.json` identify initial and final source files. The initial baseline is restored from that pinned archive. Inspection JSON has separate hashes in the index; it cannot replace a `.uasset` or the full archive.

The 18 initial exports and two maintained exports are copied byte for byte. The index records any local-path redaction separately; this set required none. Unreal `/Game/` and `/Script/` paths remain because they identify assets and engine types. The export field `asset_saved: false` means that exporting did not save an asset. It does not mean a maintained asset was never saved by the separate maintenance procedure.

## Read the graph records

Start with `graphs[].name`, then find a node using `nodes[].guid` or `title`. Each pin records its direction, type, literal default and linked node/pin identities. `function_reference` identifies called functions where available. Follow output execution pins to read a control-flow branch, and inspect its connected data pins before interpreting the condition or value.

The link summary compares execution edges using stable node GUIDs and pin names. It does not compare every data wire, layout coordinate or serialized pin ID. The complete exports retain those details. `complete: true` and zero invalid links concern export integrity, not exhaustive gameplay correctness.

## Authored Data Tables

| Table | Rows | Contents |
| --- | ---: | --- |
| [DT_NPC_Data](../inspections/tables/initial/DT_NPC_Data.json) | 5 | Names, schedules, home locations, initial trust and follow thresholds. |
| [DT_Item_Data](../inspections/tables/initial/DT_Item_Data.json) | 38 | Item types, quantities, prices, effects, trust requirements and presentation references. |
| [DT_QuestList](../inspections/tables/initial/DT_QuestList.json) | 15 | Collection or gold requirements and authored rewards. |

These extracts preserve the original row values, including Unreal export-text strings such as `NSLOCTEXT`. Such strings are serialized text, rather than broken Markdown. Each extract identifies its original asset and hash. Direct archive-entry checks confirm that all three table asset hashes match the maintained source. Preparing these inspection files did not edit a table or its defaults.

| NPC | Start minute | End minute | Follow trust threshold |
| --- | ---: | ---: | ---: |
| Charles | 420 | 1140 | 80 |
| George | 330 | 1050 | 40 |
| Leon | 390 | 1110 | 60 |
| Lucia | 0 | 720 | 60 |
| Rose | 420 | 1140 | 20 |

The active interval is `Start_Time <= CurrentTime < End_Time`. The clock wraps modulo 1,440 and preserves fractional overshoot. These five rows configure a shared implementation. George's authored initial trust is 39.

## Implementation details that affect interpretation

**Target direction.** `SendInteractionSignal` selects among actual NPC overlaps using the player's Actor-forward direction and `GetDotProductTo`. It does not use the camera-forward vector. Exact score ties and an additional front-only cutoff are not authored guarantees.

**Stamina.** Low-state dialogue reads `Current_Stamina <= 30`, rather than a separate health variable. Movement slows below 20. Values from 20 through 30 can therefore produce normal movement speed and a fatigue response together.

**Pickup lifecycle.** Overlap with the player enables the item Actor's original E binding. Successful pickup adds or increments inventory, hides the Actor and disables collision. The original 60-second Delay restores visibility and collision and re-evaluates rarity. Pickup does not destroy the Actor.

**Shop filtering.** Charles's `ItemsToSell` includes Juice, Beer, Whiskey and Titan Blood. `NPC_SalesList` retains all four catalog rows. `WBP_ShopMain.UpdateShopUI` filters the generated `NPC_ShopList` display rows using `Required_Trust`. Trust 99 displays three ordinary items; 100 and 101 also display Titan Blood. A four-row catalog at trust 99 is compatible with this display gate.

## Class defaults and task-instance settings

The [compact BT inspection](../inspections/behavior-tree/BT_NPC-task-settings.json) records task object paths, classes, parent composites and parameters.

| Setting | Blueprint class default | Task instance in BT_NPC |
| --- | ---: | ---: |
| `BTTask_ChasePlayer.ChaseSpeed` | 300 | 150 |
| `BTTask_PatrolRandom.Patrol Speed` | 150 | 150 |
| `BTTask_PatrolRandom.PatrolRadius` | 1500 | 1500 |

The existing `Update Walk Speed(NewSpeed)` assigns its argument to movement speed. It does not choose 300 or 150 from `IsFollowing`. The Chase task passes its instance parameter, so the actual tree can follow correctly at 150 despite a class default of 300.

A navigation test initially used the class default as its expected chase speed. Actual movement and task inspection exposed that oracle error. The maintained test reads authored tasks through `BT_NPC.RootNode` and `Children.ChildTask`, then compares the live Brain `NodeInstances` parameters. Distance, path and time requirements remain separate. No speed-setting gameplay patch was adopted. The initial and maintained `BT_NPC.uasset` hashes match.

## Retained graph changes

In the initial no-active-quest Talk branch, the new-quest random roll preceded ordinary stamina and money checks. The maintained graph checks low stamina, then no money, before the original new-quest roll. Existing active-quest handling stays first and retains its requirements and rewards.

The schedule correction adds one `IsInteracting` branch before issuing an off-work home intent. It preserves NPC hours and downstream movement. It prevents a new scheduled home intent while an interaction is active; it does not promise to cancel every earlier movement request. The [execution-link comparison](../inspections/blueprints/npc-execution-changes.json) records four removed edges, five added edges and guard node `15C20261-0C15-4F2A-AB80-1D0180000001`.

The [butterfly comparison](../inspections/vendor/butterfly-construction-change.json) documents the construction-safe replacement of `PlayAnimation` and `SetPlayRate` with `USkeletalMeshComponent::OverrideAnimationData`. Original animation selection and the rate expression remain, and the unrelated graph digest matches before and after. The recorded in-memory compile had zero errors and zero warnings. Visual and runtime animation behavior require their own actual-engine evidence.

These records accompany the maintained source and manifests. Use the editor and verification suites to assess movement, input, UI and execution behavior beyond static inspection.
