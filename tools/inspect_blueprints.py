"""Run inside UE 5.4.4 via -ExecutePythonScript; compile without saving assets."""
import json
from pathlib import Path
import re
import traceback

import unreal

command_line = unreal.SystemLibrary.get_command_line()
match = re.search(
    r'(?i)(?:"-NpcBlueprintReport=([^"]+)"|-NpcBlueprintReport="([^"]+)"|-NpcBlueprintReport=(\S+))',
    command_line,
)
if not match:
    raise RuntimeError("Specify -NpcBlueprintReport=<new output JSON path>")
output = Path(next(value for value in match.groups() if value is not None))
if output.exists():
    raise RuntimeError("Refusing to overwrite an existing audit report")
output.parent.mkdir(parents=True, exist_ok=True)
result = {
    "schema": "npc-project-editor-audit/v1",
    "scope": "Every /Game Blueprint asset, editor compile with SkipSave; not a gameplay coverage claim",
    "engine_version": unreal.SystemLibrary.get_engine_version(),
    "assets": [],
    "errors": [],
}
try:
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.search_all_assets(True)
    for entry in registry.get_assets_by_path("/Game", recursive=True):
        if str(entry.asset_class_path.asset_name) not in {
            "Blueprint", "WidgetBlueprint", "AnimBlueprint", "EditorUtilityBlueprint",
        }:
            continue
        row = {"path": str(entry.package_name), "asset_class": str(entry.asset_class_path.asset_name)}
        try:
            asset = entry.get_asset()
            row["loaded"] = asset is not None
            row["compile"] = json.loads(unreal.NpcAuditLibrary.compile_blueprint(asset))
        except Exception:
            row["error"] = traceback.format_exc()
        result["assets"].append(row)
except Exception:
    result["errors"].append(traceback.format_exc())
finally:
    result["asset_count"] = len(result["assets"])
    result["expected_asset_count"] = 46
    result["assertions_passed"] = not result["errors"] and len(result["assets"]) == 46 and all(
        row.get("loaded") is True and not row.get("error")
        and row.get("compile", {}).get("ok") is True
        and row["compile"].get("errors") == 0
        and row["compile"].get("warnings") == 0
        for row in result["assets"]
    )
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    unreal.log("NPC_ALL_BLUEPRINTS_COMPLETE " + str(result["asset_count"]))
