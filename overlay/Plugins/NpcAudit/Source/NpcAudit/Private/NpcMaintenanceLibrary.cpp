#include "NpcMaintenanceLibrary.h"

#include "NpcAuditLibrary.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "WorldPartition/NavigationData/NavigationDataChunkActor.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterShape.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionEditorLoaderAdapter.h"

namespace NpcMaintenance
{
    static TSharedRef<FJsonObject> Response(const TCHAR* Operation, bool bOk)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("schema"), TEXT("npc-maintenance/v1"));
        Json->SetStringField(TEXT("operation"), Operation);
        Json->SetBoolField(TEXT("ok"), bOk);
        return Json;
    }

    static FString Encode(const TSharedRef<FJsonObject>& Json)
    {
        FString Text;
        FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text));
        return Text;
    }

    static FString ErrorResponse(const TCHAR* Operation, const TCHAR* Code, const FString& Detail = FString())
    {
        TSharedRef<FJsonObject> Json = Response(Operation, false);
        Json->SetStringField(TEXT("error"), Code);
        if (!Detail.IsEmpty())
        {
            Json->SetStringField(TEXT("detail"), Detail);
        }
        return Encode(Json);
    }

    static UWorld* ResolveWorld(UObject* Context)
    {
        if (Context)
        {
            if (UWorld* World = Cast<UWorld>(Context))
            {
                return World;
            }
            return Context->GetWorld();
        }
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    static FString EditorWorldError(UWorld* World)
    {
        if (!IsInGameThread())
        {
            return TEXT("editor_game_thread_required");
        }
        if (!GEditor || !World || World->WorldType != EWorldType::Editor)
        {
            return TEXT("loaded_editor_world_required");
        }
        if (GEditor->PlayWorld || World->GetPackage()->HasAnyPackageFlags(PKG_PlayInEditor))
        {
            return TEXT("stop_pie_before_maintenance");
        }
        return FString();
    }

    static TSharedRef<FJsonObject> VectorJson(const FVector& Vector)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("x"), Vector.X);
        Json->SetNumberField(TEXT("y"), Vector.Y);
        Json->SetNumberField(TEXT("z"), Vector.Z);
        return Json;
    }

    static TSharedRef<FJsonObject> BoundsJson(const FBox& Box)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetBoolField(TEXT("valid"), Box.IsValid != 0);
        if (Box.IsValid)
        {
            Json->SetObjectField(TEXT("min"), VectorJson(Box.Min));
            Json->SetObjectField(TEXT("max"), VectorJson(Box.Max));
        }
        return Json;
    }

    static TSharedRef<FJsonObject> PackageJson(UPackage* Package)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("name"), Package ? Package->GetName() : FString());
        Json->SetBoolField(TEXT("dirty"), Package && Package->IsDirty());
        return Json;
    }

    static TSharedRef<FJsonObject> ActorJson(AActor* Actor)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("object"), Actor->GetPathName());
        Json->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
        // GetPackage resolves external actor packages, unlike following the logical outer chain.
        Json->SetObjectField(TEXT("storage_package"), PackageJson(Actor->GetPackage()));
        Json->SetStringField(TEXT("world_package"), Actor->GetWorld()->GetPackage()->GetName());
        Json->SetBoolField(TEXT("external_package"), Actor->GetPackage() != Actor->GetWorld()->GetPackage());
        return Json;
    }

    static bool ExportProperty(UObject* Object, const TCHAR* Name, FString& Out)
    {
        const FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), Name);
        if (!Property)
        {
            return false;
        }
        Property->ExportTextItem_Direct(Out, Property->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
        return true;
    }

    static bool ReadBool(UObject* Object, const TCHAR* Name, bool& Out)
    {
        const FBoolProperty* Property = FindFProperty<FBoolProperty>(Object->GetClass(), Name);
        if (!Property)
        {
            return false;
        }
        Out = Property->GetPropertyValue_InContainer(Object);
        return true;
    }

    static const TCHAR* const NavConfigurationFields[] = {
        TEXT("NavDataConfig"), TEXT("RuntimeGeneration"), TEXT("bFixedTilePoolSize"),
        TEXT("TilePoolSize"), TEXT("TileSizeUU"), TEXT("TileNumberHardLimit"),
        TEXT("bIsWorldPartitioned"), TEXT("AgentRadius"), TEXT("AgentHeight"),
        TEXT("AgentMaxSlope"), TEXT("AgentMaxStepHeight"), TEXT("NavMeshResolutionParams"),
        TEXT("CellSize"), TEXT("CellHeight"), TEXT("bSortNavigationAreasByCost"),
        TEXT("RegionPartitioning"), TEXT("LayerPartitioning"), TEXT("MinRegionArea"),
        TEXT("MergeRegionSize"), TEXT("MaxSimplificationError"), TEXT("MaxSimultaneousTileGenerationJobsCount")
    };

    static TSharedRef<FJsonObject> NavSettings(ANavigationData* NavData)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        for (const TCHAR* Name : NavConfigurationFields)
        {
            FString Value;
            if (ExportProperty(NavData, Name, Value))
            {
                Json->SetStringField(Name, Value);
            }
        }
        return Json;
    }

    static TMap<FString, FString> ConfigurationSnapshot(UWorld* World)
    {
        TMap<FString, FString> Snapshot;
        for (TActorIterator<ANavigationData> It(World); It; ++It)
        {
            for (const TCHAR* Name : NavConfigurationFields)
            {
                FString Value;
                if (ExportProperty(*It, Name, Value))
                {
                    Snapshot.Add(It->GetPathName() + TEXT("::") + Name, Value);
                }
            }
        }
        for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
        {
            Snapshot.Add(It->GetPathName() + TEXT("::bounds"), It->GetComponentsBoundingBox(true).ToString());
        }
        return Snapshot;
    }

    static bool SameConfiguration(const TMap<FString, FString>& Before, const TMap<FString, FString>& After)
    {
        if (Before.Num() != After.Num())
        {
            return false;
        }
        for (const TPair<FString, FString>& Entry : Before)
        {
            const FString* Value = After.Find(Entry.Key);
            if (!Value || *Value != Entry.Value)
            {
                return false;
            }
        }
        return true;
    }

    static bool OriginalEntriesRetained(const TMap<FString, FString>& Before, const TMap<FString, FString>& After)
    {
        for (const TPair<FString, FString>& Entry : Before)
        {
            const FString* Current = After.Find(Entry.Key);
            if (!Current || *Current != Entry.Value)
            {
                return false;
            }
        }
        return true;
    }

    static TMap<FString, FString> ActorSourceFlags(UWorld* World)
    {
        TMap<FString, FString> Snapshot;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            const FString Key = It->GetPathName();
            Snapshot.Add(Key + TEXT("::spatially_loaded"), It->GetIsSpatiallyLoaded() ? TEXT("true") : TEXT("false"));
            Snapshot.Add(Key + TEXT("::editor_only"), It->IsEditorOnly() ? TEXT("true") : TEXT("false"));
            Snapshot.Add(Key + TEXT("::hidden"), It->IsHidden() ? TEXT("true") : TEXT("false"));
            Snapshot.Add(Key + TEXT("::collision"), It->GetActorEnableCollision() ? TEXT("true") : TEXT("false"));
        }
        return Snapshot;
    }

    static TSharedRef<FJsonObject> GeometryCounts(UWorld* World)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        int32 ActorCount = 0;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            ++ActorCount;
        }
        int32 MeshCount = 0;
        int32 RegisteredCount = 0;
        int32 NavigationCollisionCount = 0;
        for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
        {
            if (IsValid(*It) && !It->IsTemplate() && It->GetWorld() == World)
            {
                ++MeshCount;
                if (It->IsRegistered())
                {
                    ++RegisteredCount;
                    if (It->GetStaticMesh() && It->CanEverAffectNavigation() && It->IsCollisionEnabled())
                    {
                        ++NavigationCollisionCount;
                    }
                }
            }
        }
        TArray<TSharedPtr<FJsonValue>> Levels;
        for (ULevel* Level : World->GetLevels())
        {
            if (IsValid(Level))
            {
                Levels.Add(MakeShared<FJsonValueString>(Level->GetPathName()));
            }
        }
        Result->SetNumberField(TEXT("loaded_actor_count"), ActorCount);
        Result->SetNumberField(TEXT("static_mesh_component_count"), MeshCount);
        Result->SetNumberField(TEXT("registered_static_mesh_component_count"), RegisteredCount);
        Result->SetNumberField(TEXT("registered_navigation_collision_mesh_components"), NavigationCollisionCount);
        Result->SetNumberField(TEXT("loaded_level_count"), Levels.Num());
        Result->SetArrayField(TEXT("loaded_levels"), Levels);
        return Result;
    }

    static int32 PopulatedTileBoundsCount(ARecastNavMesh* Recast)
    {
        int32 Count = 0;
        if (Recast->HasValidNavmesh())
        {
            for (int32 Index = 0; Index < Recast->GetNavMeshTilesCount(); ++Index)
            {
                const FBox Bounds = Recast->GetNavMeshTileBounds(Index);
                Count += Bounds.IsValid && !Bounds.ContainsNaN() ? 1 : 0;
            }
        }
        return Count;
    }

    static bool HasBuiltNavigationGeometry(ARecastNavMesh* Recast)
    {
        const FBox Bounds = Recast->GetNavMeshBounds();
        return Recast->HasValidNavmesh() && Bounds.IsValid && !Bounds.ContainsNaN()
            && PopulatedTileBoundsCount(Recast) > 0;
    }

    static TSharedRef<FJsonObject> Inspect(UWorld* World)
    {
        TSharedRef<FJsonObject> Json = Response(TEXT("inspect_navigation"), true);
        Json->SetStringField(TEXT("world"), World->GetPathName());
        Json->SetStringField(TEXT("world_type"), World->WorldType == EWorldType::Editor ? TEXT("editor") : TEXT("other"));
        Json->SetObjectField(TEXT("map_package"), PackageJson(World->GetPackage()));
        Json->SetBoolField(TEXT("world_partition_present"), World->GetWorldPartition() != nullptr);
        Json->SetBoolField(TEXT("navigation_build_in_progress"), UNavigationSystemV1::IsNavigationBeingBuilt(World));
        Json->SetStringField(TEXT("scope"), TEXT("currently loaded world actors; unloaded external actors are not enumerated"));
        TArray<TSharedPtr<FJsonValue>> DirtyPackages;
        for (TObjectIterator<UPackage> It; It; ++It)
        {
            if (It->IsDirty() && It->GetName().StartsWith(TEXT("/Game/")))
            {
                DirtyPackages.Add(MakeShared<FJsonValueString>(It->GetName()));
            }
        }
        Json->SetArrayField(TEXT("loaded_dirty_game_packages"), DirtyPackages);
        TArray<TSharedPtr<FJsonValue>> Navigation;
        for (TActorIterator<ANavigationData> It(World); It; ++It)
        {
            ANavigationData* NavData = *It;
            TSharedRef<FJsonObject> Actor = ActorJson(NavData);
            const FNavDataConfig& Config = NavData->GetConfig();
            TSharedRef<FJsonObject> Agent = MakeShared<FJsonObject>();
            Agent->SetStringField(TEXT("name"), Config.Name.ToString());
            Agent->SetNumberField(TEXT("radius"), Config.AgentRadius);
            Agent->SetNumberField(TEXT("height"), Config.AgentHeight);
            Agent->SetNumberField(TEXT("step_height"), Config.AgentStepHeight);
            Agent->SetObjectField(TEXT("default_query_extent"), VectorJson(Config.DefaultQueryExtent));
            Actor->SetObjectField(TEXT("agent"), Agent);
            Actor->SetObjectField(TEXT("configuration_export_text"), NavSettings(NavData));
            if (ARecastNavMesh* Recast = Cast<ARecastNavMesh>(NavData))
            {
                Actor->SetBoolField(TEXT("valid_navmesh"), Recast->HasValidNavmesh());
                Actor->SetBoolField(TEXT("resizable"), Recast->IsResizable());
                Actor->SetNumberField(TEXT("tile_slots"), Recast->GetNavMeshTilesCount());
                Actor->SetStringField(TEXT("tile_slots_definition"), TEXT("engine GetNavMeshTilesCount; allocated tile slots, not a traversable polygon count"));
                Actor->SetNumberField(TEXT("populated_tile_bounds_count"), PopulatedTileBoundsCount(Recast));
                Actor->SetBoolField(TEXT("built_navigation_geometry_present"), HasBuiltNavigationGeometry(Recast));
                Actor->SetObjectField(TEXT("navmesh_bounds"), BoundsJson(Recast->GetNavMeshBounds()));
                bool bPartitioned = false;
                Actor->SetBoolField(TEXT("partition_flag_available"), ReadBool(Recast, TEXT("bIsWorldPartitioned"), bPartitioned));
                Actor->SetBoolField(TEXT("partitioned_navmesh"), bPartitioned);
            }
            Navigation.Add(MakeShared<FJsonValueObject>(Actor));
        }
        Json->SetArrayField(TEXT("navigation_data"), Navigation);
        TArray<TSharedPtr<FJsonValue>> Bounds;
        for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
        {
            TSharedRef<FJsonObject> Actor = ActorJson(*It);
            Actor->SetObjectField(TEXT("bounds"), BoundsJson(It->GetComponentsBoundingBox(true)));
            Bounds.Add(MakeShared<FJsonValueObject>(Actor));
        }
        Json->SetArrayField(TEXT("bounds_volumes"), Bounds);
        TArray<TSharedPtr<FJsonValue>> Chunks;
        for (TActorIterator<ANavigationDataChunkActor> It(World); It; ++It)
        {
            TSharedRef<FJsonObject> Actor = ActorJson(*It);
            Actor->SetNumberField(TEXT("chunk_count"), It->GetNavDataChunk().Num());
            Actor->SetObjectField(TEXT("bounds"), BoundsJson(It->GetBounds()));
            Chunks.Add(MakeShared<FJsonValueObject>(Actor));
        }
        Json->SetArrayField(TEXT("navigation_chunk_actors"), Chunks);
        return Json;
    }

    static void AddNavPackages(UWorld* World, TSet<UPackage*>& Packages)
    {
        for (TActorIterator<ANavigationData> It(World); It; ++It)
        {
            Packages.Add(It->GetPackage());
        }
        for (TActorIterator<ANavigationDataChunkActor> It(World); It; ++It)
        {
            Packages.Add(It->GetPackage());
        }
    }

    static TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Items)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FString& Item : Items)
        {
            Values.Add(MakeShared<FJsonValueString>(Item));
        }
        return Values;
    }

    static bool SaveJournal(const TSharedRef<FJsonObject>& Journal, const FString& Path)
    {
        const FString Temporary = Path + TEXT(".writing");
        if (!FFileHelper::SaveStringToFile(Encode(Journal), *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            return false;
        }
        return IFileManager::Get().Move(*Path, *Temporary, true, false, false, true);
    }

    static bool FileDigest(const FString& Filename, FString& OutHash)
    {
        TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename));
        if (!Reader)
        {
            return false;
        }
        FSHA1 Hasher;
        TArray<uint8> Buffer;
        Buffer.SetNumUninitialized(1024 * 1024);
        while (!Reader->AtEnd() && !Reader->IsError())
        {
            const int64 Count = FMath::Min<int64>(Buffer.Num(), Reader->TotalSize() - Reader->Tell());
            if (Count <= 0)
            {
                break;
            }
            Reader->Serialize(Buffer.GetData(), Count);
            Hasher.Update(Buffer.GetData(), Count);
        }
        if (Reader->IsError() || !Reader->Close())
        {
            return false;
        }
        Hasher.Final();
        uint8 Hash[FSHA1::DigestSize];
        Hasher.GetHash(Hash);
        OutHash = BytesToHex(Hash, FSHA1::DigestSize).ToLower();
        return true;
    }
}

