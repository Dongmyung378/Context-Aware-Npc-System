#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NpcAuditLibrary.generated.h"

/** Editor-only bridge. Operates on actual reflected classes; never saves assets. */
UCLASS()
class NPCAUDIT_API UNpcAuditLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /** Metadata and current reflected state, with object references represented by paths. */
    UFUNCTION(BlueprintCallable, Category="NPC Audit")
    static FString DumpObject(UObject* Object, bool bIncludeInherited = true);

    /** Declared fields appear once per owning class in the inheritance hierarchy. */
    UFUNCTION(BlueprintCallable, Category="NPC Audit")
    static FString DumpClass(UClass* Class, bool bIncludeInherited = true);

    /** Read all actual graphs, nodes, pins and connections without saving or changing them. */
    UFUNCTION(BlueprintCallable, Category="NPC Audit")
    static FString DumpBlueprintGraphs(UObject* BlueprintAsset);

    /** Dispatch a real function with strict input names; does not await latent actions. */
    UFUNCTION(BlueprintCallable, Category="NPC Audit")
    static FString Invoke(UObject* Object, const FString& FunctionName, const FString& ArgumentsJson);

    /** Read one reflected property, using its exact engine name. */
    UFUNCTION(BlueprintCallable, Category="NPC Audit")
    static FString ReadProperty(UObject* Object, const FString& PropertyName);

    /** Import into temporary storage before changing the actual in-memory property. */
    UFUNCTION(BlueprintCallable, Category="NPC Audit")
    static FString WriteProperty(UObject* Object, const FString& PropertyName, const FString& ValueJson);

    /** Compile the actual Blueprint with SkipSave and return compiler counts/messages. */
    UFUNCTION(BlueprintCallable, Category="NPC Audit")
    static FString CompileBlueprint(UObject* BlueprintAsset);
};
