#include "NpcAuditLibrary.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BrainComponent.h"
#include "Blueprint/UserWidget.h"
#include "Components/CapsuleComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationData.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/UnrealType.h"

#include <initializer_list>

namespace NpcNavigationAudit
{
    const TCHAR* const TestMap = TEXT("/Game/ThirdPerson/Maps/ThirdPersonMap");

    static TSharedRef<FJsonObject> Position(const FVector& Value)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("X"), Value.X);
        Result->SetNumberField(TEXT("Y"), Value.Y);
        Result->SetNumberField(TEXT("Z"), Value.Z);
        return Result;
    }

    static TSharedRef<FJsonObject> Bounds(const FBox& Value)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("valid"), Value.IsValid != 0);
        if (Value.IsValid)
        {
            Result->SetObjectField(TEXT("min"), Position(Value.Min));
            Result->SetObjectField(TEXT("max"), Position(Value.Max));
        }
        return Result;
    }

    static TSharedRef<FJsonObject> Agent(const FNavAgentProperties& Value)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("valid"), Value.IsValid());
        Result->SetNumberField(TEXT("radius"), Value.AgentRadius);
        Result->SetNumberField(TEXT("height"), Value.AgentHeight);
        Result->SetNumberField(TEXT("step_height"), Value.AgentStepHeight);
        Result->SetNumberField(TEXT("nav_walking_search_height_scale"), Value.NavWalkingSearchHeightScale);
        Result->SetStringField(TEXT("preferred_nav_data"), Value.PreferredNavData.ToString());
        return Result;
    }

    static TSharedRef<FJsonObject> Config(const FNavDataConfig& Value)
    {
        TSharedRef<FJsonObject> Result = Agent(Value);
        Result->SetStringField(TEXT("name"), Value.Name.ToString());
        Result->SetBoolField(TEXT("config_valid"), Value.IsValid());
        Result->SetObjectField(TEXT("default_query_extent"), Position(Value.DefaultQueryExtent));
        Result->SetStringField(TEXT("description"), Value.GetDescription());
        return Result;
    }

    static FString Encode(const TSharedPtr<FJsonValue>& Value)
    {
        FString Text;
        TArray<TSharedPtr<FJsonValue>> Envelope;
        Envelope.Add(Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
        if (!FJsonSerializer::Serialize(Envelope, Writer)
            || !Text.StartsWith(TEXT("[")) || !Text.EndsWith(TEXT("]"))) return FString();
        return Text.Mid(1, Text.Len() - 2);
    }

    static FString MoveStatus(EPathFollowingStatus::Type Status)
    {
        switch (Status)
        {
        case EPathFollowingStatus::Idle: return TEXT("Idle");
        case EPathFollowingStatus::Waiting: return TEXT("Waiting");
        case EPathFollowingStatus::Paused: return TEXT("Paused");
        case EPathFollowingStatus::Moving: return TEXT("Moving");
        default: return TEXT("Unknown");
        }
    }

    /** A real-world latent observer. It never issues a replacement NPC MoveTo. */
    class FNavigationContracts final : public IAutomationLatentCommand
    {
    public:
        explicit FNavigationContracts(FAutomationTestBase* InTest) : Test(InTest)
        {
            Report = MakeShared<FJsonObject>();
            Report->SetStringField(TEXT("schema"), TEXT("npc-gameplay-navigation-contracts/v1"));
            Report->SetStringField(TEXT("test"), TEXT("NPC.Gameplay.NavigationContracts"));
            Report->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
            Report->SetStringField(TEXT("scope"), TEXT("Actual original PIE world and BT_NPC. Observe native path following, world ticks, coordinates, and completion callbacks. Follow target is a player teleport fixture on a verified reachable path. Home arrival uses a nearby projected home fixture; it is not proof that the authored home destination is reachable."));
            Report->SetStringField(TEXT("isolation"), TEXT("Fresh dedicated PIE ends after this suite. Reflected fixture fields and actor transforms are restored before EndPIE; input focus, BT history, native latent tasks, and exact timing state are not claimed generally reversible. No assets or navigation settings are saved or modified."));
            Report->SetNumberField(TEXT("whole_wait_limit_seconds"), 90.0);
            Report->SetNumberField(TEXT("sample_interval_seconds"), 0.25);
            Report->SetNumberField(TEXT("expected_stages"), 3);
        }

        virtual ~FNavigationContracts() override { DetachObserver(); }

        virtual bool Update() override
        {
            const double Now = FPlatformTime::Seconds();
            if (StartedAt < 0.0) StartedAt = Now;
            if (Now - StartedAt >= 90.0)
            {
                Test->AddError(TEXT("Navigation contracts exceeded the 90 second real-time bound"));
                if (CurrentStage.IsValid()) EndStage(false, TEXT("whole_suite_deadline"));
                return Finish(TEXT("timeout"));
            }
            UWorld* LiveWorld = GEditor ? GEditor->PlayWorld : nullptr;
            if (Phase == EPhase::AwaitWorld)
            {
                if (Now - StartedAt >= 30.0)
                {
                    Test->AddError(TEXT("Navigation PIE did not begin and advance two seconds within 30 real seconds"));
                    return Finish(TEXT("setup_failed"));
                }
                if (IsValid(LiveWorld) && LiveWorld->WorldType == EWorldType::PIE && LiveWorld->HasBegunPlay())
                {
                    if (!World.IsValid())
                    {
                        World = LiveWorld;
                        InitialGameTime = LiveWorld->GetTimeSeconds();
                    }
                    if (World.Get() != LiveWorld)
                    {
                        Test->AddError(TEXT("Navigation PIE world changed during setup"));
                        return Finish(TEXT("setup_failed"));
                    }
                    if (LiveWorld->GetTimeSeconds() - InitialGameTime >= 2.0)
                    {
                        if (!Setup()) return Finish(TEXT("setup_failed"));
                        Phase = EPhase::AwaitNavigation;
                        PhaseStartedAt = Now;
                    }
                }
                return false;
            }
            if (!World.IsValid() || World.Get() != LiveWorld || !Npc.IsValid()
                || !Player.IsValid() || !AI.IsValid() || !Board.IsValid())
            {
                Test->AddError(TEXT("An actual navigation fixture object became unavailable"));
                if (CurrentStage.IsValid()) EndStage(false, TEXT("actual_world_or_actor_lost"));
                return Finish(TEXT("failed"));
            }
            if (Phase == EPhase::AwaitNavigation)
            {
                FNavLocation Projected;
                if (Nav.IsValid() && !UNavigationSystemV1::IsNavigationBeingBuiltOrLocked(World.Get())
                    && Nav->GetNavDataForProps(Npc->GetNavAgentPropertiesRef(), Npc->GetActorLocation())
                    && Project(Npc->GetActorLocation(), Projected))
                {
                    CaptureNavigationDiagnostics(TEXT("readiness_passed"));
                    Report->SetObjectField(TEXT("initial_projected_npc_location"), Position(Projected.Location));
                    InspectAuthoredHome();
                    BeginStage(TEXT("patrol"), 15.0);
                    Phase = EPhase::Patrol;
                }
                else if (Now - PhaseStartedAt >= 10.0)
                {
                    CaptureNavigationDiagnostics(TEXT("readiness_timeout"));
                    Test->AddError(TEXT("No ready navigation data matching the original NPC agent within 10 seconds"));
                    return Finish(TEXT("navigation_setup_failed"));
                }
                return false;
            }

            Sample(false);
            const double GameElapsed = World->GetTimeSeconds() - StageGameTime;
            const double RealElapsed = Now - PhaseStartedAt;
            if (Phase == EPhase::Patrol)
            {
                const bool bPatrol = GameElapsed >= 2.0 && MaxDisplacement >= 75.0
                    && bSawMovingPath && bSawNonHomePath && bSawPatrolSpeed
                    && Brain.IsValid() && Brain->IsRunning()
                    && !Board->GetValueAsBool(TEXT("IsAtHome"))
                    && !Board->GetValueAsBool(TEXT("IsFollowing"))
                    && !Board->GetValueAsBool(TEXT("IsInteracting"));
                if (bPatrol || RealElapsed >= 15.0)
                {
                    EndStage(bPatrol, bPatrol ? TEXT("observed_original_bt_patrol_movement") : TEXT("patrol_movement_not_observed_within_bound"));
                    if (!BeginFollow()) return Finish(TEXT("failed"));
                }
            }
            else if (Phase == EPhase::Follow)
            {
                const double Distance = FVector::Dist2D(Npc->GetActorLocation(), Player->GetActorLocation());
                const bool bFollow = GameElapsed >= 2.0 && bSawMovingPath && bSawPlayerPath && bSawChaseSpeed
                    && MaxDisplacement >= 75.0 && StartDistance - Distance >= 200.0
                    && Board->GetValueAsBool(TEXT("IsFollowing"))
                    && !Board->GetValueAsBool(TEXT("IsInteracting"))
                    && ReadBool(Npc.Get(), TEXT("IsFollowing"), true);
                if (bFollow || RealElapsed >= 20.0)
                {
                    EndStage(bFollow, bFollow ? TEXT("real_npc_closed_gap_to_relocated_player") : TEXT("follow_contract_not_met_within_bound"));
                    if (!BeginHome()) return Finish(TEXT("failed"));
                }
            }
            else if (Phase == EPhase::Home)
            {
                const double Distance = FVector::Dist2D(Npc->GetActorLocation(), HomeGoal);
                const bool bHome = GameElapsed >= 0.5 && bSawMovingPath && MaxDisplacement >= 100.0
                    && bHomeMoveSucceeded && Distance <= 50.0
                    && Npc->IsHidden() && !Npc->GetActorEnableCollision();
                if (bHome || RealElapsed >= 20.0)
                {
                    EndStage(bHome, bHome ? TEXT("actual_home_move_success_then_hidden_and_collision_disabled") : TEXT("home_move_success_and_hide_not_observed_within_bound"));
                    return Finish(Test->HasAnyErrors() ? TEXT("failed") : TEXT("passed"));
                }
            }
            return false;
        }

    private:
        enum class EPhase { AwaitWorld, AwaitNavigation, Patrol, Follow, Home, Done };

        TSharedPtr<FJsonObject> Checked(const FString& Json, const FString& Operation)
        {
            TSharedPtr<FJsonObject> Result;
            bool bOk = false;
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
            if (!FJsonSerializer::Deserialize(Reader, Result) || !Result.IsValid()
                || !Result->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
            {
                Test->AddError(Operation + TEXT(" failed: ") + Json.Left(800));
                return nullptr;
            }
            return Result;
        }

        TSharedPtr<FJsonValue> Read(UObject* Object, const TCHAR* Name)
        {
            const TSharedPtr<FJsonObject> Result = Checked(UNpcAuditLibrary::ReadProperty(Object, Name), FString(TEXT("Read ")) + Name);
            const TSharedPtr<FJsonObject>* Property = nullptr;
            if (Result.IsValid() && Result->TryGetObjectField(TEXT("property"), Property))
            {
                const TSharedPtr<FJsonValue>* Value = (*Property)->Values.Find(TEXT("value"));
                if (Value) return *Value;
            }
            Test->AddError(FString(TEXT("Navigation property value missing: ")) + Name);
            return nullptr;
        }

        bool Write(UObject* Object, const TCHAR* Name, const TSharedPtr<FJsonValue>& Value)
        {
            return Value.IsValid() && Checked(UNpcAuditLibrary::WriteProperty(Object, Name, Encode(Value)),
                FString(TEXT("Write ")) + Name).IsValid();
        }

        bool Number(UObject* Object, const TCHAR* Name, double Value)
        { return Write(Object, Name, MakeShared<FJsonValueNumber>(Value)); }

        bool Boolean(UObject* Object, const TCHAR* Name, bool Value)
        { return Write(Object, Name, MakeShared<FJsonValueBoolean>(Value)); }

        bool ReadBool(UObject* Object, const TCHAR* Name, bool Expected)
        {
            const TSharedPtr<FJsonValue> Value = Read(Object, Name);
            return Value.IsValid() && Value->Type == EJson::Boolean && Value->AsBool() == Expected;
        }

        bool Call(UObject* Object, const TCHAR* Function, const FString& Arguments = TEXT("{}"))
        {
            return Checked(UNpcAuditLibrary::Invoke(Object, Function, Arguments), FString(TEXT("Invoke ")) + Function).IsValid();
        }

        bool Interact(const TCHAR* Key)
        {
            return Call(Npc.Get(), TEXT("Interact"), FString::Printf(TEXT("{\"InteractionKey\":\"%s\"}"), Key));
        }

        void Snapshot(UObject* Object, std::initializer_list<const TCHAR*> Names)
        {
            for (const TCHAR* Name : Names)
            {
                const TSharedPtr<FJsonValue> Value = Read(Object, Name);
                if (Value.IsValid())
                {
                    FSavedProperty Saved;
                    Saved.Object = Object;
                    Saved.Name = Name;
                    Saved.Value = Value;
                    SavedProperties.Add(MoveTemp(Saved));
                }
            }
        }

        bool Setup()
        {
            UWorld* ActualWorld = World.Get();
            Test->TestEqual(TEXT("Navigation uses the original map"),
                UWorld::RemovePIEPrefix(ActualWorld->GetOutermost()->GetName()), FString(TestMap));
            APlayerController* Controller = ActualWorld->GetFirstPlayerController();
            Player = Controller ? Cast<ACharacter>(Controller->GetPawn()) : nullptr;
            GameMode = ActualWorld->GetAuthGameMode();
            Nav = UNavigationSystemV1::GetCurrent(ActualWorld);
            UClass* NpcClass = LoadClass<ACharacter>(nullptr,
                TEXT("/Game/ThirdPerson/Blueprints/BP_NPC_Standard.BP_NPC_Standard_C"));
            if (NpcClass)
            {
                for (TActorIterator<ACharacter> It(ActualWorld, NpcClass); It; ++It)
                {
                    const TSharedPtr<FJsonValue> Data = Read(*It, TEXT("My_NPC_Data"));
                    FString Name;
                    if (Data.IsValid() && Data->Type == EJson::Object
                        && Data->AsObject()->TryGetStringField(TEXT("NPC_Name"), Name) && Name == TEXT("Charles"))
                        Npc = *It;
                }
            }
            AI = Npc.IsValid() ? Cast<AAIController>(Npc->GetController()) : nullptr;
            Board = AI.IsValid() ? AI->GetBlackboardComponent() : nullptr;
            Brain = AI.IsValid() ? AI->GetBrainComponent() : nullptr;
            PathFollowing = AI.IsValid() ? AI->GetPathFollowingComponent() : nullptr;
            Test->TestNotNull(TEXT("Navigation real player character"), Player.Get());
            Test->TestNotNull(TEXT("Navigation real Charles character"), Npc.Get());
            Test->TestNotNull(TEXT("Navigation actual GameMode"), GameMode.Get());
            Test->TestNotNull(TEXT("Original navigation system"), Nav.Get());
            Test->TestNotNull(TEXT("Original AIController"), AI.Get());
            Test->TestNotNull(TEXT("Original Blackboard"), Board.Get());
            Test->TestNotNull(TEXT("Original BrainComponent"), Brain.Get());
            Test->TestNotNull(TEXT("Original PathFollowingComponent"), PathFollowing.Get());
            if (!Player.IsValid() || !Npc.IsValid() || !GameMode.IsValid() || !Nav.IsValid()
                || !AI.IsValid() || !Board.IsValid() || !Brain.IsValid() || !PathFollowing.IsValid()) return false;

            UBehaviorTreeComponent* Tree = Cast<UBehaviorTreeComponent>(Brain.Get());
            UBehaviorTree* RootTree = Tree ? Tree->GetRootTree() : nullptr;
            Test->TestTrue(TEXT("The actual running behavior tree is original BT_NPC"),
                RootTree && RootTree->GetPathName() == TEXT("/Game/AI/BT_NPC.BT_NPC"));
            Test->TestTrue(TEXT("The original brain is already running"), Brain->IsRunning());
            Report->SetStringField(TEXT("brain_class"), Brain->GetClass()->GetPathName());
            Report->SetStringField(TEXT("behavior_tree"), RootTree ? RootTree->GetPathName() : TEXT("None"));
            Report->SetStringField(TEXT("npc"), Npc->GetPathName());
            Report->SetStringField(TEXT("controller"), AI->GetPathName());
            if (!ReadAuthoredSpeedOracle(RootTree) || !RecordRuntimeSpeedEvidence(TEXT("setup")))
                return false;
            PlayerTransform = Player->GetActorTransform();
            NpcTransform = Npc->GetActorTransform();
            bOriginalNpcHidden = Npc->IsHidden();
            bOriginalNpcCollision = Npc->GetActorEnableCollision();
            Snapshot(GameMode.Get(), {TEXT("CurrentTime"), TEXT("TimeSpeed"), TEXT("ActiveBGM")});
            Snapshot(Player.Get(), {TEXT("PlayerIsInteracting")});
            Snapshot(Npc.Get(), {TEXT("Trust_Level"), TEXT("IsInteracting"), TEXT("IsFollowing"),
                TEXT("InteractionEnd"), TEXT("Dialogue_Widget_Ref"), TEXT("Shop_Widget_Ref"), TEXT("My_NPC_Data")});
            for (const TCHAR* Key : {TEXT("IsFollowing"), TEXT("IsInteracting"), TEXT("IsAtHome")})
                BoardSnapshot.Emplace(Key, Board->GetValueAsBool(Key));
            const TSharedPtr<FJsonValue> Data = Read(Npc.Get(), TEXT("My_NPC_Data"));
            const TSharedPtr<FJsonObject>* Home = nullptr;
            if (!Data.IsValid() || Data->Type != EJson::Object
                || !Data->AsObject()->TryGetObjectField(TEXT("Home_Location"), Home)
                || !(*Home)->TryGetNumberField(TEXT("X"), AuthoredHome.X)
                || !(*Home)->TryGetNumberField(TEXT("Y"), AuthoredHome.Y)
                || !(*Home)->TryGetNumberField(TEXT("Z"), AuthoredHome.Z))
            {
                Test->AddError(TEXT("Charles original home coordinates are unavailable"));
                return false;
            }
            bHasSnapshot = true;
            Number(GameMode.Get(), TEXT("TimeSpeed"), 0.0);
            Number(GameMode.Get(), TEXT("CurrentTime"), 600.0);
            Boolean(Player.Get(), TEXT("PlayerIsInteracting"), false);
            Boolean(Npc.Get(), TEXT("IsInteracting"), false);
            Boolean(Npc.Get(), TEXT("IsFollowing"), false);
            Boolean(Npc.Get(), TEXT("InteractionEnd"), false);
            Board->SetValueAsBool(TEXT("IsInteracting"), false);
            Board->SetValueAsBool(TEXT("IsFollowing"), false);
            Call(Npc.Get(), TEXT("ReceiveTick"), TEXT("{\"DeltaSeconds\":0.0}"));
            CompletionHandle = PathFollowing->OnRequestFinished.AddLambda(
                [this](FAIRequestID RequestId, const FPathFollowingResult& Result)
                {
                    ++MoveCompletionCount;
                    if (Phase == EPhase::Home && Result.Code == EPathFollowingResult::Success)
                        bHomeMoveSucceeded = true;
                    if (MoveCompletions.Num() >= 1000) return;
                    TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
                    Event->SetStringField(TEXT("stage"), StageName);
                    Event->SetStringField(TEXT("request_id"), RequestId.ToString());
                    Event->SetStringField(TEXT("result"), Result.ToString());
                    Event->SetBoolField(TEXT("success"), Result.Code == EPathFollowingResult::Success);
                    Event->SetNumberField(TEXT("real_elapsed_seconds"), FPlatformTime::Seconds() - StartedAt);
                    if (Npc.IsValid()) Event->SetObjectField(TEXT("npc_position"), Position(Npc->GetActorLocation()));
                    MoveCompletions.Add(MakeShared<FJsonValueObject>(Event));
                });
            CaptureNavigationDiagnostics(TEXT("after_setup"));
            return !Test->HasAnyErrors();
        }

        bool TaskSpeed(UObject* Task, const TCHAR* Field, double& Speed,
            TSharedRef<FJsonObject> Evidence)
        {
            const TSharedPtr<FJsonValue> Value = Read(Task, Field);
            const bool bValid = Value.IsValid() && Value->Type == EJson::Number
                && FMath::IsFinite(Value->AsNumber()) && Value->AsNumber() > 0.0;
            Test->TestTrue(FString(TEXT("Actual task has a positive finite speed: ")) + Task->GetPathName(), bValid);
            Evidence->SetStringField(TEXT("task_object"), Task->GetPathName());
            Evidence->SetStringField(TEXT("task_class"), Task->GetClass()->GetPathName());
            Evidence->SetStringField(TEXT("parameter"), Field);
            Evidence->SetField(TEXT("actual_instance_value"), Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
            const TSharedPtr<FJsonValue> Default = Read(Task->GetClass()->GetDefaultObject(), Field);
            Evidence->SetField(TEXT("class_default_value"), Default.IsValid() ? Default : MakeShared<FJsonValueNull>());
            if (bValid) Speed = Value->AsNumber();
            return bValid;
        }

        /** Derive expectations from authored tree nodes, never from observed walking speed. */
        bool ReadAuthoredSpeedOracle(UBehaviorTree* Asset)
        {
            if (!Asset || !Asset->RootNode)
            {
                Test->AddError(TEXT("Original BT_NPC authored root is unavailable for speed oracle"));
                return false;
            }
            TSharedRef<FJsonObject> Oracle = MakeShared<FJsonObject>();
            Oracle->SetStringField(TEXT("tree"), Asset->GetPathName());
            Oracle->SetStringField(TEXT("root_node"), Asset->RootNode->GetPathName());
            Oracle->SetStringField(TEXT("source"), TEXT("Actual loaded BT_NPC RootNode -> Children.ChildComposite/ChildTask traversal. Blueprint task class defaults can differ from authored task-instance overrides; expected values come from the tree instance, not the CDO or movement observation."));
            TArray<TSharedPtr<FJsonValue>> Composites;
            TArray<TSharedPtr<FJsonValue>> Tasks;
            TArray<UBTCompositeNode*> Pending;
            TSet<UBTCompositeNode*> Visited;
            Pending.Add(Asset->RootNode);
            int32 PatrolCount = 0;
            int32 ChaseCount = 0;
            bool bValid = true;
            while (!Pending.IsEmpty() && Visited.Num() < 128)
            {
                UBTCompositeNode* Composite = Pending.Pop();
                if (!Composite || Visited.Contains(Composite)) continue;
                Visited.Add(Composite);
                TSharedRef<FJsonObject> CompositeEvidence = MakeShared<FJsonObject>();
                CompositeEvidence->SetStringField(TEXT("object"), Composite->GetPathName());
                TArray<TSharedPtr<FJsonValue>> Children;
                for (int32 Index = 0; Index < Composite->Children.Num(); ++Index)
                {
                    const FBTCompositeChild& Child = Composite->Children[Index];
                    TSharedRef<FJsonObject> ChildEvidence = MakeShared<FJsonObject>();
                    ChildEvidence->SetNumberField(TEXT("index"), Index);
                    ChildEvidence->SetStringField(TEXT("child_composite"), Child.ChildComposite ? Child.ChildComposite->GetPathName() : TEXT("None"));
                    ChildEvidence->SetStringField(TEXT("child_task"), Child.ChildTask ? Child.ChildTask->GetPathName() : TEXT("None"));
                    Children.Add(MakeShared<FJsonValueObject>(ChildEvidence));
                    if (Child.ChildComposite) Pending.Add(Child.ChildComposite);
                    UBTTaskNode* Task = Child.ChildTask;
                    if (!Task) continue;
                    const FString Class = Task->GetClass()->GetPathName();
                    const bool bPatrol = Class == TEXT("/Game/AI/BTTask_PatrolRandom.BTTask_PatrolRandom_C");
                    const bool bChase = Class == TEXT("/Game/AI/BTTask_ChasePlayer.BTTask_ChasePlayer_C");
                    if (!bPatrol && !bChase) continue;
                    TSharedRef<FJsonObject> TaskEvidence = MakeShared<FJsonObject>();
                    TaskEvidence->SetStringField(TEXT("parent_composite"), Composite->GetPathName());
                    TaskEvidence->SetNumberField(TEXT("child_index"), Index);
                    bValid &= TaskSpeed(Task, bPatrol ? TEXT("Patrol Speed") : TEXT("ChaseSpeed"),
                        bPatrol ? ExpectedPatrolSpeed : ExpectedChaseSpeed, TaskEvidence);
                    PatrolCount += bPatrol ? 1 : 0;
                    ChaseCount += bChase ? 1 : 0;
                    Tasks.Add(MakeShared<FJsonValueObject>(TaskEvidence));
                }
                CompositeEvidence->SetArrayField(TEXT("children"), Children);
                Composites.Add(MakeShared<FJsonValueObject>(CompositeEvidence));
            }
            Test->TestTrue(TEXT("Authored task traversal completed within its node bound"), Pending.IsEmpty());
            Test->TestEqual(TEXT("Exactly one authored patrol-speed task supplies the oracle"), PatrolCount, 1);
            Test->TestEqual(TEXT("Exactly one authored chase-speed task supplies the oracle"), ChaseCount, 1);
            Oracle->SetArrayField(TEXT("composite_traversal"), Composites);
            Oracle->SetArrayField(TEXT("speed_task_instances"), Tasks);
            Oracle->SetNumberField(TEXT("expected_patrol_speed"), ExpectedPatrolSpeed);
            Oracle->SetNumberField(TEXT("expected_chase_speed"), ExpectedChaseSpeed);
            Report->SetObjectField(TEXT("authored_speed_oracle"), Oracle);
            return bValid && Pending.IsEmpty() && PatrolCount == 1 && ChaseCount == 1;
        }

        bool RecordRuntimeSpeedEvidence(const TCHAR* Checkpoint)
        {
            UBehaviorTreeComponent* Tree = Cast<UBehaviorTreeComponent>(Brain.Get());
            if (!Tree)
            {
                Test->AddError(TEXT("Runtime tree component unavailable for speed-instance evidence"));
                return false;
            }
            const TSharedPtr<FJsonValue> Instances = Read(Tree, TEXT("NodeInstances"));
            TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
            Evidence->SetStringField(TEXT("checkpoint"), Checkpoint);
            Evidence->SetStringField(TEXT("brain"), Tree->GetPathName());
            Evidence->SetField(TEXT("actual_NodeInstances_property"), Instances.IsValid() ? Instances : MakeShared<FJsonValueNull>());
            TArray<TSharedPtr<FJsonValue>> Tasks;
            int32 PatrolCount = 0;
            int32 ChaseCount = 0;
            bool bValid = Instances.IsValid() && Instances->Type == EJson::Array;
            if (bValid)
            {
                for (const TSharedPtr<FJsonValue>& Instance : Instances->AsArray())
                {
                    UObject* Task = Instance.IsValid() && Instance->Type == EJson::String
                        ? FindObject<UObject>(nullptr, *Instance->AsString()) : nullptr;
                    if (!Task) continue;
                    const FString Class = Task->GetClass()->GetPathName();
                    const bool bPatrol = Class == TEXT("/Game/AI/BTTask_PatrolRandom.BTTask_PatrolRandom_C");
                    const bool bChase = Class == TEXT("/Game/AI/BTTask_ChasePlayer.BTTask_ChasePlayer_C");
                    if (!bPatrol && !bChase) continue;
                    double ActualSpeed = 0.0;
                    TSharedRef<FJsonObject> TaskEvidence = MakeShared<FJsonObject>();
                    const double Expected = bPatrol ? ExpectedPatrolSpeed : ExpectedChaseSpeed;
                    const bool bRead = TaskSpeed(Task, bPatrol ? TEXT("Patrol Speed") : TEXT("ChaseSpeed"), ActualSpeed, TaskEvidence);
                    const bool bMatches = bRead && FMath::IsNearlyEqual(ActualSpeed, Expected, 0.01);
                    TaskEvidence->SetNumberField(TEXT("authored_expected_speed"), Expected);
                    TaskEvidence->SetBoolField(TEXT("matches_authored_tree_instance"), bMatches);
                    Tasks.Add(MakeShared<FJsonValueObject>(TaskEvidence));
                    Test->TestTrue(FString(Checkpoint) + TEXT(" runtime task speed matches its authored tree instance"), bMatches);
                    bValid &= bMatches;
                    PatrolCount += bPatrol ? 1 : 0;
                    ChaseCount += bChase ? 1 : 0;
                }
            }
            Test->TestTrue(FString(Checkpoint) + TEXT(" runtime patrol and chase task instances were both inspected"),
                PatrolCount >= 1 && ChaseCount >= 1);
            Evidence->SetArrayField(TEXT("speed_task_instances"), Tasks);
            Evidence->SetNumberField(TEXT("patrol_task_instance_count"), PatrolCount);
            Evidence->SetNumberField(TEXT("chase_task_instance_count"), ChaseCount);
            Evidence->SetBoolField(TEXT("matches_authored_tree_instances"), bValid && PatrolCount >= 1 && ChaseCount >= 1);
            RuntimeSpeedEvidence.Add(MakeShared<FJsonValueObject>(Evidence));
            return bValid && PatrolCount >= 1 && ChaseCount >= 1;
        }

        /** Diagnostic queries never rebuild data or relax the readiness gate. */
        void CaptureNavigationDiagnostics(const TCHAR* Label)
        {
            TSharedRef<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
            Diagnostic->SetStringField(TEXT("checkpoint"), Label);
            Diagnostic->SetNumberField(TEXT("real_elapsed_seconds"), FPlatformTime::Seconds() - StartedAt);
            Diagnostic->SetStringField(TEXT("query_semantics"), TEXT("Unmodified engine GetNavDataForProps and ProjectPointToNavigation with the NPC's actual agent properties. The test extent is100/100/500; this is not a custom strict radius/height-equality filter."));
            Diagnostic->SetObjectField(TEXT("test_query_extent"), Position(FVector(100.0, 100.0, 500.0)));
            if (!World.IsValid() || !Nav.IsValid() || !Npc.IsValid())
            {
                Diagnostic->SetBoolField(TEXT("required_objects_available"), false);
                NavigationDiagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
                return;
            }
            Diagnostic->SetBoolField(TEXT("required_objects_available"), true);
            const FNavAgentProperties& Properties = Npc->GetNavAgentPropertiesRef();
            Diagnostic->SetObjectField(TEXT("npc_nav_agent"), Agent(Properties));
            Diagnostic->SetObjectField(TEXT("npc_actor_position"), Position(Npc->GetActorLocation()));
            Diagnostic->SetObjectField(TEXT("npc_nav_agent_location"), Position(Npc->GetNavAgentLocation()));
            Diagnostic->SetNumberField(TEXT("npc_capsule_radius"), Npc->GetCapsuleComponent()->GetScaledCapsuleRadius());
            Diagnostic->SetNumberField(TEXT("npc_capsule_total_height"), Npc->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 2.0);
            Diagnostic->SetBoolField(TEXT("navigation_build_in_progress"), UNavigationSystemV1::IsNavigationBeingBuilt(World.Get()));
            Diagnostic->SetBoolField(TEXT("navigation_build_or_lock_in_progress"), UNavigationSystemV1::IsNavigationBeingBuiltOrLocked(World.Get()));
            Diagnostic->SetObjectField(TEXT("navigable_world_bounds"), Bounds(Nav->GetNavigableWorldBounds()));
            ANavigationData* Selected = Nav->GetNavDataForProps(Properties, Npc->GetActorLocation());
            ANavigationData* Main = Nav->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
            Diagnostic->SetStringField(TEXT("selected_nav_data"), Selected ? Selected->GetPathName() : TEXT("None"));
            Diagnostic->SetStringField(TEXT("main_nav_data"), Main ? Main->GetPathName() : TEXT("None"));
            Diagnostic->SetNumberField(TEXT("registered_nav_data_count"), Nav->NavDataSet.Num());
            FNavLocation ActorProjection;
            const bool bActorProjects = Project(Npc->GetActorLocation(), ActorProjection);
            Diagnostic->SetBoolField(TEXT("actor_position_projects_with_test_query"), bActorProjects);
            if (bActorProjects) Diagnostic->SetObjectField(TEXT("actor_projected_position"), Position(ActorProjection.Location));
            FNavLocation AgentProjection;
            const bool bAgentProjects = Project(Npc->GetNavAgentLocation(), AgentProjection);
            Diagnostic->SetBoolField(TEXT("agent_location_projects_with_test_query"), bAgentProjects);
            if (bAgentProjects) Diagnostic->SetObjectField(TEXT("agent_projected_position"), Position(AgentProjection.Location));
            if (Selected)
            {
                Diagnostic->SetObjectField(TEXT("selected_nav_data_config"), Config(Selected->GetConfig()));
                FNavLocation DefaultProjection;
                const bool bDefaultProjects = Nav->ProjectPointToNavigation(Npc->GetActorLocation(), DefaultProjection,
                    Selected->GetDefaultQueryExtent(), Selected);
                Diagnostic->SetBoolField(TEXT("actor_position_projects_with_selected_default_extent"), bDefaultProjects);
                if (bDefaultProjects) Diagnostic->SetObjectField(TEXT("default_extent_projected_position"), Position(DefaultProjection.Location));
            }
            TArray<TSharedPtr<FJsonValue>> Supported;
            for (const FNavDataConfig& SupportedAgent : Nav->GetSupportedAgents())
                Supported.Add(MakeShared<FJsonValueObject>(Config(SupportedAgent)));
            Diagnostic->SetArrayField(TEXT("supported_agents"), Supported);
            TArray<TSharedPtr<FJsonValue>> Available;
            for (TActorIterator<ANavigationData> It(World.Get()); It; ++It)
            {
                ANavigationData* Data = *It;
                TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("object"), Data->GetPathName());
                Entry->SetStringField(TEXT("class"), Data->GetClass()->GetPathName());
                Entry->SetBoolField(TEXT("selected_for_actual_npc"), Data == Selected);
                Entry->SetBoolField(TEXT("is_main_nav_data"), Data == Main);
                Entry->SetBoolField(TEXT("registered"), Nav->NavDataSet.Contains(Data));
                Entry->SetBoolField(TEXT("supports_actual_npc_agent"), Data->DoesSupportAgent(Properties));
                Entry->SetObjectField(TEXT("config"), Config(Data->GetConfig()));
                Entry->SetObjectField(TEXT("bounds"), Bounds(Data->GetBounds()));
                Entry->SetNumberField(TEXT("runtime_generation_enum"), static_cast<int32>(Data->GetRuntimeGenerationMode()));
#if WITH_RECAST
                if (const ARecastNavMesh* Recast = Cast<ARecastNavMesh>(Data))
                {
                    Entry->SetBoolField(TEXT("has_valid_navmesh"), Recast->HasValidNavmesh());
                    Entry->SetObjectField(TEXT("navmesh_bounds"), Bounds(Recast->GetNavMeshBounds()));
                    const int32 Slots = Recast->GetNavMeshTilesCount();
                    const int32 InspectedSlots = FMath::Min(Slots, 4096);
                    int32 ValidTileBounds = 0;
                    for (int32 Index = 0; Index < InspectedSlots; ++Index)
                        ValidTileBounds += Recast->GetNavMeshTileBounds(Index).IsValid ? 1 : 0;
                    Entry->SetNumberField(TEXT("allocated_tile_slots"), Slots);
                    Entry->SetStringField(TEXT("tile_slot_semantics"), TEXT("Engine GetNavMeshTilesCount counts allocated slots, not traversable polygons. Valid tile bounds are separately counted over at most4096 inspected slots."));
                    Entry->SetNumberField(TEXT("tile_slots_inspected"), InspectedSlots);
                    Entry->SetNumberField(TEXT("inspected_slots_with_valid_tile_bounds"), ValidTileBounds);
                    Entry->SetBoolField(TEXT("tile_slot_inspection_truncated"), InspectedSlots < Slots);
                    TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
                    for (const TCHAR* Name : {TEXT("TilePoolSize"), TEXT("bFixedTilePoolSize"), TEXT("TileNumberHardLimit"),
                        TEXT("TileSizeUU"), TEXT("AgentRadius"), TEXT("AgentHeight"), TEXT("bIsWorldPartitioned"), TEXT("RuntimeGeneration")})
                    {
                        const FProperty* Property = FindFProperty<FProperty>(Data->GetClass(), Name);
                        if (Property)
                        {
                            FString Value;
                            Property->ExportTextItem_Direct(Value, Property->ContainerPtrToValuePtr<void>(Data), nullptr, Data, PPF_None);
                            Settings->SetStringField(Name, Value);
                        }
                    }
                    Entry->SetObjectField(TEXT("configuration_export_text"), Settings);
                }
#endif
                Available.Add(MakeShared<FJsonValueObject>(Entry));
            }
            Diagnostic->SetArrayField(TEXT("available_navigation_actors"), Available);
            NavigationDiagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
        }

        bool Project(const FVector& Point, FNavLocation& Result) const
        {
            return Nav.IsValid() && Npc.IsValid() && Nav->ProjectPointToNavigation(Point, Result,
                FVector(100.0, 100.0, 500.0), &Npc->GetNavAgentPropertiesRef());
        }

        UNavigationPath* PathTo(const FVector& Goal) const
        {
            return UNavigationSystemV1::FindPathToLocationSynchronously(
                World.Get(), Npc->GetActorLocation(), Goal, AI.Get());
        }

        void InspectAuthoredHome()
        {
            FNavLocation Projected;
            const bool bProjected = Project(AuthoredHome, Projected);
            UNavigationPath* Path = bProjected ? PathTo(Projected.Location) : nullptr;
            TSharedRef<FJsonObject> Original = MakeShared<FJsonObject>();
            Original->SetStringField(TEXT("oracle"), TEXT("Original DT_NPC_Data NPC_01 Home_Location; query only, not an arrival test."));
            Original->SetObjectField(TEXT("authored_position"), Position(AuthoredHome));
            Original->SetBoolField(TEXT("projects_to_navigation"), bProjected);
            if (bProjected) Original->SetObjectField(TEXT("projected_position"), Position(Projected.Location));
            Original->SetBoolField(TEXT("complete_path_exists_from_initial_npc"), Path && Path->IsValid() && !Path->IsPartial());
            if (Path) Original->SetNumberField(TEXT("queried_path_length"), Path->GetPathLength());
            Report->SetObjectField(TEXT("original_home_query"), Original);
        }

        bool ReachableRing(double Minimum, double Maximum, FVector& Goal, bool bRelocatePlayer)
        {
            const FVector Origin = Npc->GetActorLocation();
            const FVector AwayFromPlayer = Origin - Player->GetActorLocation();
            const double BaseAngle = bRelocatePlayer ? 0.0 : FMath::Atan2(AwayFromPlayer.Y, AwayFromPlayer.X);
            for (double Radius : {Maximum, (Minimum + Maximum) * 0.5, Minimum})
            {
                for (int32 Step = 0; Step < 16; ++Step)
                {
                    const double Angle = BaseAngle + Step * (2.0 * PI / 16.0);
                    const FVector Candidate = Origin + FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, 0.0);
                    FNavLocation Projected;
                    if (!Project(Candidate, Projected)) continue;
                    const double Distance = FVector::Dist2D(Origin, Projected.Location);
                    if (Distance < Minimum - 1.0 || Distance > Maximum + 1.0) continue;
                    UNavigationPath* Path = PathTo(Projected.Location);
                    if (!Path || !Path->IsValid() || Path->IsPartial() || Path->PathPoints.Num() < 2) continue;
                    if (bRelocatePlayer)
                    {
                        const double HalfHeight = Player->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
                        const FVector Centre = Projected.Location + FVector(0.0, 0.0, HalfHeight + 2.0);
                        if (!Player->TeleportTo(Centre, Player->GetActorRotation(), false, false)) continue;
                        Player->GetCharacterMovement()->StopMovementImmediately();
                        Player->ConsumeMovementInputVector();
                        // TeleportTo may adjust an encroaching location. Validate
                        // the resulting actor position, not just its proposed goal.
                        FNavLocation ActualPlayerPoint;
                        const double ActualDistance = FVector::Dist2D(Origin, Player->GetActorLocation());
                        if (ActualDistance < Minimum - 1.0 || ActualDistance > Maximum + 1.0
                            || !Project(Player->GetActorLocation(), ActualPlayerPoint)) continue;
                        Path = PathTo(ActualPlayerPoint.Location);
                        if (!Path || !Path->IsValid() || Path->IsPartial() || Path->PathPoints.Num() < 2) continue;
                        Projected = ActualPlayerPoint;
                    }
                    Goal = Projected.Location;
                    FixturePathLength = Path->GetPathLength();
                    FixturePathPointCount = Path->PathPoints.Num();
                    return true;
                }
            }
            return false;
        }

        bool OpenDialogue()
        {
            const bool bCalled = Interact(TEXT("Interact"));
            const TSharedPtr<FJsonValue> Ref = Read(Npc.Get(), TEXT("Dialogue_Widget_Ref"));
            UUserWidget* Dialogue = Ref.IsValid() && Ref->Type == EJson::String
                ? FindObject<UUserWidget>(nullptr, *Ref->AsString()) : nullptr;
            const bool bOpened = bCalled && ReadBool(Npc.Get(), TEXT("IsInteracting"), true)
                && ReadBool(Player.Get(), TEXT("PlayerIsInteracting"), true)
                && Dialogue && Dialogue->IsInViewport();
            if (Dialogue) OpenedDialogues.Add(Dialogue);
            Test->TestTrue(TEXT("Actual Interact opens Charles dialogue"), bOpened);
            return bOpened;
        }

        bool CloseDialogue()
        {
            const TSharedPtr<FJsonValue> Ref = Read(Npc.Get(), TEXT("Dialogue_Widget_Ref"));
            UUserWidget* Dialogue = Ref.IsValid() && Ref->Type == EJson::String
                ? FindObject<UUserWidget>(nullptr, *Ref->AsString()) : nullptr;
            const bool bClosed = Interact(TEXT("Interact"))
                && ReadBool(Npc.Get(), TEXT("IsInteracting"), false)
                && ReadBool(Player.Get(), TEXT("PlayerIsInteracting"), false)
                && !Board->GetValueAsBool(TEXT("IsInteracting"))
                && Dialogue && !Dialogue->IsInViewport();
            Test->TestTrue(TEXT("Actual Interact closes Charles dialogue before navigation"), bClosed);
            return bClosed;
        }

        bool BeginFollow()
        {
            if (!Number(Npc.Get(), TEXT("Trust_Level"), 80.0) || !OpenDialogue()
                || !Interact(TEXT("Follow"))) return SetupStageFailure(TEXT("follow"), TEXT("original_follow_input_failed"));
            const bool bFollowing = ReadBool(Npc.Get(), TEXT("IsFollowing"), true)
                && Board->GetValueAsBool(TEXT("IsFollowing"));
            Test->TestTrue(TEXT("Original Follow input enables actor and Blackboard state at trust80"), bFollowing);
            if (!bFollowing || !CloseDialogue()) return SetupStageFailure(TEXT("follow"), TEXT("original_follow_or_close_state_invalid"));
            FVector ProjectedPlayerGoal;
            if (!ReachableRing(600.0, 800.0, ProjectedPlayerGoal, true))
                return SetupStageFailure(TEXT("follow"), TEXT("no_reachable_player_fixture_600_to_800_units"));
            BeginStage(TEXT("follow"), 20.0);
            Phase = EPhase::Follow;
            StartDistance = FVector::Dist2D(Npc->GetActorLocation(), Player->GetActorLocation());
            CurrentStage->SetStringField(TEXT("oracle"), TEXT("Original Follow input at NPC_01 threshold80, original BT chase, real world-tick movement; require >=200 unit gap reduction, >=75 unit NPC displacement, observed moving path ending near the player, authored BT_NPC chase-task instance speed, and actor/BB follow state."));
            CurrentStage->SetNumberField(TEXT("expected_walk_speed_from_authored_task"), ExpectedChaseSpeed);
            CurrentStage->SetStringField(TEXT("target_setup"), TEXT("Player TeleportTo collision-checked near a navigation-projected point; no NPC movement command is issued by the harness."));
            CurrentStage->SetObjectField(TEXT("target_projected_position"), Position(ProjectedPlayerGoal));
            CurrentStage->SetObjectField(TEXT("target_actor_position"), Position(Player->GetActorLocation()));
            CurrentStage->SetNumberField(TEXT("initial_distance_2d"), StartDistance);
            CurrentStage->SetNumberField(TEXT("queried_path_length"), FixturePathLength);
            CurrentStage->SetNumberField(TEXT("queried_path_points"), FixturePathPointCount);
            return true;
        }

        bool BeginHome()
        {
            // Exercise the real follow-off input and close flow. If an original
            // gate prevents that transition, report a failure rather than forcing
            // a false-positive home arrival by silently changing follow state.
            if (!OpenDialogue() || !Interact(TEXT("Follow")))
                return SetupStageFailure(TEXT("home"), TEXT("original_follow_off_input_failed"));
            const bool bStopped = ReadBool(Npc.Get(), TEXT("IsFollowing"), false)
                && !Board->GetValueAsBool(TEXT("IsFollowing"));
            Test->TestTrue(TEXT("Original Follow input disables following before home phase"), bStopped);
            if (!bStopped || !CloseDialogue())
                return SetupStageFailure(TEXT("home"), TEXT("original_follow_off_or_close_state_invalid"));
            if (!ReachableRing(200.0, 400.0, HomeGoal, false))
                return SetupStageFailure(TEXT("home"), TEXT("no_reachable_home_fixture_200_to_400_units"));
            const TSharedPtr<FJsonValue> Data = Read(Npc.Get(), TEXT("My_NPC_Data"));
            if (!Data.IsValid() || Data->Type != EJson::Object)
                return SetupStageFailure(TEXT("home"), TEXT("actual_npc_home_struct_unavailable"));
            Data->AsObject()->SetObjectField(TEXT("Home_Location"), Position(HomeGoal));
            if (!Write(Npc.Get(), TEXT("My_NPC_Data"), Data))
                return SetupStageFailure(TEXT("home"), TEXT("in_memory_home_fixture_could_not_be_set"));
            BeginStage(TEXT("home"), 20.0);
            Phase = EPhase::Home;
            bHomeMoveSucceeded = false;
            StartDistance = FVector::Dist2D(Npc->GetActorLocation(), HomeGoal);
            CurrentStage->SetStringField(TEXT("oracle"), TEXT("Original scheduled off-work ReceiveTick -> original AI MoveTo -> actual Success callback -> original actor hide/collision-off branch. Require >=100 unit displacement and <=50 unit final 2D distance; original MoveTo acceptance radius is10 and no harness hide or NPC MoveTo is used."));
            CurrentStage->SetStringField(TEXT("fixture_scope"), TEXT("My_NPC_Data.Home_Location is temporarily replaced by a verified reachable nearby point. Authored home validity is separately queried and not inferred from this fixture."));
            CurrentStage->SetObjectField(TEXT("fixture_home_position"), Position(HomeGoal));
            CurrentStage->SetObjectField(TEXT("authored_home_position"), Position(AuthoredHome));
            CurrentStage->SetNumberField(TEXT("initial_distance_2d"), StartDistance);
            CurrentStage->SetNumberField(TEXT("queried_path_length"), FixturePathLength);
            CurrentStage->SetNumberField(TEXT("queried_path_points"), FixturePathPointCount);
            if (Npc->IsHidden() || !Npc->GetActorEnableCollision())
                return SetupStageFailure(TEXT("home"), TEXT("home_phase_must_start_visible_with_collision"));
            if (!Number(GameMode.Get(), TEXT("CurrentTime"), 1140.0)
                || !Call(Npc.Get(), TEXT("ReceiveTick"), TEXT("{\"DeltaSeconds\":0.0}")))
                return SetupStageFailure(TEXT("home"), TEXT("original_home_tick_failed"));
            CurrentStage->SetStringField(TEXT("first_home_request_id"), AI->GetCurrentMoveRequestID().ToString());
            return true;
        }

        void BeginStage(const FString& Name, double Deadline)
        {
            StageName = Name;
            PhaseStartedAt = FPlatformTime::Seconds();
            StageGameTime = World->GetTimeSeconds();
            StartPosition = Npc->GetActorLocation();
            MaxDisplacement = 0.0;
            bSawMovingPath = false;
            bSawNonHomePath = false;
            bSawPatrolSpeed = false;
            bSawChaseSpeed = false;
            bSawPlayerPath = false;
            LastSampleAt = -1.0;
            Samples.Reset();
            CurrentStage = MakeShared<FJsonObject>();
            CurrentStage->SetStringField(TEXT("id"), Name);
            CurrentStage->SetNumberField(TEXT("wait_limit_seconds"), Deadline);
            CurrentStage->SetObjectField(TEXT("initial_npc_position"), Position(StartPosition));
            if (Name == TEXT("patrol"))
            {
                CurrentStage->SetStringField(TEXT("oracle"), TEXT("Actual original BT_NPC running at time600, no follow/interaction/home flag. Require >=2 game seconds, >=75 unit displacement, moving valid path away from authored home, and the authored BT_NPC patrol-task instance speed. No harness NPC MoveTo is issued."));
                CurrentStage->SetNumberField(TEXT("expected_walk_speed_from_authored_task"), ExpectedPatrolSpeed);
            }
        }

        bool SetupStageFailure(const FString& Name, const FString& Reason)
        {
            if (!CurrentStage.IsValid()) BeginStage(Name, 0.0);
            EndStage(false, Reason);
            return false;
        }

        void Sample(bool bForce)
        {
            if (!CurrentStage.IsValid() || !World.IsValid() || !Npc.IsValid() || !Player.IsValid()
                || !Board.IsValid() || !AI.IsValid() || !PathFollowing.IsValid()) return;
            const double Now = FPlatformTime::Seconds();
            MaxDisplacement = FMath::Max(MaxDisplacement, FVector::Dist2D(StartPosition, Npc->GetActorLocation()));
            if (!bForce && LastSampleAt >= 0.0 && Now - LastSampleAt < 0.25) return;
            LastSampleAt = Now;
            TSharedRef<FJsonObject> SampleRow = MakeShared<FJsonObject>();
            SampleRow->SetNumberField(TEXT("real_elapsed_seconds"), Now - PhaseStartedAt);
            SampleRow->SetNumberField(TEXT("game_elapsed_seconds"), World->GetTimeSeconds() - StageGameTime);
            SampleRow->SetObjectField(TEXT("npc_position"), Position(Npc->GetActorLocation()));
            SampleRow->SetObjectField(TEXT("player_position"), Position(Player->GetActorLocation()));
            SampleRow->SetNumberField(TEXT("distance_to_player_2d"), FVector::Dist2D(Npc->GetActorLocation(), Player->GetActorLocation()));
            SampleRow->SetNumberField(TEXT("distance_to_authored_home_2d"), FVector::Dist2D(Npc->GetActorLocation(), AuthoredHome));
            if (Phase == EPhase::Home)
                SampleRow->SetNumberField(TEXT("distance_to_fixture_home_2d"), FVector::Dist2D(Npc->GetActorLocation(), HomeGoal));
            SampleRow->SetNumberField(TEXT("max_displacement_2d"), MaxDisplacement);
            SampleRow->SetBoolField(TEXT("hidden"), Npc->IsHidden());
            SampleRow->SetBoolField(TEXT("collision_enabled"), Npc->GetActorEnableCollision());
            SampleRow->SetBoolField(TEXT("brain_running"), Brain.IsValid() && Brain->IsRunning());
            SampleRow->SetBoolField(TEXT("bb_following"), Board->GetValueAsBool(TEXT("IsFollowing")));
            SampleRow->SetBoolField(TEXT("bb_interacting"), Board->GetValueAsBool(TEXT("IsInteracting")));
            SampleRow->SetBoolField(TEXT("bb_at_home"), Board->GetValueAsBool(TEXT("IsAtHome")));
            const double WalkSpeed = Npc->GetCharacterMovement()->MaxWalkSpeed;
            SampleRow->SetNumberField(TEXT("max_walk_speed"), WalkSpeed);
            bSawPatrolSpeed |= Phase == EPhase::Patrol && FMath::IsNearlyEqual(WalkSpeed, ExpectedPatrolSpeed, 0.01);
            bSawChaseSpeed |= Phase == EPhase::Follow && FMath::IsNearlyEqual(WalkSpeed, ExpectedChaseSpeed, 0.01);
            SampleRow->SetStringField(TEXT("path_status"), MoveStatus(AI->GetMoveStatus()));
            SampleRow->SetStringField(TEXT("move_request_id"), AI->GetCurrentMoveRequestID().ToString());
            const FNavPathSharedPtr Path = PathFollowing->GetPath();
            const bool bPathValid = Path.IsValid() && Path->IsValid();
            SampleRow->SetBoolField(TEXT("path_valid"), bPathValid);
            if (bPathValid)
            {
                const FVector End = Path->GetEndLocation();
                SampleRow->SetObjectField(TEXT("path_end"), Position(End));
                SampleRow->SetNumberField(TEXT("path_points"), Path->GetPathPoints().Num());
                SampleRow->SetBoolField(TEXT("path_partial"), Path->IsPartial());
                bSawMovingPath |= AI->GetMoveStatus() == EPathFollowingStatus::Moving && !Path->IsPartial();
                bSawNonHomePath |= AI->GetMoveStatus() == EPathFollowingStatus::Moving
                    && !Path->IsPartial() && FVector::Dist2D(End, AuthoredHome) > 100.0;
                bSawPlayerPath |= AI->GetMoveStatus() == EPathFollowingStatus::Moving
                    && !Path->IsPartial() && FVector::Dist2D(End, Player->GetActorLocation()) <= 100.0;
            }
            UBehaviorTreeComponent* Tree = Cast<UBehaviorTreeComponent>(Brain.Get());
            const UBTNode* Active = Tree ? Tree->GetActiveNode() : nullptr;
            if (Active)
            {
                SampleRow->SetStringField(TEXT("active_bt_node"), Active->GetNodeName());
                SampleRow->SetStringField(TEXT("active_bt_node_class"), Active->GetClass()->GetPathName());
            }
            if (Samples.Num() < 400) Samples.Add(MakeShared<FJsonValueObject>(SampleRow));
        }

        void EndStage(bool bPassed, const FString& Reason)
        {
            if (!CurrentStage.IsValid()) return;
            Sample(true);
            const bool bRuntimeSpeedsMatch = RecordRuntimeSpeedEvidence(*StageName);
            bPassed &= bRuntimeSpeedsMatch;
            CurrentStage->SetBoolField(TEXT("passed"), bPassed);
            CurrentStage->SetBoolField(TEXT("runtime_task_speeds_match_authored_oracle"), bRuntimeSpeedsMatch);
            CurrentStage->SetStringField(TEXT("reason"), Reason);
            CurrentStage->SetNumberField(TEXT("real_elapsed_seconds"), FPlatformTime::Seconds() - PhaseStartedAt);
            if (World.IsValid())
                CurrentStage->SetNumberField(TEXT("game_elapsed_seconds"), World->GetTimeSeconds() - StageGameTime);
            if (Npc.IsValid())
                CurrentStage->SetObjectField(TEXT("final_npc_position"), Position(Npc->GetActorLocation()));
            CurrentStage->SetNumberField(TEXT("max_displacement_2d"), MaxDisplacement);
            CurrentStage->SetBoolField(TEXT("observed_moving_complete_path"), bSawMovingPath);
            CurrentStage->SetBoolField(TEXT("observed_non_authored_home_path"), bSawNonHomePath);
            CurrentStage->SetBoolField(TEXT("observed_authored_patrol_speed"), bSawPatrolSpeed);
            CurrentStage->SetBoolField(TEXT("observed_authored_chase_speed"), bSawChaseSpeed);
            CurrentStage->SetBoolField(TEXT("observed_path_ending_near_player"), bSawPlayerPath);
            CurrentStage->SetBoolField(TEXT("observed_home_move_success_callback"), bHomeMoveSucceeded);
            CurrentStage->SetArrayField(TEXT("samples"), Samples);
            if (StageName == TEXT("follow") && Npc.IsValid() && Player.IsValid())
            {
                const double FinalDistance = FVector::Dist2D(Npc->GetActorLocation(), Player->GetActorLocation());
                CurrentStage->SetNumberField(TEXT("final_distance_to_player_2d"), FinalDistance);
                CurrentStage->SetNumberField(TEXT("distance_reduction_2d"), StartDistance - FinalDistance);
            }
            Stages.Add(MakeShared<FJsonValueObject>(CurrentStage));
            Test->TestTrue(TEXT("Navigation ") + StageName + TEXT(": ") + Reason, bPassed);
            CurrentStage.Reset();
        }

        void DetachObserver()
        {
            if (PathFollowing.IsValid() && CompletionHandle.IsValid())
                PathFollowing->OnRequestFinished.Remove(CompletionHandle);
            CompletionHandle.Reset();
        }

        bool Finish(const FString& Status)
        {
            if (Phase == EPhase::Done) return true;
            DetachObserver();
            Report->SetStringField(TEXT("status"), Status);
            Report->SetNumberField(TEXT("real_elapsed_seconds"), FPlatformTime::Seconds() - StartedAt);
            Report->SetNumberField(TEXT("completed_stages"), Stages.Num());
            Report->SetArrayField(TEXT("stages"), Stages);
            Report->SetArrayField(TEXT("navigation_diagnostics"), NavigationDiagnostics);
            Report->SetArrayField(TEXT("runtime_task_speed_evidence"), RuntimeSpeedEvidence);
            Report->SetArrayField(TEXT("move_completion_events"), MoveCompletions);
            Report->SetNumberField(TEXT("move_completion_event_count"), MoveCompletionCount);
            Report->SetBoolField(TEXT("completion_events_truncated"), MoveCompletionCount > MoveCompletions.Num());
            if (bHasSnapshot)
            {
                if (Npc.IsValid() && ReadBool(Npc.Get(), TEXT("IsInteracting"), true))
                {
                    Number(GameMode.Get(), TEXT("CurrentTime"), 600.0);
                    Interact(TEXT("Interact"));
                }
                for (const TWeakObjectPtr<UUserWidget>& Dialogue : OpenedDialogues)
                    if (Dialogue.IsValid()) Dialogue->RemoveFromParent();
                for (int32 Index = SavedProperties.Num() - 1; Index >= 0; --Index)
                {
                    const FSavedProperty& Saved = SavedProperties[Index];
                    if (Saved.Object.IsValid()) Write(Saved.Object.Get(), *Saved.Name, Saved.Value);
                }
                if (Board.IsValid())
                    for (const TPair<FName, bool>& Saved : BoardSnapshot) Board->SetValueAsBool(Saved.Key, Saved.Value);
                if (Npc.IsValid())
                {
                    Npc->SetActorTransform(NpcTransform, false, nullptr, ETeleportType::TeleportPhysics);
                    Npc->SetActorHiddenInGame(bOriginalNpcHidden);
                    Npc->SetActorEnableCollision(bOriginalNpcCollision);
                }
                if (Player.IsValid()) Player->SetActorTransform(PlayerTransform, false, nullptr, ETeleportType::TeleportPhysics);
            }
            Report->SetBoolField(TEXT("assertions_passed"), !Test->HasAnyErrors());
            if (Test->HasAnyErrors() && Status == TEXT("passed")) Report->SetStringField(TEXT("status"), TEXT("failed"));
            FString Path;
            if (!FParse::Value(FCommandLine::Get(), TEXT("NpcNavigationReport="), Path) || Path.IsEmpty())
                Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NpcAudit"), TEXT("navigation-contracts.json"));
            Path = FPaths::ConvertRelativePathToFull(Path);
            FString Json;
            const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
            const bool bSaved = IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true)
                && FJsonSerializer::Serialize(Report.ToSharedRef(), Writer)
                && FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            Test->TestTrue(TEXT("Actual navigation report was written"), bSaved);
            if (bSaved) Test->AddInfo(TEXT("NPC navigation report: ") + Path);
            Phase = EPhase::Done;
            return true;
        }

        struct FSavedProperty
        {
            TWeakObjectPtr<UObject> Object;
            FString Name;
            TSharedPtr<FJsonValue> Value;
        };
        FAutomationTestBase* Test;
        EPhase Phase = EPhase::AwaitWorld;
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<ACharacter> Npc;
        TWeakObjectPtr<ACharacter> Player;
        TWeakObjectPtr<AGameModeBase> GameMode;
        TWeakObjectPtr<AAIController> AI;
        TWeakObjectPtr<UBlackboardComponent> Board;
        TWeakObjectPtr<UBrainComponent> Brain;
        TWeakObjectPtr<UPathFollowingComponent> PathFollowing;
        TWeakObjectPtr<UNavigationSystemV1> Nav;
        FDelegateHandle CompletionHandle;
        TSharedPtr<FJsonObject> Report;
        TSharedPtr<FJsonObject> CurrentStage;
        TArray<TSharedPtr<FJsonValue>> Stages;
        TArray<TSharedPtr<FJsonValue>> Samples;
        TArray<TSharedPtr<FJsonValue>> MoveCompletions;
        TArray<TSharedPtr<FJsonValue>> NavigationDiagnostics;
        TArray<TSharedPtr<FJsonValue>> RuntimeSpeedEvidence;
        TArray<FSavedProperty> SavedProperties;
        TArray<TPair<FName, bool>> BoardSnapshot;
        TArray<TWeakObjectPtr<UUserWidget>> OpenedDialogues;
        FTransform PlayerTransform;
        FTransform NpcTransform;
        FVector AuthoredHome = FVector::ZeroVector;
        FVector HomeGoal = FVector::ZeroVector;
        FVector StartPosition = FVector::ZeroVector;
        FString StageName;
        double StartedAt = -1.0;
        double InitialGameTime = 0.0;
        double PhaseStartedAt = 0.0;
        double StageGameTime = 0.0;
        double LastSampleAt = -1.0;
        double StartDistance = 0.0;
        double MaxDisplacement = 0.0;
        double FixturePathLength = 0.0;
        double ExpectedPatrolSpeed = 0.0;
        double ExpectedChaseSpeed = 0.0;
        int32 FixturePathPointCount = 0;
        int32 MoveCompletionCount = 0;
        bool bSawMovingPath = false;
        bool bSawNonHomePath = false;
        bool bSawPatrolSpeed = false;
        bool bSawChaseSpeed = false;
        bool bSawPlayerPath = false;
        bool bHomeMoveSucceeded = false;
        bool bHasSnapshot = false;
        bool bOriginalNpcHidden = false;
        bool bOriginalNpcCollision = true;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNpcGameplayNavigationContracts,
    "NPC.Gameplay.NavigationContracts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNpcGameplayNavigationContracts::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("Unreal Editor is available for navigation contracts"), GEditor)) return false;
    if (GEditor->PlayWorld)
    {
        AddError(TEXT("NavigationContracts requires no pre-existing PIE session"));
        return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(NpcNavigationAudit::TestMap));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(NpcNavigationAudit::FNavigationContracts(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