FString UNpcMaintenanceLibrary::InspectNavigation(UObject* WorldContextObject)
{
    if (!IsInGameThread())
    {
        return NpcMaintenance::ErrorResponse(TEXT("inspect_navigation"), TEXT("game_thread_required"));
    }
    UWorld* World = NpcMaintenance::ResolveWorld(WorldContextObject);
    return World ? NpcMaintenance::Encode(NpcMaintenance::Inspect(World))
                 : NpcMaintenance::ErrorResponse(TEXT("inspect_navigation"), TEXT("world_unavailable"));
}

FString UNpcMaintenanceLibrary::LoadNavigationGeometry(UObject* WorldContextObject)
{
    using namespace NpcMaintenance;
    UWorld* World = ResolveWorld(WorldContextObject);
    const FString ValidationError = EditorWorldError(World);
    if (!ValidationError.IsEmpty())
    {
        return ErrorResponse(TEXT("load_navigation_geometry"), TEXT("invalid_editor_state"), ValidationError);
    }
    FBox NavigationBounds(ForceInit);
    int32 BoundsCount = 0;
    for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
    {
        const FBox Bounds = It->GetComponentsBoundingBox(true);
        if (Bounds.IsValid && !Bounds.Min.ContainsNaN() && !Bounds.Max.ContainsNaN())
        {
            NavigationBounds += Bounds;
            ++BoundsCount;
        }
    }
    if (!NavigationBounds.IsValid || BoundsCount == 0)
    {
        return ErrorResponse(TEXT("load_navigation_geometry"), TEXT("actual_loaded_nav_bounds_required"));
    }
    const TMap<FString, FString> BeforeNavigation = ConfigurationSnapshot(World);
    const TMap<FString, FString> BeforeFlags = ActorSourceFlags(World);
    const TSharedRef<FJsonObject> BeforeCounts = GeometryCounts(World);
    const double Started = FPlatformTime::Seconds();
    UWorldPartition* Partition = World->GetWorldPartition();
    UWorldPartitionEditorLoaderAdapter* Adapter = nullptr;
    if (Partition)
    {
        if (!Partition->IsInitialized())
        {
            return ErrorResponse(TEXT("load_navigation_geometry"), TEXT("world_partition_not_initialized"));
        }
        Adapter = Partition->CreateEditorLoaderAdapter<FLoaderAdapterShape>(World, NavigationBounds,
            FString(TEXT("NPC Audit actual navigation bounds geometry")));
        if (!Adapter || !Adapter->GetLoaderAdapter())
        {
            return ErrorResponse(TEXT("load_navigation_geometry"), TEXT("editor_spatial_loader_unavailable"));
        }
        // The WorldPartition UPROPERTY retains this adapter. Do not release or unload
        // it before the subsequent wait/build/save, and do not alter actor source flags.
        Adapter->GetLoaderAdapter()->Load();
    }
    // A level-instance actor can be present while its world asset is still in the
    // LevelInstanceSubsystem's deferred queue. FlushLevelStreaming alone does not
    // drain that queue. Only load instances whose actual package bounds intersect
    // our original nav region; the engine's blocking primitive also drains children.
    ULevelInstanceSubsystem* LevelSubsystem = World->GetSubsystem<ULevelInstanceSubsystem>();
    TArray<TWeakObjectPtr<AActor>> InstancesToLoad;
    TArray<TSharedPtr<FJsonValue>> InstanceReports;
    int32 InitiallyUnloadedInstances = 0;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (ILevelInstanceInterface* Instance = Cast<ILevelInstanceInterface>(*It))
        {
            if (!LevelSubsystem)
            {
                return ErrorResponse(TEXT("load_navigation_geometry"), TEXT("level_instance_subsystem_unavailable"));
            }
            FBox InstanceBounds(ForceInit);
            if (!LevelSubsystem->GetLevelInstanceBounds(Instance, InstanceBounds)
                || !InstanceBounds.IsValid || InstanceBounds.ContainsNaN())
            {
                return ErrorResponse(TEXT("load_navigation_geometry"), TEXT("cannot_review_level_instance_bounds"), It->GetPathName());
            }
            if (!NavigationBounds.Intersect(InstanceBounds))
            {
                continue;
            }
            if (Instance->IsEditing() || Instance->HasChildEdit() || Instance->HasParentEdit())
            {
                return ErrorResponse(TEXT("load_navigation_geometry"), TEXT("finish_level_instance_edit_before_loading"), It->GetPathName());
            }
            TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("actor"), It->GetPathName());
            Entry->SetStringField(TEXT("world_asset"), Instance->GetWorldAssetPackage());
            Entry->SetObjectField(TEXT("bounds"), BoundsJson(InstanceBounds));
            Entry->SetBoolField(TEXT("loaded_before_block"), Instance->IsLoaded());
            InstanceReports.Add(MakeShared<FJsonValueObject>(Entry));
            InstancesToLoad.Add(*It);
            InitiallyUnloadedInstances += Instance->IsLoaded() ? 0 : 1;
        }
    }
    bool bInstancesLoaded = true;
    for (int32 Index = 0; Index < InstancesToLoad.Num(); ++Index)
    {
        AActor* Actor = InstancesToLoad[Index].Get();
        ILevelInstanceInterface* Instance = Actor ? Cast<ILevelInstanceInterface>(Actor) : nullptr;
        if (!Instance || Instance->IsEditing() || Instance->HasChildEdit() || Instance->HasParentEdit())
        {
            return ErrorResponse(TEXT("load_navigation_geometry"), TEXT("level_instance_changed_during_load_do_not_save"));
        }
        if (!Instance->IsLoaded() || LevelSubsystem->IsLoading(Instance) || !Instance->GetLoadedLevel())
        {
            LevelSubsystem->BlockLoadLevelInstance(Instance);
        }
        const bool bLoaded = Instance->IsLoaded() && Instance->GetLoadedLevel() != nullptr;
        bInstancesLoaded = bInstancesLoaded && bLoaded;
        InstanceReports[Index]->AsObject()->SetBoolField(TEXT("loaded_after_block"), bLoaded);
    }
    World->FlushLevelStreaming(EFlushLevelStreamingType::Full);
    FlushAsyncLoading();
    // Register the newly loaded geometry without rerunning construction scripts.
    World->UpdateWorldComponents(false, false);
    const double Elapsed = FPlatformTime::Seconds() - Started;
    const bool bNavigationRetained = OriginalEntriesRetained(BeforeNavigation, ConfigurationSnapshot(World));
    const bool bFlagsRetained = OriginalEntriesRetained(BeforeFlags, ActorSourceFlags(World));
    const TSharedRef<FJsonObject> AfterCounts = GeometryCounts(World);
    const bool bNewGeometryArrived = InitiallyUnloadedInstances == 0
        || (AfterCounts->GetNumberField(TEXT("loaded_level_count")) > BeforeCounts->GetNumberField(TEXT("loaded_level_count"))
            && AfterCounts->GetNumberField(TEXT("static_mesh_component_count")) > BeforeCounts->GetNumberField(TEXT("static_mesh_component_count")));
    const bool bInstanceGeometryPresent = InstancesToLoad.IsEmpty()
        || AfterCounts->GetNumberField(TEXT("loaded_level_count")) > 1;
    TSharedRef<FJsonObject> Result = Response(TEXT("load_navigation_geometry"), bNavigationRetained && bFlagsRetained
        && bInstancesLoaded && bNewGeometryArrived && bInstanceGeometryPresent);
    Result->SetStringField(TEXT("world"), World->GetPathName());
    Result->SetNumberField(TEXT("elapsed_seconds"), Elapsed);
    Result->SetNumberField(TEXT("original_nav_bounds_volume_count"), BoundsCount);
    Result->SetObjectField(TEXT("requested_load_bounds"), BoundsJson(NavigationBounds));
    Result->SetBoolField(TEXT("world_partition_present"), Partition != nullptr);
    Result->SetBoolField(TEXT("adapter_kept_registered"), Adapter != nullptr);
    Result->SetStringField(TEXT("adapter"), Adapter ? Adapter->GetPathName() : FString());
    Result->SetBoolField(TEXT("previously_loaded_navigation_configuration_unchanged"), bNavigationRetained);
    Result->SetBoolField(TEXT("previously_loaded_actor_source_flags_unchanged"), bFlagsRetained);
    Result->SetBoolField(TEXT("asset_saved"), false);
    Result->SetBoolField(TEXT("construction_scripts_rerun_requested"), false);
    Result->SetNumberField(TEXT("initially_unloaded_selected_level_instances"), InitiallyUnloadedInstances);
    Result->SetBoolField(TEXT("selected_level_instances_loaded"), bInstancesLoaded);
    Result->SetBoolField(TEXT("new_geometry_arrived_when_needed"), bNewGeometryArrived);
    Result->SetArrayField(TEXT("selected_level_instances"), InstanceReports);
    Result->SetObjectField(TEXT("before_geometry"), BeforeCounts);
    Result->SetObjectField(TEXT("after_geometry"), AfterCounts);
    Result->SetObjectField(TEXT("after_navigation"), Inspect(World));
    Result->SetStringField(TEXT("load_scope"), TEXT("spatial query limited to the original nav-bounds union; intersecting level instances and actor dependencies may also load child geometry"));
    Result->SetStringField(TEXT("validation_scope"), TEXT("loaded geometry counts and unchanged original settings/flags; nav build and gameplay readiness require subsequent checks"));
    if (!bNavigationRetained || !bFlagsRetained)
    {
        Result->SetStringField(TEXT("error"), TEXT("original_navigation_or_actor_source_state_changed_do_not_save"));
    }
    else if (!bInstancesLoaded || !bNewGeometryArrived || !bInstanceGeometryPresent)
    {
        Result->SetStringField(TEXT("error"), TEXT("level_instance_geometry_not_loaded_do_not_build_or_save"));
    }
    return Encode(Result);
}

