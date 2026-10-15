using UnrealBuildTool;

public class NpcAudit : ModuleRules
{
    public NpcAudit(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new[] { "Json", "JsonUtilities", "UnrealEd", "KismetCompiler", "BlueprintGraph", "AIModule", "NavigationSystem", "UMG", "Slate", "SlateCore", "RHI", "InputCore" });
    }
}
