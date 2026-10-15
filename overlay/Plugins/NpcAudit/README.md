# NpcAudit editor bridge

This editor-only plugin inspects real reflected Unreal classes and dispatches their original functions. It does not contain a second implementation of the NPC rules. It does not save assets.

## Python API

After building NPCEditor and enabling the engine PythonScriptPlugin:

```python
import json
import unreal

player_bp = unreal.load_asset('/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter')
player_class = unreal.BlueprintEditorLibrary.generated_class(player_bp)
metadata = json.loads(unreal.NpcAuditLibrary.dump_class(player_class, True))
compiled = json.loads(unreal.NpcAuditLibrary.compile_blueprint(player_bp))

# Use an actual spawned actor from the test's game/PIE world for state tests.
# Obtain its exact property and function names from dump_object first.
state = json.loads(unreal.NpcAuditLibrary.dump_object(actor, True))
value = json.loads(unreal.NpcAuditLibrary.read_property(actor, exact_property_name))
write = json.loads(unreal.NpcAuditLibrary.write_property(actor, exact_property_name, json.dumps(new_value)))
call = json.loads(unreal.NpcAuditLibrary.invoke(actor, exact_function_name, json.dumps(arguments)))
```

All methods return a JSON string containing `schema`, `operation`, and `ok`. An input error returns `ok=false` and an error code. Inspect that field before using any result. Python calls are editor game-thread operations.

- `dump_class` lists each declared property and function once under its owning class, traversing superclasses when requested. Function parameters include exact names, reflected types, input/output/return roles, flags and available native defaults.
- `dump_object` adds the actual current property values. Object references are paths rather than recursive object expansion. 64-bit integer values are exported as strings to avoid double precision loss. Fields whose names indicate credentials are redacted.
- `dump_blueprint_graphs(blueprint_asset)` reads all actual graphs and exports graph/node GUIDs, node classes and titles, comments, positions, function references, pin types, defaults, and linked node/pin GUIDs. It has no row cap and reports counts, invalid references and a completeness flag. Local OS paths in text are redacted; Unreal asset paths remain. It does not compile, reconnect pins, modify nodes, or save the Blueprint.
- `read_property` returns metadata and value for one exact reflected property.
- `write_property` converts into temporary initialized property storage before copying into the actual object. A conversion failure leaves the destination property unchanged. Changes are in memory; no Modify, SavePackage or save-asset operation is performed. This is a fixture control, not proof that normal gameplay can make the same transition.
- `invoke` rejects unknown argument names and missing required inputs. It initializes parameter memory, calls the original `ProcessEvent`, and exports the resulting parameter values. It does not implement game rules itself.
- `compile_blueprint` calls the engine compiler with `SkipSave`, preserving compiler error and warning counts and messages.

## Scope of invocation

A successful invocation result means synchronous dispatch completed. It does not mean that gameplay assertions passed, a latent action finished, an RPC arrived, or the expected state change occurred. Functions marked with Latent metadata are rejected by the synchronous bridge. Custom events may internally schedule latent actions without that metadata; tick the actual world and observe subsequent state using an independent bounded test step.

Spawn original generated classes in the correct world and preserve their GameMode, local player/controller, Behavior Tree, Blackboard, navigation and UI prerequisites. Isolated calls without those prerequisites can create artificial Accessed None errors. Compare the actual state before and after the original function, and record the real engine's diagnostics.

Read/Write require exact reflected property names, which may include Blueprint-generated identifiers. Structured values should be read, modified and written back using their complete exported shape. Reference inputs accept a valid loaded object path or JSON null; class inputs are checked against the property's required base class.

The bridge deliberately performs no automatic function-name guessing, alias matching, reset, timer advancement, asset saving, or pass/fail promotion. Those decisions belong to the explicit scenario harness.

## Build scope

Module type: Editor. Public dependencies: Core, CoreUObject, Engine. Private dependencies: Json, JsonUtilities, UnrealEd, KismetCompiler, BlueprintGraph. The plugin uses only installed UE5.4.4 public headers. Runtime NPC source and Blueprint graphs are not rewritten by adding the plugin.