FString UNpcMaintenanceLibrary::WaitForNavigationBuild(UObject* WorldContextObject)
{
    using namespace NpcMaintenance;
    UWorld* World = ResolveWorld(WorldContextObject);
    const FString ValidationError = EditorWorldError(World);
    if (!ValidationError.IsEmpty())
    {
        return ErrorResponse(TEXT("wait_for_navigation_build"), TEXT("invalid_editor_state"), ValidationError);
    }
    UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
    if (!NavSys)
    {
        return ErrorResponse(TEXT("wait_for_navigation_build"), TEXT("navigation_system_unavailable"));
    }
    const bool bWasBuilding = NavSys->IsNavigationBuildInProgress();
    const TMap<FString, FString> BeforeConfiguration = ConfigurationSnapshot(World);
    const double Started = FPlatformTime::Seconds();
    TArray<TSharedPtr<FJsonValue>> CompletedActors;
    // This is the same per-NavData blocking completion primitive used by the
    // engine's explicit Build. It does not re-enter the editor's main tick.
    for (TActorIterator<ANavigationData> It(World); It; ++It)
    {
        It->EnsureBuildCompletion();
        CompletedActors.Add(MakeShared<FJsonValueString>(It->GetPathName()));
    }
    const double Elapsed = FPlatformTime::Seconds() - Started;
    const bool bStillBuilding = NavSys->IsNavigationBuildInProgress();
    const bool bSameConfiguration = SameConfiguration(BeforeConfiguration, ConfigurationSnapshot(World));
    TSharedRef<FJsonObject> Result = Response(TEXT("wait_for_navigation_build"), !bStillBuilding && bSameConfiguration);
    Result->SetStringField(TEXT("world"), World->GetPathName());
    Result->SetBoolField(TEXT("navigation_build_in_progress_before"), bWasBuilding);
    Result->SetBoolField(TEXT("navigation_build_in_progress_after"), bStillBuilding);
    Result->SetBoolField(TEXT("configuration_unchanged"), bSameConfiguration);
    Result->SetBoolField(TEXT("asset_saved"), false);
    Result->SetBoolField(TEXT("explicit_rebuild_requested"), false);
    Result->SetNumberField(TEXT("elapsed_seconds"), Elapsed);
    Result->SetArrayField(TEXT("completed_navigation_actors"), CompletedActors);
    Result->SetObjectField(TEXT("after"), Inspect(World));
    if (bStillBuilding)
    {
        Result->SetStringField(TEXT("error"), TEXT("navigation_still_has_pending_work_do_not_save"));
    }
    else if (!bSameConfiguration)
    {
        Result->SetStringField(TEXT("error"), TEXT("navigation_configuration_changed_do_not_save"));
    }
    return Encode(Result);
}

