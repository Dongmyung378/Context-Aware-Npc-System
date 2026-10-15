#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NpcTickOptimizationLibrary.generated.h"

/** Reviewed editor-only candidate. Never saves the Blueprint asset. */
UCLASS()
class NPCAUDIT_API UNpcTickOptimizationLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /** Share one GameMode lookup/cast across the original three ordered Tick branches. */
    UFUNCTION(BlueprintCallable, Category="NPC Maintenance")
    static FString OptimizeTickLookup(UObject* BlueprintAsset);
};
