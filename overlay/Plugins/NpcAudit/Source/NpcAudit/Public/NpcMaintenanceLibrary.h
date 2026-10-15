#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NpcMaintenanceLibrary.generated.h"

/** Editor maintenance on real loaded assets. No implicit save-all or setting changes. */
UCLASS()
class NPCAUDIT_API UNpcMaintenanceLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /** Read actual nav actors, agent settings, bounds, chunk packages and dirty state. */
    UFUNCTION(BlueprintCallable, Category="NPC Audit|Maintenance")
    static FString InspectNavigation(UObject* WorldContextObject);

    /** Keep editor geometry intersecting the actual nav-bounds union loaded through
     * a world-owned spatial adapter; block deferred level-instance loads and flush
     * streaming/register existing components. Refuses active level-instance edits.
     * Does not change source streaming flags, rerun construction scripts or save.
     */
    UFUNCTION(BlueprintCallable, Category="NPC Audit|Maintenance")
    static FString LoadNavigationGeometry(UObject* WorldContextObject);

    /** Finish the loaded nav actors' already queued initial generation work.
     * Does not rebuild settings, tick the editor recursively, or save any assets.
     */
    UFUNCTION(BlueprintCallable, Category="NPC Audit|Maintenance")
    static FString WaitForNavigationBuild(UObject* WorldContextObject);

    /** Build the current editor world's non-WP navigation with its existing configuration.
     * Save defaults to false. WP navigation requires the engine's dedicated chunk builder.
     */
    UFUNCTION(BlueprintCallable, Category="NPC Audit|Maintenance")
    static FString RebuildNavigation(UObject* WorldContextObject, bool bSave = false);

    /** Dump the actual vendor butterfly graph without modifying or compiling it. */
    UFUNCTION(BlueprintCallable, Category="NPC Audit|Maintenance")
    static FString InspectButterflyGraphs();

    /** Save only an explicitly reviewed list of loaded /Game packages. Requires an unused
     * JSON journal filename inside Saved/NpcAudit. The caller maintains source backups.
     * Each package save is independent; the journal records partial failure.
     */
    UFUNCTION(BlueprintCallable, Category="NPC Audit|Maintenance")
    static FString SaveReviewedPackages(const TArray<FString>& PackagePaths, const FString& JournalPath);
};