FString UNpcMaintenanceLibrary::RebuildNavigation(UObject* WorldContextObject, bool bSave)
{
    using namespace NpcMaintenance;
    UWorld* World = ResolveWorld(WorldContextObject);
    const FString ValidationError = EditorWorldError(World);
    if (!ValidationError.IsEmpty())
    {
        return ErrorResponse(TEXT("rebuild_navigation"), TEXT("invalid_editor_state"), ValidationError);
    }
    UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
    if (!NavSys)
    {
        return ErrorResponse(TEXT("rebuild_navigation"), TEXT("navigation_system_unavailable"));
    }
    // The engine permits an explicit Build when only automatic editor updates are disabled.
    const uint8 ExplicitBuildLockMask = static_cast<uint8>(~ENavigationBuildLock::NoUpdateInEditor);
    if (NavSys->IsNavigationBuildingLocked(ExplicitBuildLockMask) || NavSys->IsNavigationBuildInProgress())
    {
        return ErrorResponse(TEXT("rebuild_navigation"), TEXT("navigation_locked_or_already_building"));
    }
    int32 NavCount = 0;
    for (TActorIterator<ANavigationData> It(World); It; ++It)
    {
        ++NavCount;
        if (ARecastNavMesh* Recast = Cast<ARecastNavMesh>(*It))
        {
            bool bPartitioned = false;
            if (!ReadBool(Recast, TEXT("bIsWorldPartitioned"), bPartitioned) || bPartitioned)
            {
                return ErrorResponse(TEXT("rebuild_navigation"), TEXT("partitioned_navmesh_requires_reviewed_world_partition_builder"));
            }
        }
    }
    if (NavCount == 0)
    {
        return ErrorResponse(TEXT("rebuild_navigation"), TEXT("existing_navigation_data_required"));
    }
    for (TActorIterator<ANavigationDataChunkActor> It(World); It; ++It)
    {
        return ErrorResponse(TEXT("rebuild_navigation"), TEXT("existing_navigation_chunks_require_reviewed_world_partition_builder"));
    }
    const TSharedRef<FJsonObject> Before = Inspect(World);
    const TMap<FString, FString> OriginalConfiguration = ConfigurationSnapshot(World);
    TSet<UPackage*> RelevantPackages;
    AddNavPackages(World, RelevantPackages);
    TSet<UPackage*> InitiallyDirty;
    for (TObjectIterator<UPackage> It; It; ++It)
    {
        if (It->IsDirty())
        {
            InitiallyDirty.Add(*It);
        }
    }
    const double Start = FPlatformTime::Seconds();
    // Engine 5.4 UNavigationSystemV1::Build invokes EnsureBuildCompletion on every NavData.
    // Deliberately do not change pool capacity, agent dimensions, bounds or generation mode.
    NavSys->Build();
    const double Elapsed = FPlatformTime::Seconds() - Start;
    AddNavPackages(World, RelevantPackages);
    if (World->GetPackage()->IsDirty() && !InitiallyDirty.Contains(World->GetPackage()))
    {
        RelevantPackages.Add(World->GetPackage());
    }
    TArray<FString> ReviewedCandidates;
    TArray<FString> PreexistingDirtyExcluded;
    for (UPackage* Package : RelevantPackages)
    {
        if (!Package || !Package->GetName().StartsWith(TEXT("/Game/")))
        {
            continue;
        }
        if (InitiallyDirty.Contains(Package))
        {
            PreexistingDirtyExcluded.Add(Package->GetName());
            continue;
        }
        // Recast data is serialized with its actor package. A rebuild is an intentional
        // in-memory change even when the generator did not mark the package for saving.
        Package->MarkPackageDirty();
        if (Package->IsDirty())
        {
            ReviewedCandidates.Add(Package->GetName());
        }
    }
    ReviewedCandidates.Sort();
    PreexistingDirtyExcluded.Sort();
    const bool bSameConfiguration = SameConfiguration(OriginalConfiguration, ConfigurationSnapshot(World));
    const bool bStillBuilding = NavSys->IsNavigationBuildInProgress();
    bool bRecastDataValid = true;
    int32 RecastCount = 0;
    for (TActorIterator<ARecastNavMesh> It(World); It; ++It)
    {
        ++RecastCount;
        bRecastDataValid = bRecastDataValid && HasBuiltNavigationGeometry(*It);
    }
    bRecastDataValid = RecastCount > 0 && bRecastDataValid;
    TSharedRef<FJsonObject> Result = Response(TEXT("rebuild_navigation"), bSameConfiguration && !bStillBuilding && bRecastDataValid);
    Result->SetNumberField(TEXT("elapsed_seconds"), Elapsed);
    Result->SetBoolField(TEXT("configuration_unchanged"), bSameConfiguration);
    Result->SetBoolField(TEXT("build_still_in_progress"), bStillBuilding);
    Result->SetBoolField(TEXT("recast_data_valid"), bRecastDataValid);
    Result->SetNumberField(TEXT("recast_navmesh_count"), RecastCount);
    Result->SetBoolField(TEXT("disk_save_requested"), bSave);
    Result->SetBoolField(TEXT("in_memory_navigation_rebuilt"), true);
    Result->SetArrayField(TEXT("reviewed_save_candidates"), Strings(ReviewedCandidates));
    Result->SetArrayField(TEXT("preexisting_dirty_excluded"), Strings(PreexistingDirtyExcluded));
    Result->SetObjectField(TEXT("before"), Before);
    Result->SetObjectField(TEXT("after"), Inspect(World));
    Result->SetStringField(TEXT("validation_scope"), TEXT("build completion and unchanged configuration; gameplay and fresh-load warning checks must be performed separately"));
    if (!bRecastDataValid)
    {
        Result->SetStringField(TEXT("error"), TEXT("rebuilt_navigation_has_no_valid_bounds_or_populated_tiles_do_not_save"));
    }
    if (bSave && bSameConfiguration && !bStillBuilding && bRecastDataValid && !ReviewedCandidates.IsEmpty())
    {
        const FString Journal = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NpcAudit"),
            TEXT("navigation-save-") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".json"));
        TSharedPtr<FJsonObject> SaveResult;
        const FString SaveJson = SaveReviewedPackages(ReviewedCandidates, Journal);
        if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SaveJson), SaveResult) && SaveResult)
        {
            Result->SetObjectField(TEXT("save"), SaveResult);
            Result->SetBoolField(TEXT("ok"), SaveResult->GetBoolField(TEXT("ok")));
        }
        else
        {
            Result->SetBoolField(TEXT("ok"), false);
            Result->SetStringField(TEXT("save_error"), TEXT("invalid_internal_save_response"));
        }
    }
    return Encode(Result);
}

