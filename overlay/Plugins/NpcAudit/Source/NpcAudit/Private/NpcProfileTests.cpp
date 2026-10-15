#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AIController.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BrainComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DynamicRHI.h"
#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "RHI.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Slate/SceneViewport.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/UnrealType.h"
#include "UnrealEngine.h"

namespace NpcProfileAudit
{
    constexpr int32 Seed = 378;
    constexpr int32 InitialNpcCount = 5;
    constexpr int32 MinimumSamples = 30;
    const TCHAR* const TestMap = TEXT("/Game/ThirdPerson/Maps/ThirdPersonMap");
    const TCHAR* const NpcClassPath =
        TEXT("/Game/ThirdPerson/Blueprints/BP_NPC_Standard.BP_NPC_Standard_C");

    struct FSettings
    {
        int32 NpcCount = 5;
        double WarmupSeconds = 10.0;
        double CaptureSeconds = 15.0;
        FString Report;
    };

    struct FDurationSample
    {
        double AtSeconds = 0.0;
        double Milliseconds = 0.0;
    };

    struct FCounts
    {
        int32 Npcs = 0;
        int32 BegunPlay = 0;
        int32 Hidden = 0;
        int32 CollisionEnabled = 0;
        int32 TickEnabled = 0;
        int32 NpcAiControllers = 0;
        int32 WorldAiControllers = 0;
        int32 BrainRunning = 0;
        int32 BlackboardKnown = 0;
        int32 Active = 0;

