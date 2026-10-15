#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NpcBlueprintPatchLibrary.generated.h"

/** Auditable editor maintenance. Changes are in memory until an explicit asset save. */
UCLASS()
class NPCAUDIT_API UNpcBlueprintPatchLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="NPC Maintenance")
    static FString CorrectNpcBlueprint(UObject* BlueprintAsset);
};
