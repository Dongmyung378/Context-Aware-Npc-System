#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NpcVendorPatchLibrary.generated.h"

/** Narrow, reviewed vendor compatibility fixes. No asset is saved by this library. */
UCLASS()
class NPCAUDIT_API UNpcVendorPatchLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /** Replace only the reviewed unsafe butterfly construction animation chain.
     * Existing animation selection, rate expression and all other graphs are retained.
     * Compiles with SkipSave and reports warnings as failure; disk saving is separate.
     */
    UFUNCTION(BlueprintCallable, Category="NPC Audit|Maintenance")
    static FString CorrectButterfly(UObject* BlueprintAsset);
};