FString UNpcMaintenanceLibrary::InspectButterflyGraphs()
{
    if (!IsInGameThread())
    {
        return NpcMaintenance::ErrorResponse(TEXT("inspect_butterfly_graphs"), TEXT("game_thread_required"));
    }
    UObject* Blueprint = LoadObject<UBlueprint>(nullptr,
        TEXT("/Game/AdvancedVillagePack/Blueprints/BP_Butterfly.BP_Butterfly"));
    return Blueprint ? UNpcAuditLibrary::DumpBlueprintGraphs(Blueprint)
                     : NpcMaintenance::ErrorResponse(TEXT("inspect_butterfly_graphs"), TEXT("butterfly_blueprint_unavailable"));
}

FString UNpcMaintenanceLibrary::SaveReviewedPackages(const TArray<FString>& PackagePaths, const FString& JournalPath)
{
    using namespace NpcMaintenance;
    const FString ValidationError = EditorWorldError(ResolveWorld(nullptr));
    if (!ValidationError.IsEmpty())
    {
        return ErrorResponse(TEXT("save_reviewed_packages"), TEXT("invalid_editor_state"), ValidationError);
    }
    if (PackagePaths.IsEmpty() || PackagePaths.Num() > 128)
    {
        return ErrorResponse(TEXT("save_reviewed_packages"), TEXT("explicit_package_list_1_to_128_required"));
    }
    FString Journal = FPaths::ConvertRelativePathToFull(JournalPath);
    FPaths::NormalizeFilename(Journal);
    FPaths::CollapseRelativeDirectories(Journal);
    FString JournalRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NpcAudit")));
    FPaths::NormalizeDirectoryName(JournalRoot);
    if (JournalPath.IsEmpty() || !FPaths::IsUnderDirectory(Journal, JournalRoot)
        || FPaths::GetExtension(Journal) != TEXT("json")
        || IFileManager::Get().FileExists(*Journal) || IFileManager::Get().FileExists(*(Journal + TEXT(".writing"))))
    {
        return ErrorResponse(TEXT("save_reviewed_packages"), TEXT("unused_json_journal_under_saved_npcaudit_required"));
    }
    TArray<UPackage*> Packages;
    TArray<FString> Filenames;
    TArray<TSharedPtr<FJsonValue>> Entries;
    TSet<FString> Seen;
    const FString ContentRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
    for (const FString& PackagePath : PackagePaths)
    {
        if (!PackagePath.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(PackagePath, false)
            || Seen.Contains(PackagePath.ToLower()))
        {
            return ErrorResponse(TEXT("save_reviewed_packages"), TEXT("invalid_or_duplicate_game_package"), PackagePath);
        }
        Seen.Add(PackagePath.ToLower());
        UPackage* Package = FindPackage(nullptr, *PackagePath);
        if (!Package || Package->GetName() != PackagePath || Package == GetTransientPackage()
            || Package->HasAnyFlags(RF_Transient) || Package->HasAnyPackageFlags(PKG_PlayInEditor)
            || !Package->IsFullyLoaded())
        {
            return ErrorResponse(TEXT("save_reviewed_packages"), TEXT("fully_loaded_source_package_required"), PackagePath);
        }
        TArray<UObject*> Objects;
        GetObjectsWithPackage(Package, Objects, true);
        for (UObject* Object : Objects)
        {
            if (const UBlueprint* Blueprint = Cast<UBlueprint>(Object))
            {
                if (Blueprint->Status != BS_UpToDate)
                {
                    return ErrorResponse(TEXT("save_reviewed_packages"), TEXT("blueprint_must_compile_without_errors_or_warnings"), Blueprint->GetPathName());
                }
            }
        }
        FString Filename;
        const FString Extension = Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
        if (!FPackageName::TryConvertLongPackageNameToFilename(PackagePath, Filename, Extension))
        {
            return ErrorResponse(TEXT("save_reviewed_packages"), TEXT("package_filename_conversion_failed"), PackagePath);
        }
        Filename = FPaths::ConvertRelativePathToFull(Filename);
        if (!FPaths::IsUnderDirectory(Filename, ContentRoot))
        {
            return ErrorResponse(TEXT("save_reviewed_packages"), TEXT("package_file_outside_project_content"), PackagePath);
        }
        if (IFileManager::Get().IsReadOnly(*Filename))
        {
            return ErrorResponse(TEXT("save_reviewed_packages"), TEXT("package_file_is_read_only"), PackagePath);
        }
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("package"), PackagePath);
        Entry->SetStringField(TEXT("filename"), Filename);
        Entry->SetStringField(TEXT("state"), TEXT("pending"));
        Entry->SetBoolField(TEXT("dirty_before"), Package->IsDirty());
        const bool bExists = IFileManager::Get().FileExists(*Filename);
        Entry->SetBoolField(TEXT("file_existed"), bExists);
        if (bExists)
        {
            FString Digest;
            if (!FileDigest(Filename, Digest))
            {
                return ErrorResponse(TEXT("save_reviewed_packages"), TEXT("cannot_hash_existing_package"), PackagePath);
            }
            Entry->SetStringField(TEXT("sha1_before"), Digest);
            Entry->SetNumberField(TEXT("bytes_before"), IFileManager::Get().FileSize(*Filename));
        }
        Packages.Add(Package);
        Filenames.Add(Filename);
        Entries.Add(MakeShared<FJsonValueObject>(Entry));
    }
    if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Journal), true))
    {
        return ErrorResponse(TEXT("save_reviewed_packages"), TEXT("cannot_create_journal_directory"));
    }
    TSharedRef<FJsonObject> Log = Response(TEXT("save_reviewed_packages"), false);
    Log->SetStringField(TEXT("state"), TEXT("prepared"));
    Log->SetStringField(TEXT("journal"), Journal);
    Log->SetStringField(TEXT("transaction_scope"), TEXT("independent package saves; no distributed or multi-file atomicity"));
    Log->SetBoolField(TEXT("caller_backups_required"), true);
    Log->SetBoolField(TEXT("backups_verified_by_this_api"), false);
    Log->SetArrayField(TEXT("packages"), Entries);
    if (!SaveJournal(Log, Journal))
    {
        return ErrorResponse(TEXT("save_reviewed_packages"), TEXT("cannot_record_save_intent"));
    }
    for (int32 Index = 0; Index < Packages.Num(); ++Index)
    {
        TSharedPtr<FJsonObject> Entry = Entries[Index]->AsObject();
        Log->SetStringField(TEXT("state"), TEXT("saving"));
        Entry->SetStringField(TEXT("state"), TEXT("saving"));
        if (!SaveJournal(Log, Journal))
        {
            Log->SetStringField(TEXT("error"), TEXT("journal_write_failed_before_save"));
            return Encode(Log);
        }
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.bSlowTask = false;
        const bool bSaved = UPackage::SavePackage(Packages[Index], Packages[Index]->FindAssetInPackage(), *Filenames[Index], Args);
        Entry->SetStringField(TEXT("state"), bSaved ? TEXT("saved") : TEXT("save_failed"));
        FString Digest;
        const bool bHasDigest = FileDigest(Filenames[Index], Digest);
        Entry->SetBoolField(TEXT("post_save_file_readable"), bHasDigest);
        if (bHasDigest)
        {
            Entry->SetStringField(TEXT("sha1_after"), Digest);
            Entry->SetNumberField(TEXT("bytes_after"), IFileManager::Get().FileSize(*Filenames[Index]));
        }
        const bool bComplete = bSaved && bHasDigest;
        Log->SetStringField(TEXT("state"), bComplete ? TEXT("saving") : TEXT("partial_failure"));
        if (!SaveJournal(Log, Journal))
        {
            Log->SetStringField(TEXT("error"), TEXT("save_outcome_not_committed_to_journal"));
            Log->SetStringField(TEXT("state"), TEXT("journal_failure_after_save"));
            return Encode(Log);
        }
        if (!bComplete)
        {
            return Encode(Log);
        }
    }
    Log->SetStringField(TEXT("state"), TEXT("completed"));
    Log->SetBoolField(TEXT("ok"), true);
    if (!SaveJournal(Log, Journal))
    {
        Log->SetBoolField(TEXT("ok"), false);
        Log->SetStringField(TEXT("error"), TEXT("final_journal_write_failed"));
    }
    return Encode(Log);
}