        TSharedRef<FJsonObject> Json() const
        {
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("npc_count"), Npcs);
            Result->SetNumberField(TEXT("npc_begun_play_count"), BegunPlay);
            Result->SetNumberField(TEXT("npc_hidden_count"), Hidden);
            Result->SetNumberField(TEXT("npc_collision_enabled_count"), CollisionEnabled);
            Result->SetNumberField(TEXT("npc_actor_tick_enabled_count"), TickEnabled);
            Result->SetNumberField(TEXT("npc_ai_controller_count"), NpcAiControllers);
            Result->SetNumberField(TEXT("world_ai_controller_count"), WorldAiControllers);
            Result->SetNumberField(TEXT("npc_brain_running_count"), BrainRunning);
            Result->SetNumberField(TEXT("npc_blackboard_known_count"), BlackboardKnown);
            Result->SetNumberField(TEXT("npc_active_count"), Active);
            return Result;
        }
    };

    struct FMovementObservation
    {
        FString ObjectPath;
        FString Row;
        FVector Start = FVector::ZeroVector;
        FVector Last = FVector::ZeroVector;
        double FirstSampleAt = 0.0;
        double LastSampleAt = 0.0;
        double TotalDistance2D = 0.0;
        double MaxDistance2D = 0.0;
        int32 Samples = 0;
        int32 MovingPathSamples = 0;
        int32 MovingStatusSamples = 0;
        int32 ValidPathSamples = 0;
        int32 IdleSamples = 0;
        int32 WaitingSamples = 0;
        int32 PausedSamples = 0;
        int32 NoControllerSamples = 0;
        int32 SelectedNavDataSamples = 0;
        int32 ValidNavDataSamples = 0;
        int32 ProjectionSuccessSamples = 0;
        int32 AgentMatchSamples = 0;
        bool bFirstProjection = false;
        bool bLastProjection = false;
        bool bLastAgentValid = false;
        bool bLastAgentMatch = false;
        bool bLastNavDataValid = false;
        double LastAgentRadius = 0.0;
        double LastAgentHeight = 0.0;
        double LastDataAgentRadius = 0.0;
        double LastDataAgentHeight = 0.0;
        FString LastNavData;
    };

    static TSharedRef<FJsonObject> VectorJson(const FVector& Value)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("x"), Value.X);
        Result->SetNumberField(TEXT("y"), Value.Y);
        Result->SetNumberField(TEXT("z"), Value.Z);
        return Result;
    }

    static double Quantile(const TArray<double>& Sorted, double Fraction)
    {
        const double Position = static_cast<double>(Sorted.Num() - 1) * Fraction;
        const int32 Lower = FMath::FloorToInt(Position);
        const int32 Upper = FMath::Min(Lower + 1, Sorted.Num() - 1);
        return FMath::Lerp(Sorted[Lower], Sorted[Upper], Position - Lower);
    }

    static TSharedRef<FJsonObject> Summarize(const TArray<FDurationSample>& Samples)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("samples"), Samples.Num());
        Result->SetStringField(TEXT("quantile_method"), TEXT("linear interpolation at (n-1)*p"));
        if (Samples.IsEmpty())
        {
            return Result;
        }
        TArray<double> Sorted;
        Sorted.Reserve(Samples.Num());
        double Sum = 0.0;
        for (const FDurationSample& Sample : Samples)
        {
            Sorted.Add(Sample.Milliseconds);
            Sum += Sample.Milliseconds;
        }
        Sorted.Sort();
        Result->SetNumberField(TEXT("median_ms"), Quantile(Sorted, 0.5));
        Result->SetNumberField(TEXT("p95_ms"), Quantile(Sorted, 0.95));
        Result->SetNumberField(TEXT("minimum_ms"), Sorted[0]);
        Result->SetNumberField(TEXT("maximum_ms"), Sorted.Last());
        Result->SetNumberField(TEXT("mean_ms"), Sum / Samples.Num());
        Result->SetNumberField(TEXT("first_sample_at_seconds"), Samples[0].AtSeconds);
        Result->SetNumberField(TEXT("last_sample_at_seconds"), Samples.Last().AtSeconds);
        return Result;
    }

    static TArray<TSharedPtr<FJsonValue>> RawSamples(const TArray<FDurationSample>& Samples)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        Result.Reserve(Samples.Num());
        for (const FDurationSample& Sample : Samples)
        {
            TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetNumberField(TEXT("at_seconds"), Sample.AtSeconds);
            Entry->SetNumberField(TEXT("milliseconds"), Sample.Milliseconds);
            Result.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return Result;
    }

    static bool SetDouble(UObject* Object, const TCHAR* Name, double Value)
    {
        FDoubleProperty* Property = Object
            ? FindFProperty<FDoubleProperty>(Object->GetClass(), FName(Name)) : nullptr;
        if (!Property)
        {
            return false;
        }
        Property->SetPropertyValue_InContainer(Object, Value);
        return true;
    }

    static bool GetDouble(UObject* Object, const TCHAR* Name, double& Value)
    {
        FDoubleProperty* Property = Object
            ? FindFProperty<FDoubleProperty>(Object->GetClass(), FName(Name)) : nullptr;
        if (!Property)
        {
            return false;
        }
        Value = Property->GetPropertyValue_InContainer(Object);
        return true;
    }

    class FCapture final : public IAutomationLatentCommand
    {
    public:
        FCapture(FAutomationTestBase* InTest, FSettings InSettings)
            : Test(InTest), Settings(MoveTemp(InSettings)), StartedAt(FPlatformTime::Seconds())
        {
        }

        virtual ~FCapture() override
        {
            RemoveDelegates();
        }

        virtual bool Update() override
        {
            const double Now = FPlatformTime::Seconds();
            if (Now - StartedAt > Settings.WarmupSeconds + Settings.CaptureSeconds + 90.0)
            {
                Finish(false, TEXT("PIE setup or capture exceeded the bounded wall-clock limit"));
                return true;
            }
            UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
            if (!bSetup)
            {
                if (!IsValid(World) || World->WorldType != EWorldType::PIE || !World->HasBegunPlay())
                {
                    return false;
                }
                if (!Setup(World))
                {
                    Finish(false, TEXT("Actual PIE profile setup failed"));
                    return true;
                }
            }
            if (!ObservedWorld.IsValid() || World != ObservedWorld.Get())
            {
                Finish(false, TEXT("The observed PIE world ended or changed during capture"));
                return true;
            }
            if (Now >= SampleEnd)
            {
                Finish(true, FString());
                return true;
            }
            return false;
        }

    private:
        bool Setup(UWorld* World)
        {
            const FString Map = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
            if (Map != TestMap)
            {
                Test->AddError(TEXT("Profile did not start the original ThirdPersonMap"));
                return false;
            }
            UClass* LoadedClass = LoadClass<AActor>(nullptr, NpcClassPath);
            if (!LoadedClass)
            {
                Test->AddError(TEXT("The actual BP_NPC_Standard class could not be loaded"));
                return false;
            }
            ObservedWorld = World;
            NpcClass = LoadedClass;
            if (World->GetGameViewport() && World->GetGameViewport()->GetGameViewport())
            {
                World->GetGameViewport()->GetGameViewport()->SetFixedViewportSize(1280, 720);
            }
            AGameModeBase* GameMode = World->GetAuthGameMode();
            if (!SetDouble(GameMode, TEXT("TimeSpeed"), 0.0)
                || !SetDouble(GameMode, TEXT("CurrentTime"), 600.0))
            {
                Test->AddError(TEXT("Actual GameMode TimeSpeed or CurrentTime double property is missing"));
                return false;
            }
            FNameProperty* RowProperty = FindFProperty<FNameProperty>(LoadedClass, TEXT("NPC_Row_Name"));
            if (!RowProperty)
            {
                Test->AddError(TEXT("Actual NPC_Row_Name property is missing"));
                return false;
            }
            TArray<AActor*> Originals;
            for (TActorIterator<AActor> It(World, LoadedClass); It; ++It)
            {
                Originals.Add(*It);
            }
            if (Originals.Num() != InitialNpcCount)
            {
                Test->AddError(FString::Printf(TEXT("Expected five original NPC actors, observed %d"), Originals.Num()));
                return false;
            }
            Originals.Sort([RowProperty](const AActor& Left, const AActor& Right)
            {
                return RowProperty->GetPropertyValue_InContainer(&Left).ToString()
                    < RowProperty->GetPropertyValue_InContainer(&Right).ToString();
            });
            for (AActor* Original : Originals)
            {
                const FName Row = RowProperty->GetPropertyValue_InContainer(Original);
                if (Row.IsNone())
                {
                    Test->AddError(TEXT("An original NPC has no data row name"));
                    return false;
                }
                OriginalRows.Add(MakeShared<FJsonValueString>(Row.ToString()));
            }
            FMath::RandInit(Seed);
            // Eight surrounding cells, then one additional nearby cell. At most
            // nine clones of each of the five original rows are required.
            const int32 GridX[] = {-1, 0, 1, -1, 1, -1, 0, 1, 2};
            const int32 GridY[] = {-1, -1, -1, 0, 0, 1, 1, 1, 0};
            for (int32 Index = InitialNpcCount; Index < Settings.NpcCount; ++Index)
            {
                const int32 Added = Index - InitialNpcCount;
                AActor* Reference = Originals[Added % InitialNpcCount];
                const int32 Cell = Added / InitialNpcCount;
                FVector Location = Reference->GetActorLocation();
                Location += FVector(GridX[Cell] * 180.0, GridY[Cell] * 180.0, 5.0);
                const FTransform Transform(Reference->GetActorRotation(), Location);
                AActor* Spawned = World->SpawnActorDeferred<AActor>(LoadedClass, Transform,
                    nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
                if (!Spawned)
                {
                    Test->AddError(TEXT("Deferred spawning of an actual NPC Blueprint actor failed"));
                    return false;
                }
                RowProperty->SetPropertyValue_InContainer(Spawned,
                    RowProperty->GetPropertyValue_InContainer(Reference));
                UGameplayStatics::FinishSpawningActor(Spawned, Transform);
                if (APawn* Pawn = Cast<APawn>(Spawned))
                {
                    if (!Pawn->GetController())
                    {
                        Pawn->SpawnDefaultController();
                    }
                }
            }
            // Controller, Blackboard and behavior-tree initialization are given
            // real world frames during warmup; do not assert them at spawn time.
            SetupCounts = Count(World);
            if (SetupCounts.Npcs != Settings.NpcCount)
            {
                Test->AddError(TEXT("The actual NPC actor total differs after scaling"));
                return false;
            }
            SetupFinished = FPlatformTime::Seconds();
            SampleStart = SetupFinished + Settings.WarmupSeconds;
            SampleEnd = SampleStart + Settings.CaptureSeconds;
            InitialWorldTime = World->GetTimeSeconds();
            PreActorTick = FWorldDelegates::OnWorldPreActorTick.AddRaw(this, &FCapture::BeforeActors);
            PostActorTick = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FCapture::AfterActors);
            EndFrame = FCoreDelegates::OnEndFrame.AddRaw(this, &FCapture::AfterFrame);
            bSetup = true;
            return true;
        }

        FCounts Count(UWorld* World) const
        {
            FCounts Result;
            if (!IsValid(World) || !NpcClass.IsValid())
            {
                return Result;
            }
            for (TActorIterator<AAIController> It(World); It; ++It)
            {
                ++Result.WorldAiControllers;
            }
            for (TActorIterator<AActor> It(World, NpcClass.Get()); It; ++It)
            {
                AActor* Actor = *It;
                ++Result.Npcs;
                Result.BegunPlay += Actor->HasActorBegunPlay() ? 1 : 0;
                Result.Hidden += Actor->IsHidden() ? 1 : 0;
                Result.CollisionEnabled += Actor->GetActorEnableCollision() ? 1 : 0;
                Result.TickEnabled += Actor->IsActorTickEnabled() ? 1 : 0;
                const APawn* Pawn = Cast<APawn>(Actor);
                AAIController* Controller = Pawn ? Cast<AAIController>(Pawn->GetController()) : nullptr;
                if (Controller)
                {
                    ++Result.NpcAiControllers;
                    const UBrainComponent* Brain = Controller->GetBrainComponent();
                    Result.BrainRunning += Brain && Brain->IsRunning() ? 1 : 0;
                    const UBlackboardComponent* Blackboard = Controller->GetBlackboardComponent();
                    if (Blackboard && Blackboard->GetKeyID(TEXT("IsAtHome")) != FBlackboard::InvalidKey)
                    {
                        ++Result.BlackboardKnown;
                        Result.Active += !Actor->IsHidden() && Actor->GetActorEnableCollision()
                            && !Blackboard->GetValueAsBool(TEXT("IsAtHome")) ? 1 : 0;
                    }
                }
            }
            return Result;
        }

        void BeforeActors(UWorld* World, ELevelTick TickType, float DeltaSeconds)
        {
            const double Now = FPlatformTime::Seconds();
            if (World == ObservedWorld.Get() && Now >= SampleStart && Now < SampleEnd)
            {
                ActorTickStarted = Now;
            }
        }

        void AfterActors(UWorld* World, ELevelTick TickType, float DeltaSeconds)
        {
            if (World != ObservedWorld.Get() || ActorTickStarted < 0.0)
            {
                return;
            }
            const double Now = FPlatformTime::Seconds();
            const double Milliseconds = (Now - ActorTickStarted) * 1000.0;
            if (FMath::IsFinite(Milliseconds) && Milliseconds >= 0.0)
            {
                ActorSamples.Add({ActorTickStarted - SampleStart, Milliseconds});
            }
            ActorTickStarted = -1.0;
        }

        void AfterFrame()
        {
            if (!IsInGameThread() || !ObservedWorld.IsValid())
            {
                return;
            }
            const double Now = FPlatformTime::Seconds();
            if (LastFrameEnd >= SampleStart && Now <= SampleEnd)
            {
                const double Milliseconds = (Now - LastFrameEnd) * 1000.0;
                if (FMath::IsFinite(Milliseconds) && Milliseconds >= 0.0)
                {
                    FrameSamples.Add({Now - SampleStart, Milliseconds});
                }
            }
            LastFrameEnd = Now;
            if (Now >= SampleStart && Now <= SampleEnd && Now - LastMemorySample >= 0.25)
            {
                const FPlatformMemoryStats Memory = FPlatformMemory::GetStats();
                PeakSampledPhysical = FMath::Max(PeakSampledPhysical, Memory.UsedPhysical);
                TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetNumberField(TEXT("at_seconds"), Now - SampleStart);
                Entry->SetNumberField(TEXT("process_used_physical_bytes"), static_cast<double>(Memory.UsedPhysical));
                Entry->SetNumberField(TEXT("process_lifetime_peak_used_physical_bytes"), static_cast<double>(Memory.PeakUsedPhysical));
                Entry->SetObjectField(TEXT("counts"), Count(ObservedWorld.Get()).Json());
                // Deliberately outside the measured world pre/post actor-tick
                // span, on the existing memory cadence. End-frame intervals
                // include the cost of this observation, as they do other probes.
                Entry->SetObjectField(TEXT("npc_movement"), ObserveMovement(Now));
                MemoryAndCountSamples.Add(MakeShared<FJsonValueObject>(Entry));
                LastMemorySample = Now;
            }
        }

        TSharedRef<FJsonObject> ObserveMovement(double Now)
        {
            TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
            UWorld* World = ObservedWorld.Get();
            UNavigationSystemV1* Navigation = UNavigationSystemV1::GetCurrent(World);
            const FNameProperty* RowProperty = NpcClass.IsValid()
                ? FindFProperty<FNameProperty>(NpcClass.Get(), TEXT("NPC_Row_Name")) : nullptr;
            int32 Observed = 0;
            int32 Moving = 0;
            int32 Projected = 0;
            int32 ValidData = 0;
            int32 AgentMatches = 0;
            TMap<ANavigationData*, bool> DataValidity;
            for (TActorIterator<AActor> It(World, NpcClass.Get()); It; ++It)
            {
                AActor* Actor = *It;
                ++Observed;
                const FString Path = Actor->GetPathName();
                FMovementObservation& Record = Movement.FindOrAdd(Path);
                const FVector Location = Actor->GetActorLocation();
                if (Record.Samples == 0)
                {
                    Record.ObjectPath = Path;
                    Record.Row = RowProperty ? RowProperty->GetPropertyValue_InContainer(Actor).ToString() : TEXT("None");
                    Record.Start = Location;
                    Record.Last = Location;
                    Record.FirstSampleAt = Now - SampleStart;
                }
                else
                {
                    Record.TotalDistance2D += FVector::Dist2D(Record.Last, Location);
                }
                Record.Last = Location;
                Record.LastSampleAt = Now - SampleStart;
                Record.MaxDistance2D = FMath::Max(Record.MaxDistance2D, FVector::Dist2D(Record.Start, Location));
                const APawn* Pawn = Cast<APawn>(Actor);
                const AAIController* Controller = Pawn ? Cast<AAIController>(Pawn->GetController()) : nullptr;
                const UPathFollowingComponent* Following = Controller ? Controller->GetPathFollowingComponent() : nullptr;
                if (Controller && Following)
                {
                    const EPathFollowingStatus::Type Status = Controller->GetMoveStatus();
                    Record.MovingStatusSamples += Status == EPathFollowingStatus::Moving ? 1 : 0;
                    Record.IdleSamples += Status == EPathFollowingStatus::Idle ? 1 : 0;
                    Record.WaitingSamples += Status == EPathFollowingStatus::Waiting ? 1 : 0;
                    Record.PausedSamples += Status == EPathFollowingStatus::Paused ? 1 : 0;
                    const FNavPathSharedPtr CurrentPath = Following->GetPath();
                    const bool bValidPath = CurrentPath.IsValid() && CurrentPath->IsValid() && !CurrentPath->IsPartial();
                    Record.ValidPathSamples += bValidPath ? 1 : 0;
                    if (Status == EPathFollowingStatus::Moving && bValidPath)
                    {
                        ++Record.MovingPathSamples;
                        ++Moving;
                    }
                }
                else
                {
                    ++Record.NoControllerSamples;
                }
                Record.bLastProjection = false;
                Record.bLastAgentValid = Pawn && Pawn->GetNavAgentPropertiesRef().IsValid();
                Record.bLastAgentMatch = false;
                Record.bLastNavDataValid = false;
                Record.LastNavData = TEXT("None");
                Record.LastDataAgentRadius = 0.0;
                Record.LastDataAgentHeight = 0.0;
                if (Pawn)
                {
                    const FNavAgentProperties& Properties = Pawn->GetNavAgentPropertiesRef();
                    Record.LastAgentRadius = Properties.AgentRadius;
                    Record.LastAgentHeight = Properties.AgentHeight;
                    ANavigationData* Data = Navigation
                        ? Navigation->GetNavDataForProps(Properties, Location) : nullptr;
                    if (Data)
                    {
                        ++Record.SelectedNavDataSamples;
                        Record.LastNavData = Data->GetPathName();
                        Record.LastDataAgentRadius = Data->GetConfig().AgentRadius;
                        Record.LastDataAgentHeight = Data->GetConfig().AgentHeight;
                        Record.bLastAgentMatch = Data->DoesSupportAgent(Properties);
                        Record.AgentMatchSamples += Record.bLastAgentMatch ? 1 : 0;
                        AgentMatches += Record.bLastAgentMatch ? 1 : 0;
                        const bool* Cached = DataValidity.Find(Data);
                        if (!Cached)
                        {
                            bool bDataValid = Data->GetBounds().IsValid != 0;
#if WITH_RECAST
                            if (const ARecastNavMesh* Recast = Cast<ARecastNavMesh>(Data))
                                bDataValid = bDataValid && Recast->HasValidNavmesh();
#endif
                            DataValidity.Add(Data, bDataValid);
                            Cached = DataValidity.Find(Data);
                        }
                        Record.bLastNavDataValid = Cached && *Cached;
                        Record.ValidNavDataSamples += Record.bLastNavDataValid ? 1 : 0;
                        ValidData += Record.bLastNavDataValid ? 1 : 0;
                        FNavLocation NavPoint;
                        Record.bLastProjection = Navigation->ProjectPointToNavigation(Location, NavPoint,
                            FVector(100.0, 100.0, 500.0), &Properties);
                        Record.ProjectionSuccessSamples += Record.bLastProjection ? 1 : 0;
                        Projected += Record.bLastProjection ? 1 : 0;
                    }
                }
                if (Record.Samples == 0) Record.bFirstProjection = Record.bLastProjection;
                ++Record.Samples;
            }
            Summary->SetNumberField(TEXT("observed_actor_count"), Observed);
            Summary->SetNumberField(TEXT("moving_complete_path_actor_count"), Moving);
            Summary->SetNumberField(TEXT("projected_actor_count"), Projected);
            Summary->SetNumberField(TEXT("valid_nav_data_actor_count"), ValidData);
            Summary->SetNumberField(TEXT("native_agent_config_match_actor_count"), AgentMatches);
            Summary->SetBoolField(TEXT("navigation_build_or_lock_in_progress"), Navigation
                && UNavigationSystemV1::IsNavigationBeingBuiltOrLocked(World));
            return Summary;
        }

        TSharedRef<FJsonObject> MovementJson(int32& MovedAndPathActors) const
        {
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            TArray<FString> Keys;
            Movement.GetKeys(Keys);
            Keys.Sort();
            TArray<TSharedPtr<FJsonValue>> Actors;
            int32 Moved = 0;
            int32 MovingPathSamples = 0;
            int32 LastProjected = 0;
            int32 EverProjected = 0;
            int32 LastValidData = 0;
            int32 LastValidAgents = 0;
            int32 LastAgentMatches = 0;
            MovedAndPathActors = 0;
            for (const FString& Key : Keys)
            {
                const FMovementObservation& Record = Movement.FindChecked(Key);
                const bool bMoved = Record.MaxDistance2D > 25.0;
                Moved += bMoved ? 1 : 0;
                MovingPathSamples += Record.MovingPathSamples;
                MovedAndPathActors += bMoved && Record.MovingPathSamples > 0 ? 1 : 0;
                LastProjected += Record.bLastProjection ? 1 : 0;
                EverProjected += Record.ProjectionSuccessSamples > 0 ? 1 : 0;
                LastValidData += Record.bLastNavDataValid ? 1 : 0;
                LastValidAgents += Record.bLastAgentValid ? 1 : 0;
                LastAgentMatches += Record.bLastAgentMatch ? 1 : 0;
                TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("actor"), Record.ObjectPath);
                Entry->SetStringField(TEXT("npc_row"), Record.Row);
                Entry->SetObjectField(TEXT("start_position"), VectorJson(Record.Start));
                Entry->SetObjectField(TEXT("end_position"), VectorJson(Record.Last));
                Entry->SetNumberField(TEXT("first_sample_at_seconds"), Record.FirstSampleAt);
                Entry->SetNumberField(TEXT("last_sample_at_seconds"), Record.LastSampleAt);
                Entry->SetNumberField(TEXT("samples"), Record.Samples);
                Entry->SetNumberField(TEXT("total_distance_2d_uu"), Record.TotalDistance2D);
                Entry->SetNumberField(TEXT("max_distance_from_start_2d_uu"), Record.MaxDistance2D);
                Entry->SetNumberField(TEXT("start_to_end_distance_2d_uu"), FVector::Dist2D(Record.Start, Record.Last));
                Entry->SetBoolField(TEXT("moved_over_25uu"), bMoved);
                Entry->SetNumberField(TEXT("moving_path_sample_count"), Record.MovingPathSamples);
                Entry->SetNumberField(TEXT("valid_complete_path_sample_count"), Record.ValidPathSamples);
                Entry->SetNumberField(TEXT("moving_status_sample_count"), Record.MovingStatusSamples);
                Entry->SetNumberField(TEXT("idle_status_sample_count"), Record.IdleSamples);
                Entry->SetNumberField(TEXT("waiting_status_sample_count"), Record.WaitingSamples);
                Entry->SetNumberField(TEXT("paused_status_sample_count"), Record.PausedSamples);
                Entry->SetNumberField(TEXT("missing_controller_or_path_component_sample_count"), Record.NoControllerSamples);
                Entry->SetBoolField(TEXT("initial_projection_success"), Record.bFirstProjection);
                Entry->SetBoolField(TEXT("last_projection_success"), Record.bLastProjection);
                Entry->SetNumberField(TEXT("projection_success_sample_count"), Record.ProjectionSuccessSamples);
                Entry->SetNumberField(TEXT("selected_nav_data_sample_count"), Record.SelectedNavDataSamples);
                Entry->SetNumberField(TEXT("valid_nav_data_sample_count"), Record.ValidNavDataSamples);
                Entry->SetNumberField(TEXT("native_agent_config_match_sample_count"), Record.AgentMatchSamples);
                Entry->SetBoolField(TEXT("last_agent_properties_valid"), Record.bLastAgentValid);
                Entry->SetBoolField(TEXT("last_native_agent_config_match"), Record.bLastAgentMatch);
                Entry->SetStringField(TEXT("last_selected_nav_data"), Record.LastNavData);
                Entry->SetNumberField(TEXT("last_agent_radius"), Record.LastAgentRadius);
                Entry->SetNumberField(TEXT("last_agent_height"), Record.LastAgentHeight);
                Entry->SetNumberField(TEXT("last_nav_data_agent_radius"), Record.LastDataAgentRadius);
                Entry->SetNumberField(TEXT("last_nav_data_agent_height"), Record.LastDataAgentHeight);
                Actors.Add(MakeShared<FJsonValueObject>(Entry));
            }
            Result->SetStringField(TEXT("schema"), TEXT("npc-profile-movement-observations/v1"));
            Result->SetStringField(TEXT("scope"), TEXT("Actual NPC actors observed only inside the capture window from AfterFrame on the existing memory cadence. Initial/end positions refer to first/last movement samples. Distances are2D to exclude falling; sampled segment totals underestimate curved/full trajectories. Movement plus moving complete path is evidence for at least one actor, not a guarantee that every NPC walks or reaches every target."));
            Result->SetStringField(TEXT("nav_scope"), TEXT("Actual NPC agent properties select navigation data using the engine. Projection uses100/100/500 extent. Native DoesSupportAgent is recorded, not imposed as a stricter selection rule. Valid data requires valid bounds and, for Recast, native HasValidNavmesh. Projection and moving paths are separate evidence of traversable local navigation."));
            Result->SetStringField(TEXT("clone_position_scope"), TEXT("Original actors retain authored positions. Clones use the existing fixed180uu offsets and are not silently relocated onto navigation. Per-actor projections expose which sampled locations are navigable; no all-spawn-points guarantee is inferred."));
            Result->SetStringField(TEXT("probe_timing_scope"), TEXT("Movement/path/projection probes execute in AfterFrame, outside OnWorldPre/PostActorTick. Their overhead is included in editor end-frame intervals."));
            Result->SetNumberField(TEXT("observed_actor_count"), Movement.Num());
            Result->SetNumberField(TEXT("moved_over_25uu_count"), Moved);
            Result->SetNumberField(TEXT("moving_path_sample_count"), MovingPathSamples);
            Result->SetNumberField(TEXT("moved_over_25uu_with_moving_path_count"), MovedAndPathActors);
            Result->SetNumberField(TEXT("last_projected_actor_count"), LastProjected);
            Result->SetNumberField(TEXT("ever_projected_actor_count"), EverProjected);
            Result->SetNumberField(TEXT("last_valid_nav_data_actor_count"), LastValidData);
            Result->SetNumberField(TEXT("last_valid_agent_properties_actor_count"), LastValidAgents);
            Result->SetNumberField(TEXT("last_native_agent_config_match_actor_count"), LastAgentMatches);
            Result->SetNumberField(TEXT("sampling_minimum_interval_seconds"), 0.25);
            Result->SetArrayField(TEXT("per_actor"), Actors);
            return Result;
        }

        void RemoveDelegates()
        {
            if (PreActorTick.IsValid())
            {
                FWorldDelegates::OnWorldPreActorTick.Remove(PreActorTick);
                PreActorTick.Reset();
            }
            if (PostActorTick.IsValid())
            {
                FWorldDelegates::OnWorldPostActorTick.Remove(PostActorTick);
                PostActorTick.Reset();
            }
            if (EndFrame.IsValid())
            {
                FCoreDelegates::OnEndFrame.Remove(EndFrame);
                EndFrame.Reset();
            }
        }

        void Finish(bool bCompleted, const FString& Error)
        {
            RemoveDelegates();
            const double Now = FPlatformTime::Seconds();
            UWorld* World = ObservedWorld.Get();
            const FCounts FinalCounts = Count(World);
            int32 MovedAndPathActors = 0;
            const TSharedRef<FJsonObject> MovementReport = MovementJson(MovedAndPathActors);
            if (!Error.IsEmpty())
            {
                Test->AddError(Error);
            }
            if (bCompleted)
            {
                Test->TestTrue(TEXT("At least 30 actual world actor-tick samples were captured"),
                    ActorSamples.Num() >= MinimumSamples);
                Test->TestTrue(TEXT("At least 30 actual end-frame intervals were captured"),
                    FrameSamples.Num() >= MinimumSamples);
                Test->TestEqual(TEXT("Final actual NPC count equals the requested fixture"),
                    FinalCounts.Npcs, Settings.NpcCount);
                Test->TestEqual(TEXT("Every profiled NPC has an actual AI controller after warmup"),
                    FinalCounts.NpcAiControllers, Settings.NpcCount);
                Test->TestEqual(TEXT("Every profiled NPC has an actual running brain after capture"),
                    FinalCounts.BrainRunning, Settings.NpcCount);
                Test->TestEqual(TEXT("All profiled NPCs are visibly active at minute 600"),
                    FinalCounts.Active, Settings.NpcCount);
                Test->TestEqual(TEXT("Every profiled NPC was observed by the movement probe"),
                    Movement.Num(), Settings.NpcCount);
                Test->TestTrue(TEXT("At least one same NPC moved over25uu and had an actual moving complete path during capture"),
                    MovedAndPathActors >= 1);
            }
            TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
            Report->SetStringField(TEXT("schema"), TEXT("npc-actual-pie-profile/v1"));
            Report->SetStringField(TEXT("test"), TEXT("NPC.Profile.Capture"));
            Report->SetStringField(TEXT("status"), bCompleted && !Test->HasAnyErrors() ? TEXT("passed") : TEXT("failed"));
            Report->SetStringField(TEXT("error"), Error);
            Report->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
            Report->SetStringField(TEXT("map"), TestMap);
            Report->SetNumberField(TEXT("requested_npc_count"), Settings.NpcCount);
            Report->SetNumberField(TEXT("original_npc_count"), InitialNpcCount);
            Report->SetNumberField(TEXT("spawned_npc_count"), Settings.NpcCount - InitialNpcCount);
            Report->SetArrayField(TEXT("original_row_names"), OriginalRows);
            Report->SetNumberField(TEXT("seed"), Seed);
            Report->SetStringField(TEXT("seed_scope"), TEXT("Global FMath::RandInit before loading the map and before deferred clone spawning; not a guarantee of identical whole-engine stochastic traces."));
            Report->SetNumberField(TEXT("frozen_schedule_minute"), 600.0);
            Report->SetNumberField(TEXT("configured_time_speed"), 0.0);
            Report->SetNumberField(TEXT("requested_warmup_seconds"), Settings.WarmupSeconds);
            Report->SetNumberField(TEXT("requested_sample_seconds"), Settings.CaptureSeconds);
            Report->SetNumberField(TEXT("total_test_wall_seconds"), Now - StartedAt);
            Report->SetNumberField(TEXT("setup_wall_seconds"), bSetup ? SetupFinished - StartedAt : Now - StartedAt);
            Report->SetNumberField(TEXT("maximum_test_wall_seconds"), Settings.WarmupSeconds + Settings.CaptureSeconds + 90.0);
            Report->SetNumberField(TEXT("completion_check_lateness_seconds"), bSetup ? FMath::Max(0.0, Now - SampleEnd) : 0.0);
            Report->SetNumberField(TEXT("minimum_required_samples"), MinimumSamples);
            Report->SetObjectField(TEXT("setup_counts"), SetupCounts.Json());
            Report->SetObjectField(TEXT("final_counts"), FinalCounts.Json());
            Report->SetObjectField(TEXT("npc_movement"), MovementReport);
            Report->SetBoolField(TEXT("movement_requirement_passed"), MovedAndPathActors >= 1);
            Report->SetStringField(TEXT("active_count_definition"), TEXT("Original NPC class, visible, actor collision enabled, actual AI Blackboard has IsAtHome=false. Actor tick and brain running are reported separately."));
            Report->SetObjectField(TEXT("world_actor_tick_ms"), Summarize(ActorSamples));
            Report->SetObjectField(TEXT("editor_end_frame_interval_ms"), Summarize(FrameSamples));
            Report->SetArrayField(TEXT("raw_world_actor_tick_ms"), RawSamples(ActorSamples));
            Report->SetArrayField(TEXT("raw_editor_end_frame_interval_ms"), RawSamples(FrameSamples));
            Report->SetArrayField(TEXT("raw_memory_and_counts"), MemoryAndCountSamples);
            Report->SetStringField(TEXT("world_actor_tick_scope"), TEXT("FPlatformTime span between OnWorldPreActorTick and OnWorldPostActorTick for this PIE world. Includes all actors and intervening world work, not isolated NPC CPU time, a complete game-thread time, or GPU time. Tick starts within the capture window; its completion can cross the final boundary."));
            Report->SetStringField(TEXT("frame_interval_scope"), TEXT("Wall-clock intervals between actual game-thread OnEndFrame delegates wholly inside the capture window. Includes editor, PIE, rendering waits, frame caps and instrumentation; not standalone packaged-game FPS or isolated GPU frame time."));
            Report->SetStringField(TEXT("memory_scope"), TEXT("Whole editor process physical memory sampled at least 0.25 seconds apart; not per-NPC allocation or an allocation cap. Lifetime OS peak also includes startup and map loading."));
            Report->SetNumberField(TEXT("memory_sampling_minimum_interval_seconds"), 0.25);
            Report->SetNumberField(TEXT("peak_sampled_process_used_physical_bytes"), static_cast<double>(PeakSampledPhysical));
            const FPlatformMemoryStats Memory = FPlatformMemory::GetStats();
            Report->SetNumberField(TEXT("final_process_used_physical_bytes"), static_cast<double>(Memory.UsedPhysical));
            Report->SetNumberField(TEXT("process_lifetime_peak_used_physical_bytes"), static_cast<double>(Memory.PeakUsedPhysical));
            Report->SetStringField(TEXT("cpu_brand"), FPlatformMisc::GetCPUBrand());
            Report->SetNumberField(TEXT("logical_cpu_count"), FPlatformMisc::NumberOfCoresIncludingHyperthreads());
            Report->SetNumberField(TEXT("machine_physical_memory_bytes"), static_cast<double>(FPlatformMemory::GetConstants().TotalPhysical));
            Report->SetBoolField(TEXT("nullrhi_requested"), FParse::Param(FCommandLine::Get(), TEXT("nullrhi")));
            Report->SetStringField(TEXT("rhi_name"), GDynamicRHI ? GDynamicRHI->GetName() : TEXT("not_initialized"));
            Report->SetStringField(TEXT("rhi_adapter_name"), GRHIAdapterName);
            Report->SetNumberField(TEXT("system_resolution_width"), GSystemResolution.ResX);
            Report->SetNumberField(TEXT("system_resolution_height"), GSystemResolution.ResY);
            Report->SetNumberField(TEXT("system_window_mode"), static_cast<int32>(GSystemResolution.WindowMode));
            Report->SetBoolField(TEXT("fixed_timestep_enabled"), FApp::UseFixedTimeStep());
            Report->SetNumberField(TEXT("configured_fixed_delta_seconds"), FApp::GetFixedDeltaTime());
            if (const IConsoleVariable* MaxFps = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS")))
            {
                Report->SetNumberField(TEXT("configured_max_fps"), MaxFps->GetFloat());
            }
            if (const IConsoleVariable* VSync = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync")))
            {
                Report->SetNumberField(TEXT("configured_vsync"), VSync->GetInt());
            }
            if (World)
            {
                Report->SetNumberField(TEXT("end_world_time_seconds"), World->GetTimeSeconds());
                Report->SetNumberField(TEXT("world_time_advanced_since_setup_seconds"), World->GetTimeSeconds() - InitialWorldTime);
                double CurrentTime = 0.0;
                if (GetDouble(World->GetAuthGameMode(), TEXT("CurrentTime"), CurrentTime))
                {
                    Report->SetNumberField(TEXT("end_schedule_minute"), CurrentTime);
                }
                UGameViewportClient* GameViewport = World->GetGameViewport();
                if (GameViewport && GameViewport->Viewport)
                {
                    const FIntPoint Size = GameViewport->Viewport->GetSizeXY();
                    Report->SetNumberField(TEXT("actual_game_viewport_width"), Size.X);
                    Report->SetNumberField(TEXT("actual_game_viewport_height"), Size.Y);
                }
            }
            Report->SetBoolField(TEXT("assertions_passed"), !Test->HasAnyErrors());
            FString Json;
            const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
            if (!FJsonSerializer::Serialize(Report, Writer)
                || !IFileManager::Get().MakeDirectory(*FPaths::GetPath(Settings.Report), true)
                || !FFileHelper::SaveStringToFile(Json, *Settings.Report,
                    FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
            {
                Test->AddError(TEXT("Could not write the actual PIE profile report"));
            }
            else
            {
                Test->AddInfo(FString::Printf(TEXT("Actual PIE profile report written: %s"), *Settings.Report));
            }
        }

        FAutomationTestBase* Test;
        FSettings Settings;
        TWeakObjectPtr<UWorld> ObservedWorld;
        TWeakObjectPtr<UClass> NpcClass;
        FDelegateHandle PreActorTick;
        FDelegateHandle PostActorTick;
        FDelegateHandle EndFrame;
        TArray<FDurationSample> ActorSamples;
        TArray<FDurationSample> FrameSamples;
        TArray<TSharedPtr<FJsonValue>> MemoryAndCountSamples;
        TArray<TSharedPtr<FJsonValue>> OriginalRows;
        TMap<FString, FMovementObservation> Movement;
        FCounts SetupCounts;
        double StartedAt = 0.0;
        double SetupFinished = 0.0;
        double SampleStart = 0.0;
        double SampleEnd = 0.0;
        double ActorTickStarted = -1.0;
        double LastFrameEnd = -1.0;
        double LastMemorySample = -1.0;
        double InitialWorldTime = 0.0;
        uint64 PeakSampledPhysical = 0;
        bool bSetup = false;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNpcActualPieProfile,
    "NPC.Profile.Capture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNpcActualPieProfile::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("An Unreal Editor instance is available"), GEditor))
    {
        return false;
    }
    if (GEditor->PlayWorld)
    {
        AddError(TEXT("NPC profiling requires no pre-existing PIE session"));
        return false;
    }
    NpcProfileAudit::FSettings Settings;
    FParse::Value(FCommandLine::Get(), TEXT("NpcProfileCount="), Settings.NpcCount);
    FParse::Value(FCommandLine::Get(), TEXT("NpcProfileWarmup="), Settings.WarmupSeconds);
    FParse::Value(FCommandLine::Get(), TEXT("NpcProfileSeconds="), Settings.CaptureSeconds);
    FParse::Value(FCommandLine::Get(), TEXT("NpcProfileReport="), Settings.Report);
    if (Settings.NpcCount != 5 && Settings.NpcCount != 20 && Settings.NpcCount != 50)
    {
        AddError(TEXT("NpcProfileCount must be 5, 20 or 50 total NPCs"));
        return false;
    }
    if (!FMath::IsFinite(Settings.WarmupSeconds) || !FMath::IsFinite(Settings.CaptureSeconds)
        || Settings.WarmupSeconds < 0.0 || Settings.CaptureSeconds < 1.0
        || Settings.WarmupSeconds + Settings.CaptureSeconds < 24.0
        || Settings.WarmupSeconds + Settings.CaptureSeconds > 60.0)
    {
        AddError(TEXT("Profile warmup and sampling must be finite, total 24..60 seconds, with at least one sampling second"));
        return false;
    }
    if (Settings.Report.IsEmpty())
    {
        Settings.Report = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NpcAudit/profile.json"));
    }
    Settings.Report = FPaths::ConvertRelativePathToFull(Settings.Report);
    FMath::RandInit(NpcProfileAudit::Seed);
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(NpcProfileAudit::TestMap));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(NpcProfileAudit::FCapture(this, MoveTemp(Settings)));
    // A failed setup, timeout or insufficient sample count still returns here.
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
