#include "NpcAuditLibrary.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/PanelWidget.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace NpcInteractionAudit
{
    constexpr int32 RandomSeed = 378;
    constexpr int32 RareTrials = 10000;
    const TCHAR* const OriginalMap = TEXT("/Game/ThirdPerson/Maps/ThirdPersonMap");

    // Runtime fixture snapshots only. Copy the actual reflected value rather than
    // reconstructing an inventory or gameplay rule in a parallel implementation.
    class FPropertySnapshot final
    {
    public:
        FPropertySnapshot(UObject* InOwner, FProperty* InProperty)
            : Owner(InOwner), Property(InProperty)
        {
            Value = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
            Property->InitializeValue(Value);
            Property->CopyCompleteValue(Value, Property->ContainerPtrToValuePtr<void>(InOwner));
        }

        ~FPropertySnapshot()
        {
            Restore();
            Property->DestroyValue(Value);
            FMemory::Free(Value);
        }

        void Restore() const
        {
            if (Owner.IsValid())
            {
                Property->CopyCompleteValue(
                    Property->ContainerPtrToValuePtr<void>(Owner.Get()), Value);
            }
        }

        bool MatchesSnapshot() const
        {
            return Owner.IsValid() && Property->Identical(
                Property->ContainerPtrToValuePtr<void>(Owner.Get()), Value, PPF_None);
        }

    private:
        TWeakObjectPtr<UObject> Owner;
        FProperty* Property = nullptr;
        void* Value = nullptr;
    };

    static FProperty* StructField(const UScriptStruct* Struct, const TCHAR* Stem)
    {
        if (!Struct)
        {
            return nullptr;
        }
        const FString Prefix = FString(Stem) + TEXT("_");
        for (TFieldIterator<FProperty> It(Struct); It; ++It)
        {
            if (It->GetName() == Stem || It->GetName().StartsWith(Prefix))
            {
                return *It;
            }
        }
        return nullptr;
    }

    static double Numeric(FProperty* Property, const void* Value)
    {
        const FNumericProperty* Number = CastField<FNumericProperty>(Property);
        if (!Number || !Value)
        {
            return 0.0;
        }
        return Number->IsFloatingPoint()
            ? Number->GetFloatingPointPropertyValue(Value)
            : static_cast<double>(Number->GetSignedIntPropertyValue(Value));
    }

    static void SetNumeric(FProperty* Property, void* Value, double Number)
    {
        if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
        {
            if (NumericProperty->IsFloatingPoint())
            {
                NumericProperty->SetFloatingPointPropertyValue(Value, Number);
            }
            else
            {
                NumericProperty->SetIntPropertyValue(Value, static_cast<int64>(Number));
            }
        }
    }

    static UObject* ObjectProperty(UObject* Object, const TCHAR* Name)
    {
        FObjectPropertyBase* Property = Object
            ? FindFProperty<FObjectPropertyBase>(Object->GetClass(), Name) : nullptr;
        return Property ? Property->GetObjectPropertyValue_InContainer(Object) : nullptr;
    }

    struct FStack
    {
        FName Row;
        int32 Count = 1;
    };

    struct FActorAvailability
    {
        TWeakObjectPtr<AActor> Actor;
        bool bHidden = false;
        bool bCollisionEnabled = false;
    };

    class FInteractionFixture final
    {
    public:
        FInteractionFixture(FAutomationTestBase* InTest, UWorld* InWorld)
            : Test(InTest), World(InWorld), Report(MakeShared<FJsonObject>())
        {
            Report->SetStringField(TEXT("schema"), TEXT("npc-interaction-contracts/v1"));
            Report->SetStringField(TEXT("test"), TEXT("NPC.Gameplay.InteractionContracts"));
            Report->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
            Report->SetStringField(TEXT("map_package"),
                UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()));
            Report->SetStringField(TEXT("scope"),
                TEXT("Actual original-map PIE player and NPCs, original Blueprint ProcessEvent dispatch, "
                     "actual constructed UMG UButton OnClicked delegates, and original DecideItem calls. "
                     "Runtime fixture snapshots are restored. No physical keyboard/mouse or rendered UI claim."));
            Report->SetNumberField(TEXT("rng_seed"), RandomSeed);
            StartedAt = FPlatformTime::Seconds();
        }

        ~FInteractionFixture()
        {
            CloseWidgets();
            if (IsValid(OverlapFixture))
            {
                OverlapFixture->DestroyComponent();
            }
            for (const TPair<TWeakObjectPtr<UPrimitiveComponent>, bool>& Pair : OverlapSettings)
            {
                if (Pair.Key.IsValid())
                {
                    Pair.Key->SetGenerateOverlapEvents(Pair.Value);
                }
            }
            for (const TPair<TWeakObjectPtr<AActor>, FTransform>& Pair : ActorTransforms)
            {
                if (Pair.Key.IsValid())
                {
                    Pair.Key->SetActorTransform(Pair.Value, false, nullptr, ETeleportType::TeleportPhysics);
                }
            }
            RestoreTargetAvailability();
            if (IsValid(ItemActor))
            {
                ItemActor->Destroy();
            }
            for (const TUniquePtr<FPropertySnapshot>& Snapshot : Snapshots)
            {
                Snapshot->Restore();
            }
        }

        void Run()
        {
            if (Setup())
            {
                TargetSelection();
                CloseWidgets();
                Trading();
                CloseWidgets();
                Food();
                CloseWidgets();
                RareItemProbability();
                BehaviorDecoratorProbability();
            }
            bool bRestored = true;
            for (const TUniquePtr<FPropertySnapshot>& Snapshot : Snapshots)
            {
                Snapshot->Restore();
                bRestored &= Snapshot->MatchesSnapshot();
            }
            Check(TEXT("fixture.captured_runtime_properties_restored"), bRestored);
            Report->SetNumberField(TEXT("runtime_property_snapshot_count"), Snapshots.Num());
            Report->SetBoolField(TEXT("captured_runtime_properties_restored"), bRestored);
            Report->SetArrayField(TEXT("checks"), Checks);
            Report->SetNumberField(TEXT("check_count"), Checks.Num());
            Report->SetNumberField(TEXT("passed_check_count"), PassedChecks);
            Report->SetNumberField(TEXT("failed_check_count"), Checks.Num() - PassedChecks);
            Report->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - StartedAt);
            Report->SetBoolField(TEXT("assertions_passed"), !Test->HasAnyErrors());
            WriteReport();
        }

    private:
        bool Check(const FString& Label, bool bPassed,
            const TSharedPtr<FJsonValue>& Expected = nullptr,
            const TSharedPtr<FJsonValue>& Actual = nullptr)
        {
            TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
            Record->SetStringField(TEXT("case"), Label);
            Record->SetBoolField(TEXT("passed"), bPassed);
            Record->SetField(TEXT("expected"), Expected.IsValid()
                ? Expected : MakeShared<FJsonValueBoolean>(true));
            Record->SetField(TEXT("actual"), Actual.IsValid()
                ? Actual : MakeShared<FJsonValueBoolean>(bPassed));
            Checks.Add(MakeShared<FJsonValueObject>(Record));
            PassedChecks += bPassed ? 1 : 0;
            Test->TestTrue(Label, bPassed);
            return bPassed;
        }

        bool Equal(const FString& Label, double Expected, double Actual, double Tolerance = 0.00001)
        {
            return Check(Label, FMath::IsNearlyEqual(Expected, Actual, Tolerance),
                MakeShared<FJsonValueNumber>(Expected), MakeShared<FJsonValueNumber>(Actual));
        }

        bool Setup()
        {
            Controller = World->GetFirstPlayerController();
            Player = Controller ? Controller->GetPawn() : nullptr;
            Table = LoadObject<UDataTable>(nullptr, TEXT("/Game/AI/DT_Item_Data.DT_Item_Data"));
            UClass* NpcClass = LoadClass<AActor>(nullptr,
                TEXT("/Game/ThirdPerson/Blueprints/BP_NPC_Standard.BP_NPC_Standard_C"));
            bool bReady = Check(TEXT("setup.actual_possessed_original_player"), IsValid(Player)
                && Player->GetClass()->GetPathName()
                    == TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter_C"));
            bReady &= Check(TEXT("setup.original_item_table"), IsValid(Table) && Table->GetRowStruct());
            bReady &= Check(TEXT("setup.original_npc_class"), NpcClass != nullptr);
            if (!bReady)
            {
                return false;
            }
            for (TActorIterator<AActor> It(World, NpcClass); It; ++It)
            {
                Npcs.Add(*It);
            }
            bReady &= Equal(TEXT("setup.original_five_npcs"), 5, Npcs.Num());
            for (const TCHAR* Name : {TEXT("Inventory"), TEXT("Money"), TEXT("Current_Stamina"),
                TEXT("Max_Stamina"), TEXT("Base_Speed"), TEXT("Reduce_Stamina"),
                TEXT("BestScore"), TEXT("BestTarget"), TEXT("InventoryWidgetRef")})
            {
                FProperty* Property = FindFProperty<FProperty>(Player->GetClass(), Name);
                bReady &= Check(FString(TEXT("setup.player_property.")) + Name, Property != nullptr);
                if (Property)
                {
                    Snapshots.Add(MakeUnique<FPropertySnapshot>(Player, Property));
                }
            }
            for (AActor* Npc : Npcs)
            {
                FProperty* Trust = FindFProperty<FProperty>(Npc->GetClass(), TEXT("Trust_Level"));
                bReady &= Check(TEXT("setup.npc_trust_property.") + Npc->GetName(), Trust != nullptr);
                if (Trust)
                {
                    Snapshots.Add(MakeUnique<FPropertySnapshot>(Npc, Trust));
                }
            }
            return bReady;
        }

        double ReadNumber(UObject* Object, const TCHAR* Name) const
        {
            FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), Name);
            return Property ? Numeric(Property, Property->ContainerPtrToValuePtr<void>(Object)) : 0.0;
        }

        bool WriteNumber(UObject* Object, const TCHAR* Name, double Value)
        {
            FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), Name);
            if (!Check(FString(TEXT("fixture.numeric_property.")) + Name,
                CastField<FNumericProperty>(Property) != nullptr))
            {
                return false;
            }
            SetNumeric(Property, Property->ContainerPtrToValuePtr<void>(Object), Value);
            return true;
        }

        bool Invoke(UObject* Object, const TCHAR* Function, const FString& Arguments = TEXT("{}"))
        {
            TSharedPtr<FJsonObject> Parsed;
            const FString Json = UNpcAuditLibrary::Invoke(Object, Function, Arguments);
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
            bool bOk = false;
            const bool bParsed = FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid();
            const bool bSuccess = bParsed && Parsed->TryGetBoolField(TEXT("ok"), bOk) && bOk;
            return Check(FString(TEXT("dispatch.original_function.")) + Function, bSuccess,
                MakeShared<FJsonValueString>(TEXT("successful actual ProcessEvent")),
                MakeShared<FJsonValueString>(Json));
        }

        const uint8* Row(FName Name) const
        {
            return Table ? Table->FindRowUnchecked(Name) : nullptr;
        }

        double RowNumber(const uint8* Data, const TCHAR* Field) const
        {
            FProperty* Property = StructField(Table->GetRowStruct(), Field);
            return Property && Data ? Numeric(Property, Property->ContainerPtrToValuePtr<void>(Data)) : 0.0;
        }

        bool SetRows(UObject* Object, const TCHAR* ArrayName, const TArray<FStack>& Stacks)
        {
            FArrayProperty* Array = FindFProperty<FArrayProperty>(Object->GetClass(), ArrayName);
            FStructProperty* Inner = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
            if (!Check(FString(TEXT("fixture.actual_row_array.")) + ArrayName,
                Inner && Inner->Struct == Table->GetRowStruct()))
            {
                return false;
            }
            FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Object));
            Helper.EmptyValues();
            FProperty* CountProperty = StructField(Inner->Struct, TEXT("Count"));
            if (!Check(TEXT("fixture.item_count_field"), CastField<FNumericProperty>(CountProperty) != nullptr))
            {
                return false;
            }
            for (const FStack& Stack : Stacks)
            {
                const uint8* Data = Row(Stack.Row);
                if (!Check(TEXT("fixture.table_row.") + Stack.Row.ToString(), Data != nullptr))
                {
                    return false;
                }
                const int32 Index = Helper.AddValue();
                Inner->Struct->CopyScriptStruct(Helper.GetRawPtr(Index), Data);
                SetNumeric(CountProperty, CountProperty->ContainerPtrToValuePtr<void>(Helper.GetRawPtr(Index)), Stack.Count);
            }
            return true;
        }

        bool SameItem(const void* Data, const uint8* TableRow) const
        {
            FTextProperty* Name = CastField<FTextProperty>(StructField(Table->GetRowStruct(), TEXT("Item_Name")));
            return Name && Data && TableRow && Name->GetPropertyValue_InContainer(Data).EqualTo(
                Name->GetPropertyValue_InContainer(TableRow));
        }

        int32 InventoryCount(FName Name, int32* OutStacks = nullptr) const
        {
            FArrayProperty* Array = FindFProperty<FArrayProperty>(Player->GetClass(), TEXT("Inventory"));
            FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Player));
            FProperty* Count = StructField(Table->GetRowStruct(), TEXT("Count"));
            int32 Total = 0;
            int32 Stacks = 0;
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
            {
                if (SameItem(Helper.GetRawPtr(Index), Row(Name)))
                {
                    Total += static_cast<int32>(Numeric(Count, Count->ContainerPtrToValuePtr<void>(Helper.GetRawPtr(Index))));
                    ++Stacks;
                }
            }
            if (OutStacks)
            {
                *OutStacks = Stacks;
            }
            return Total;
        }

        UUserWidget* NewContainer(const TCHAR* ClassPath, bool bShop, FName Catalog = NAME_None)
        {
            UClass* Class = LoadClass<UUserWidget>(nullptr, ClassPath);
            if (!Check(TEXT("fixture.original_widget_class"), Class != nullptr))
            {
                return nullptr;
            }
            UUserWidget* Widget = CreateWidget<UUserWidget>(Controller, Class);
            if (!Check(TEXT("fixture.actual_owned_widget"), IsValid(Widget)))
            {
                return nullptr;
            }
            Widgets.Add(TStrongObjectPtr<UUserWidget>(Widget));
            if (bShop)
            {
                FObjectPropertyBase* Target = FindFProperty<FObjectPropertyBase>(Class, TEXT("TargetNPC"));
                if (!Check(TEXT("fixture.shop_target_npc"), Target && Npcs.Num() > 0))
                {
                    return nullptr;
                }
                Target->SetObjectPropertyValue_InContainer(Widget, Npcs[0]);
                if (!SetRows(Widget, TEXT("NPC_SalesList"), {{Catalog, 1}}))
                {
                    return nullptr;
                }
            }
            else if (FObjectPropertyBase* Ref = FindFProperty<FObjectPropertyBase>(Player->GetClass(), TEXT("InventoryWidgetRef")))
            {
                Ref->SetObjectPropertyValue_InContainer(Player, Widget);
            }
            Widget->AddToViewport();
            Check(TEXT("fixture.container_in_viewport"), Widget->IsInViewport());
            if (!Invoke(Widget, bShop ? TEXT("UpdateShopUI") : TEXT("UpdateInventoryUI")))
            {
                return nullptr;
            }
            return Widget;
        }

        UUserWidget* FindLine(UUserWidget* Container, const TCHAR* PanelName, FName ItemName)
        {
            UPanelWidget* Panel = Cast<UPanelWidget>(ObjectProperty(Container, PanelName));
            if (!Check(FString(TEXT("fixture.original_panel.")) + PanelName, Panel != nullptr))
            {
                return nullptr;
            }
            for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
            {
                UUserWidget* Line = Cast<UUserWidget>(Panel->GetChildAt(Index));
                FStructProperty* Data = Line
                    ? FindFProperty<FStructProperty>(Line->GetClass(), TEXT("TargetItemData")) : nullptr;
                if (Data && SameItem(Data->ContainerPtrToValuePtr<void>(Line), Row(ItemName)))
                {
                    return Line;
                }
            }
            Check(TEXT("fixture.original_generated_item_line.") + ItemName.ToString(), false);
            return nullptr;
        }

        bool Click(UUserWidget* Container, const TCHAR* PanelName, FName RowName, const TCHAR* ButtonName)
        {
            UUserWidget* Line = FindLine(Container, PanelName, RowName);
            UButton* Button = Cast<UButton>(ObjectProperty(Line, ButtonName));
            if (!Check(FString(TEXT("input.actual_bound_button.")) + ButtonName,
                IsValid(Button) && Button->OnClicked.IsBound()))
            {
                return false;
            }
            if (!Check(FString(TEXT("input.button_available.")) + ButtonName,
                Button->GetIsEnabled() && Button->GetVisibility() != ESlateVisibility::Collapsed
                    && Button->GetVisibility() != ESlateVisibility::Hidden))
            {
                return false;
            }
            // The callback can remove its own line during the original UI refresh.
            TStrongObjectPtr<UUserWidget> KeepAlive(Line);
            Button->OnClicked.Broadcast();
            return true;
        }

        void CloseWidgets()
        {
            for (const TStrongObjectPtr<UUserWidget>& Widget : Widgets)
            {
                if (Widget.IsValid())
                {
                    Widget->RemoveFromParent();
                }
            }
            Widgets.Empty();
        }

        void TargetSelection()
        {
            ActorTransforms.Add({Player, Player->GetActorTransform()});
            for (AActor* Npc : Npcs)
            {
                ActorTransforms.Add({Npc, Npc->GetActorTransform()});
                ActorAvailability.Add({Npc, Npc->IsHidden(), Npc->GetActorEnableCollision()});
                TInlineComponentArray<UPrimitiveComponent*> Components(Npc);
                for (UPrimitiveComponent* Component : Components)
                {
                    OverlapSettings.Add({Component, Component->GetGenerateOverlapEvents()});
                    Component->SetGenerateOverlapEvents(true);
                }
            }
            const FVector Origin = Player->GetActorLocation();
            Player->SetActorRotation(FRotator::ZeroRotator, ETeleportType::TeleportPhysics);
            const FVector Offsets[] = {FVector(200, 0, 0), FVector(140, 140, 0), FVector(-200, 0, 0)};
            for (int32 Index = 0; Index < Npcs.Num(); ++Index)
            {
                const FVector Offset = Index < 3 ? Offsets[Index] : FVector(10000 + Index * 1000, 0, 0);
                Npcs[Index]->SetActorLocation(Origin + Offset, false, nullptr, ETeleportType::TeleportPhysics);
            }
            // Valid navigation lets off-duty NPCs arrive home before this test
            // begins. Their original home callback correctly hides them and
            // disables actor collision. Establish availability for the spatial
            // fixture explicitly; component overlap flags alone cannot override
            // actor-level collision-off. This does not change schedule rules.
            TArray<TSharedPtr<FJsonValue>> Availability;
            for (int32 Index = 0; Index < Npcs.Num(); ++Index)
            {
                const FActorAvailability& Before = ActorAvailability[Index];
                if (Index < 3)
                {
                    Npcs[Index]->SetActorHiddenInGame(false);
                    Npcs[Index]->SetActorEnableCollision(true);
                }
                TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
                State->SetNumberField(TEXT("npc_position_index"), Index);
                State->SetStringField(TEXT("actor_name"), Npcs[Index]->GetName());
                State->SetBoolField(TEXT("selected_for_direction_fixture"), Index < 3);
                State->SetBoolField(TEXT("hidden_before"), Before.bHidden);
                State->SetBoolField(TEXT("collision_enabled_before"), Before.bCollisionEnabled);
                State->SetBoolField(TEXT("hidden_after_preparation"), Npcs[Index]->IsHidden());
                State->SetBoolField(TEXT("collision_enabled_after_preparation"), Npcs[Index]->GetActorEnableCollision());
                Availability.Add(MakeShared<FJsonValueObject>(State));
            }
            TSharedRef<FJsonObject> GeometryFixture = MakeShared<FJsonObject>();
            GeometryFixture->SetStringField(TEXT("scope"),
                TEXT("Three original NPCs are made visible and actor-collision-enabled only for the fixed direction fixture. "
                     "The original hidden/collision flags and transforms are captured and restored. "
                     "This isolates spatial selection; scheduled NPC availability is not its contract."));
            GeometryFixture->SetArrayField(TEXT("actor_availability"), Availability);
            Report->SetObjectField(TEXT("target_geometry_fixture"), GeometryFixture);
            // A transient real overlap component controls the test geometry without
            // replacing the original GetOverlappingActors or selection Blueprint.
            OverlapFixture = NewObject<USphereComponent>(Player, NAME_None, RF_Transient);
            OverlapFixture->SetupAttachment(Player->GetRootComponent());
            OverlapFixture->SetSphereRadius(600.0f);
            OverlapFixture->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
            OverlapFixture->SetCollisionResponseToAllChannels(ECR_Overlap);
            OverlapFixture->SetGenerateOverlapEvents(true);
            Player->AddInstanceComponent(OverlapFixture);
            OverlapFixture->RegisterComponent();
            OverlapFixture->UpdateOverlaps();
            UClass* NpcClass = Npcs[0]->GetClass();
            TArray<AActor*> Overlapping;
            Player->GetOverlappingActors(Overlapping, NpcClass);
            if (Check(TEXT("target.geometry_contains_three_original_npcs"),
                Overlapping.Contains(Npcs[0]) && Overlapping.Contains(Npcs[1]) && Overlapping.Contains(Npcs[2])))
            {
                if (Invoke(Player, TEXT("SendInteractionSignal"), TEXT("{\"KeyName\":\"__NpcAuditSelectionOnly\"}")))
                {
                    Check(TEXT("target.actor_forward_selects_front_npc"),
                        ObjectProperty(Player, TEXT("BestTarget")) == Npcs[0],
                        MakeShared<FJsonValueString>(Npcs[0]->GetName()),
                        MakeShared<FJsonValueString>(GetNameSafe(ObjectProperty(Player, TEXT("BestTarget")))));
                    Equal(TEXT("target.front_dot_product"), 1.0, ReadNumber(Player, TEXT("BestScore")), 0.001);
                }
                Player->SetActorRotation(FRotator(0, 180, 0), ETeleportType::TeleportPhysics);
                if (Invoke(Player, TEXT("SendInteractionSignal"), TEXT("{\"KeyName\":\"__NpcAuditSelectionOnly\"}")))
                {
                    Check(TEXT("target.rotated_actor_selects_other_npc"),
                        ObjectProperty(Player, TEXT("BestTarget")) == Npcs[2],
                        MakeShared<FJsonValueString>(Npcs[2]->GetName()),
                        MakeShared<FJsonValueString>(GetNameSafe(ObjectProperty(Player, TEXT("BestTarget")))));
                }
            }
            TargetSelectionMatrix(NpcClass);
            OverlapFixture->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            for (int32 Index = 0; Index < Npcs.Num(); ++Index)
            {
                Npcs[Index]->SetActorLocation(Origin + FVector(20000 + Index * 1000, 0, 0),
                    false, nullptr, ETeleportType::TeleportPhysics);
                TInlineComponentArray<UPrimitiveComponent*> Components(Npcs[Index]);
                for (UPrimitiveComponent* Component : Components)
                {
                    Component->UpdateOverlaps();
                }
            }
            Overlapping.Reset();
            Player->GetOverlappingActors(Overlapping, NpcClass);
            Equal(TEXT("target.no_npc_overlap_precondition"), 0, Overlapping.Num());
            if (Invoke(Player, TEXT("SendInteractionSignal"), TEXT("{\"KeyName\":\"__NpcAuditSelectionOnly\"}")))
            {
                Check(TEXT("target.no_overlap_clears_previous_target"),
                    ObjectProperty(Player, TEXT("BestTarget")) == nullptr);
                Equal(TEXT("target.no_overlap_resets_score"), -1, ReadNumber(Player, TEXT("BestScore")));
            }
            for (const TPair<TWeakObjectPtr<AActor>, FTransform>& Pair : ActorTransforms)
            {
                if (Pair.Key.IsValid())
                {
                    Pair.Key->SetActorTransform(Pair.Value, false, nullptr, ETeleportType::TeleportPhysics);
                }
            }
            const bool bAvailabilityRestored = RestoreTargetAvailability();
            Check(TEXT("fixture.target_actor_hidden_and_collision_flags_restored"), bAvailabilityRestored);
            GeometryFixture->SetBoolField(TEXT("actor_availability_restored"), bAvailabilityRestored);
            TArray<TSharedPtr<FJsonValue>> RestoredStates;
            for (const FActorAvailability& Original : ActorAvailability)
            {
                TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
                State->SetStringField(TEXT("actor_name"), GetNameSafe(Original.Actor.Get()));
                State->SetBoolField(TEXT("actor_valid"), Original.Actor.IsValid());
                State->SetBoolField(TEXT("expected_hidden"), Original.bHidden);
                State->SetBoolField(TEXT("expected_collision_enabled"), Original.bCollisionEnabled);
                if (Original.Actor.IsValid())
                {
                    State->SetBoolField(TEXT("restored_hidden"), Original.Actor->IsHidden());
                    State->SetBoolField(TEXT("restored_collision_enabled"), Original.Actor->GetActorEnableCollision());
                }
                RestoredStates.Add(MakeShared<FJsonValueObject>(State));
            }
            GeometryFixture->SetArrayField(TEXT("restored_actor_availability"), RestoredStates);
        }

        bool RestoreTargetAvailability()
        {
            bool bRestored = true;
            for (const FActorAvailability& Original : ActorAvailability)
            {
                if (!Original.Actor.IsValid())
                {
                    bRestored = false;
                    continue;
                }
                Original.Actor->SetActorHiddenInGame(Original.bHidden);
                Original.Actor->SetActorEnableCollision(Original.bCollisionEnabled);
                bRestored &= Original.Actor->IsHidden() == Original.bHidden
                    && Original.Actor->GetActorEnableCollision() == Original.bCollisionEnabled;
            }
            return bRestored;
        }

        void TargetSelectionMatrix(UClass* NpcClass)
        {
            // Independent, predefined answers for the fixed actor offsets
            // (200,0), (140,140), (-200,0). Do not calculate the oracle by
            // reimplementing the Blueprint dot-product/maximum loop.
            // Avoid exact Voronoi boundaries at 22.5, 112.5 and -90 degrees.
            struct FDirectionCase
            {
                double Yaw;
                int32 ExpectedNpcIndex;
            };
            const FDirectionCase Directions[] = {
                {-179, 2}, {-165, 2}, {-150, 2}, {-135, 2}, {-120, 2}, {-105, 2}, {-95, 2},
                {-85, 0}, {-75, 0}, {-60, 0}, {-45, 0}, {-30, 0}, {-15, 0}, {0, 0}, {15, 0}, {20, 0},
                {25, 1}, {30, 1}, {45, 1}, {60, 1}, {75, 1}, {90, 1}, {105, 1}, {110, 1},
                {115, 2}, {120, 2}, {135, 2}, {150, 2}, {165, 2}, {179, 2}
            };
            TSharedRef<FJsonObject> Matrix = MakeShared<FJsonObject>();
            Matrix->SetStringField(TEXT("function"), TEXT("BP_ThirdPersonCharacter.SendInteractionSignal"));
            Matrix->SetStringField(TEXT("scope"),
                TEXT("Fixed three-original-NPC geometry, 30 predefined unique nearest-direction cases; "
                     "rotate the actual player Actor only. Does not assert camera targeting or physical key input."));
            Matrix->SetStringField(TEXT("interaction_key"), TEXT("__NpcAuditSelectionOnly"));
            Matrix->SetStringField(TEXT("oracle"),
                TEXT("Predeclared NPC position indices; no runtime recreation of the production selection algorithm. "
                     "Exact angular ties excluded; no new tie or front-only contract."));
            TArray<TSharedPtr<FJsonValue>> Geometry;
            for (int32 Index = 0; Index < 3; ++Index)
            {
                const FVector Relative = Npcs[Index]->GetActorLocation() - Player->GetActorLocation();
                TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
                Candidate->SetNumberField(TEXT("npc_position_index"), Index);
                Candidate->SetStringField(TEXT("actor_name"), Npcs[Index]->GetName());
                Candidate->SetNumberField(TEXT("relative_x"), Relative.X);
                Candidate->SetNumberField(TEXT("relative_y"), Relative.Y);
                Candidate->SetNumberField(TEXT("relative_z"), Relative.Z);
                Geometry.Add(MakeShared<FJsonValueObject>(Candidate));
            }
            Matrix->SetArrayField(TEXT("fixed_geometry"), Geometry);
            TArray<TSharedPtr<FJsonValue>> Cases;
            int32 Successes = 0;
            int32 Dispatched = 0;
            int32 ValidOverlapCases = 0;
            for (const FDirectionCase& Direction : Directions)
            {
                Player->SetActorRotation(FRotator(0, Direction.Yaw, 0), ETeleportType::TeleportPhysics);
                OverlapFixture->UpdateOverlaps();
                TArray<AActor*> Overlapping;
                Player->GetOverlappingActors(Overlapping, NpcClass);
                const bool bGeometryValid = Overlapping.Num() == 3
                    && Overlapping.Contains(Npcs[0]) && Overlapping.Contains(Npcs[1])
                    && Overlapping.Contains(Npcs[2]);
                const FString Label = FString::Printf(TEXT("target.matrix.yaw_%g"), Direction.Yaw);
                Check(Label + TEXT(".three_live_overlaps"), bGeometryValid,
                    MakeShared<FJsonValueNumber>(3), MakeShared<FJsonValueNumber>(Overlapping.Num()));
                ValidOverlapCases += bGeometryValid ? 1 : 0;
                const bool bDispatched = bGeometryValid && Invoke(Player, TEXT("SendInteractionSignal"),
                    TEXT("{\"KeyName\":\"__NpcAuditSelectionOnly\"}"));
                Dispatched += bDispatched ? 1 : 0;
                UObject* ActualTarget = bDispatched ? ObjectProperty(Player, TEXT("BestTarget")) : nullptr;
                const int32 ActualIndex = Npcs.IndexOfByKey(Cast<AActor>(ActualTarget));
                const bool bSuccess = bDispatched && ActualTarget == Npcs[Direction.ExpectedNpcIndex];
                Check(Label + TEXT(".expected_target"), bSuccess,
                    MakeShared<FJsonValueNumber>(Direction.ExpectedNpcIndex),
                    MakeShared<FJsonValueNumber>(ActualIndex));
                Successes += bSuccess ? 1 : 0;
                TSharedRef<FJsonObject> Case = MakeShared<FJsonObject>();
                Case->SetNumberField(TEXT("requested_actor_yaw_degrees"), Direction.Yaw);
                Case->SetNumberField(TEXT("actual_actor_yaw_degrees"), Player->GetActorRotation().Yaw);
                Case->SetNumberField(TEXT("expected_npc_position_index"), Direction.ExpectedNpcIndex);
                Case->SetNumberField(TEXT("actual_npc_position_index"), ActualIndex);
                Case->SetStringField(TEXT("actual_target_name"), GetNameSafe(ActualTarget));
                Case->SetNumberField(TEXT("overlapping_npc_count"), Overlapping.Num());
                Case->SetBoolField(TEXT("all_three_expected_npcs_overlap"), bGeometryValid);
                Case->SetBoolField(TEXT("original_function_dispatched"), bDispatched);
                if (bDispatched)
                {
                    Case->SetNumberField(TEXT("observed_best_score"), ReadNumber(Player, TEXT("BestScore")));
                }
                Case->SetBoolField(TEXT("passed"), bSuccess);
                Cases.Add(MakeShared<FJsonValueObject>(Case));
            }
            Matrix->SetNumberField(TEXT("trials"), Cases.Num());
            Matrix->SetNumberField(TEXT("valid_overlap_trials"), ValidOverlapCases);
            Matrix->SetNumberField(TEXT("dispatched_trials"), Dispatched);
            Matrix->SetNumberField(TEXT("success_count"), Successes);
            Matrix->SetNumberField(TEXT("failure_count"), Cases.Num() - Successes);
            Matrix->SetNumberField(TEXT("success_rate"),
                Cases.IsEmpty() ? 0.0 : static_cast<double>(Successes) / Cases.Num());
            Matrix->SetArrayField(TEXT("cases"), Cases);
            Report->SetObjectField(TEXT("target_selection_matrix"), Matrix);
            Equal(TEXT("target.matrix.all_30_predefined_cases"), 30, Successes);
        }

        void Trading()
        {
            // The original UI intentionally hides the buy button for Gathering
            // rows. Use a real Normal_Food catalog item for purchasable scenarios.
            const FName Item(TEXT("Bread"));
            const uint8* ItemRow = Row(Item);
            if (!Check(TEXT("trade.original_catalog_row"), ItemRow != nullptr))
            {
                return;
            }
            const double Price = RowNumber(ItemRow, TEXT("Item_Price"));
            WriteNumber(Npcs[0], TEXT("Trust_Level"), 10);
            WriteNumber(Player, TEXT("Money"), Price);
            SetRows(Player, TEXT("Inventory"), {});
            UUserWidget* Shop = NewContainer(TEXT("/Game/AI/WBP_ShopMain.WBP_ShopMain_C"), true, Item);
            if (Shop && Click(Shop, TEXT("NPC_ShopList"), Item, TEXT("Button2")))
            {
                Equal(TEXT("trade.exact_funds_money_debited"), 0, ReadNumber(Player, TEXT("Money")));
                Equal(TEXT("trade.exact_funds_one_item_added"), 1, InventoryCount(Item));
            }
            CloseWidgets();

            WriteNumber(Player, TEXT("Money"), 100);
            SetRows(Player, TEXT("Inventory"), {{Item, 2}});
            Shop = NewContainer(TEXT("/Game/AI/WBP_ShopMain.WBP_ShopMain_C"), true, Item);
            if (Shop && Click(Shop, TEXT("NPC_ShopList"), Item, TEXT("Button2")))
            {
                int32 StackCount = 0;
                Equal(TEXT("trade.existing_stack_quantity_incremented"), 3, InventoryCount(Item, &StackCount));
                Equal(TEXT("trade.existing_stack_not_duplicated"), 1, StackCount);
                Equal(TEXT("trade.existing_stack_price_debited"), 100 - Price, ReadNumber(Player, TEXT("Money")));
            }
            CloseWidgets();

            WriteNumber(Player, TEXT("Money"), 0);
            SetRows(Player, TEXT("Inventory"), {});
            Shop = NewContainer(TEXT("/Game/AI/WBP_ShopMain.WBP_ShopMain_C"), true, Item);
            if (Shop && Click(Shop, TEXT("NPC_ShopList"), Item, TEXT("Button2")))
            {
                Equal(TEXT("trade.insufficient_funds_money_unchanged"), 0, ReadNumber(Player, TEXT("Money")));
                Equal(TEXT("trade.insufficient_funds_inventory_unchanged"), 0, InventoryCount(Item));
            }
            CloseWidgets();

            const FName Rare(TEXT("Rare_Watermelon"));
            const uint8* RareRow = Row(Rare);
            if (!Check(TEXT("trade.original_rare_sale_row"), RareRow != nullptr))
            {
                return;
            }
            constexpr int32 SaleCount = 3;
            WriteNumber(Player, TEXT("Money"), 10);
            WriteNumber(Npcs[0], TEXT("Trust_Level"), 10);
            SetRows(Player, TEXT("Inventory"), {{Rare, SaleCount}});
            Shop = NewContainer(TEXT("/Game/AI/WBP_ShopMain.WBP_ShopMain_C"), true, Item);
            if (Shop && Click(Shop, TEXT("Player_InventoryList"), Rare, TEXT("Button1")))
            {
                Equal(TEXT("trade.sell_entire_stack_removed"), 0, InventoryCount(Rare));
                Equal(TEXT("trade.sell_stack_revenue"),
                    10 + RowNumber(RareRow, TEXT("Item_Price")) * SaleCount, ReadNumber(Player, TEXT("Money")));
                Equal(TEXT("trade.sell_stack_npc_trust"),
                    10 + RowNumber(RareRow, TEXT("Trust_Point")) * SaleCount, ReadNumber(Npcs[0], TEXT("Trust_Level")));
            }
            CloseWidgets();
        }

        void Food()
        {
            const FName Bread(TEXT("Bread"));
            const uint8* BreadRow = Row(Bread);
            if (!Check(TEXT("food.original_bread_row"), BreadRow != nullptr))
            {
                return;
            }
            WriteNumber(Player, TEXT("Max_Stamina"), 100);
            WriteNumber(Player, TEXT("Current_Stamina"), 10);
            SetRows(Player, TEXT("Inventory"), {{Bread, 2}});
            UUserWidget* Inventory = NewContainer(TEXT("/Game/AI/WBP_InventoryMain.WBP_InventoryMain_C"), false);
            if (Inventory && Click(Inventory, TEXT("InventoryList"), Bread, TEXT("Button0")))
            {
                Equal(TEXT("food.table_stamina_restored"),
                    10 + RowNumber(BreadRow, TEXT("Restore_Amount")), ReadNumber(Player, TEXT("Current_Stamina")));
                Equal(TEXT("food.stack_consumes_one"), 1, InventoryCount(Bread));
            }
            CloseWidgets();

            WriteNumber(Player, TEXT("Current_Stamina"), 99);
            SetRows(Player, TEXT("Inventory"), {{Bread, 1}});
            Inventory = NewContainer(TEXT("/Game/AI/WBP_InventoryMain.WBP_InventoryMain_C"), false);
            if (Inventory && Click(Inventory, TEXT("InventoryList"), Bread, TEXT("Button0")))
            {
                Equal(TEXT("food.stamina_clamped_at_maximum"), 100, ReadNumber(Player, TEXT("Current_Stamina")));
                Equal(TEXT("food.final_unit_removed"), 0, InventoryCount(Bread));
            }
            CloseWidgets();

            struct FSpecialFood
            {
                const TCHAR* RowName;
                const TCHAR* Property;
                double Before;
                double After;
            };
            const FSpecialFood SpecialFoods[] = {
                {TEXT("Cyber_Core"), TEXT("Base_Speed"), 300, 320},
                {TEXT("Titan_Blood"), TEXT("Max_Stamina"), 100, 110},
                {TEXT("Eternal_Drink"), TEXT("Reduce_Stamina"), 10, 9}
            };
            for (const FSpecialFood& Food : SpecialFoods)
            {
                const FName Name(Food.RowName);
                WriteNumber(Player, Food.Property, Food.Before);
                SetRows(Player, TEXT("Inventory"), {{Name, 1}});
                Inventory = NewContainer(TEXT("/Game/AI/WBP_InventoryMain.WBP_InventoryMain_C"), false);
                if (Inventory && Click(Inventory, TEXT("InventoryList"), Name, TEXT("Button0")))
                {
                    Equal(FString(TEXT("food.original_special_effect.")) + Food.RowName,
                        Food.After, ReadNumber(Player, Food.Property));
                    Equal(FString(TEXT("food.special_consumed.")) + Food.RowName, 0, InventoryCount(Name));
                }
                CloseWidgets();
            }
        }

        void RareItemProbability()
        {
            UClass* ItemClass = LoadClass<AActor>(nullptr, TEXT("/Game/AI/BP_Item_Master.BP_Item_Master_C"));
            if (!Check(TEXT("rare.original_item_class"), ItemClass != nullptr))
            {
                return;
            }
            const FTransform Transform(FRotator::ZeroRotator,
                Player->GetActorLocation() + FVector(0, 0, 10000));
            ItemActor = World->SpawnActorDeferred<AActor>(ItemClass, Transform, nullptr, nullptr,
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
            if (!Check(TEXT("rare.actual_deferred_item_actor"), IsValid(ItemActor)))
            {
                return;
            }
            FNameProperty* ItemId = FindFProperty<FNameProperty>(ItemClass, TEXT("Item_ID"));
            FNameProperty* FinalId = FindFProperty<FNameProperty>(ItemClass, TEXT("Final_ID"));
            FStructProperty* ItemData = FindFProperty<FStructProperty>(ItemClass, TEXT("MyItemData"));
            UFunction* Decide = ItemActor->FindFunction(TEXT("DecideItem"));
            if (!Check(TEXT("rare.original_reflected_function_and_fields"),
                ItemId && FinalId && ItemData && Decide && Decide->ParmsSize == 0
                    && ItemData->Struct == Table->GetRowStruct()))
            {
                return;
            }
            const FName NormalName(TEXT("Watermelon"));
            const FName RareName(TEXT("Rare_Watermelon"));
            ItemId->SetPropertyValue_InContainer(ItemActor, NormalName);
            UGameplayStatics::FinishSpawningActor(ItemActor, Transform);
            const uint8* NormalRow = Row(NormalName);
            const uint8* RareRow = Row(RareName);
            if (!Check(TEXT("rare.both_original_table_rows"), NormalRow && RareRow))
            {
                return;
            }
            int32 RareCount = 0;
            int32 UnexpectedIdCount = 0;
            int32 DataMismatchCount = 0;
            const double SamplingStartedAt = FPlatformTime::Seconds();
            FMath::RandInit(RandomSeed);
            for (int32 Trial = 0; Trial < RareTrials; ++Trial)
            {
                // This is the original compiled Blueprint event, including the
                // original random comparison, table lookup, mesh and material.
                ItemActor->ProcessEvent(Decide, nullptr);
                const FName Result = FinalId->GetPropertyValue_InContainer(ItemActor);
                const bool bRare = Result == RareName;
                RareCount += bRare ? 1 : 0;
                const bool bKnown = bRare || Result == NormalName;
                UnexpectedIdCount += bKnown ? 0 : 1;
                const void* ActualData = ItemData->ContainerPtrToValuePtr<void>(ItemActor);
                const uint8* ExpectedData = bRare ? RareRow : NormalRow;
                DataMismatchCount += bKnown && ItemData->Identical(ActualData, ExpectedData, PPF_None) ? 0 : 1;
            }
            const double Rate = static_cast<double>(RareCount) / RareTrials;
            constexpr double Z = 1.959963984540054;
            const double Denominator = 1.0 + Z * Z / RareTrials;
            const double Centre = (Rate + Z * Z / (2.0 * RareTrials)) / Denominator;
            const double Radius = Z * FMath::Sqrt(
                Rate * (1.0 - Rate) / RareTrials + Z * Z / (4.0 * RareTrials * RareTrials)) / Denominator;
            TSharedRef<FJsonObject> Sampling = MakeShared<FJsonObject>();
            Sampling->SetStringField(TEXT("function"), TEXT("BP_Item_Master.DecideItem"));
            Sampling->SetStringField(TEXT("rng"), TEXT("Engine FMath::RandInit seed; original RandomFloatInRange graph"));
            Sampling->SetNumberField(TEXT("seed"), RandomSeed);
            Sampling->SetNumberField(TEXT("trials"), RareTrials);
            Sampling->SetNumberField(TEXT("rare_count"), RareCount);
            Sampling->SetNumberField(TEXT("rare_rate"), Rate);
            Sampling->SetNumberField(TEXT("configured_threshold"), 0.1);
            Sampling->SetStringField(TEXT("comparison"), TEXT("random_draw <= 0.1"));
            Sampling->SetNumberField(TEXT("wilson_95_lower"), Centre - Radius);
            Sampling->SetNumberField(TEXT("wilson_95_upper"), Centre + Radius);
            Sampling->SetNumberField(TEXT("unexpected_id_count"), UnexpectedIdCount);
            Sampling->SetNumberField(TEXT("table_row_mismatch_count"), DataMismatchCount);
            Sampling->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - SamplingStartedAt);
            Sampling->SetStringField(TEXT("acceptance_rule"),
                TEXT("Predefined broad sanity interval 0.07 <= observed rate <= 0.13, not exact population probability proof."));
            Report->SetObjectField(TEXT("rare_item_sampling"), Sampling);
            Equal(TEXT("rare.only_original_normal_or_rare_ids"), 0, UnexpectedIdCount);
            Equal(TEXT("rare.every_result_matches_original_table_row"), 0, DataMismatchCount);
            Check(TEXT("rare.observed_frequency_sanity_bound"), Rate >= 0.07 && Rate <= 0.13,
                MakeShared<FJsonValueString>(TEXT("0.07 <= observed rate <= 0.13")),
                MakeShared<FJsonValueNumber>(Rate));
            UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(ObjectProperty(ItemActor, TEXT("StaticMesh")));
            const uint8* LastRow = FinalId->GetPropertyValue_InContainer(ItemActor) == RareName ? RareRow : NormalRow;
            FObjectPropertyBase* MeshField = CastField<FObjectPropertyBase>(StructField(Table->GetRowStruct(), TEXT("Item_Mesh")));
            FObjectPropertyBase* MaterialField = CastField<FObjectPropertyBase>(StructField(Table->GetRowStruct(), TEXT("Material")));
            Check(TEXT("rare.actual_mesh_matches_selected_row"), Mesh && MeshField
                && Mesh->GetStaticMesh() == MeshField->GetObjectPropertyValue_InContainer(LastRow));
            Check(TEXT("rare.actual_material_matches_selected_row"), Mesh && MaterialField
                && Mesh->GetMaterial(0) == MaterialField->GetObjectPropertyValue_InContainer(LastRow));
        }

        void BehaviorDecoratorProbability()
        {
            UClass* DecoratorClass = LoadClass<UObject>(nullptr,
                TEXT("/Game/AI/BTD_RandomChance.BTD_RandomChance_C"));
            if (!Check(TEXT("decorator.original_blueprint_class"), DecoratorClass != nullptr))
            {
                return;
            }
            TStrongObjectPtr<UObject> Decorator(NewObject<UObject>(World, DecoratorClass));
            UFunction* Function = Decorator->FindFunction(TEXT("PerformConditionCheck"));
            FObjectPropertyBase* Owner = Function
                ? FindFProperty<FObjectPropertyBase>(Function, TEXT("OwnerActor")) : nullptr;
            FBoolProperty* Return = Function
                ? FindFProperty<FBoolProperty>(Function, TEXT("ReturnValue")) : nullptr;
            FNumericProperty* Chance = FindFProperty<FNumericProperty>(DecoratorClass, TEXT("Chance"));
            if (!Check(TEXT("decorator.actual_function_parameters"), Function && Owner && Return && Chance))
            {
                return;
            }
            const double OriginalDefault = ReadNumber(Decorator.Get(), TEXT("Chance"));
            FStructOnScope Parameters(Function);
            Owner->SetObjectPropertyValue_InContainer(Parameters.GetStructMemory(), Player);
            SetNumeric(Chance, Chance->ContainerPtrToValuePtr<void>(Decorator.Get()), 0.0);
            int32 ZeroChanceAccepted = 0;
            for (int32 Trial = 0; Trial < 100; ++Trial)
            {
                Decorator->ProcessEvent(Function, Parameters.GetStructMemory());
                ZeroChanceAccepted += Return->GetPropertyValue_InContainer(Parameters.GetStructMemory()) ? 1 : 0;
            }
            Equal(TEXT("decorator.zero_chance_rejects_all_100_calls"), 0, ZeroChanceAccepted);

            constexpr int32 Trials = 2000;
            constexpr double ConfiguredChance = 0.1;
            SetNumeric(Chance, Chance->ContainerPtrToValuePtr<void>(Decorator.Get()), ConfiguredChance);
            FMath::RandInit(RandomSeed);
            int32 Accepted = 0;
            const double Started = FPlatformTime::Seconds();
            for (int32 Trial = 0; Trial < Trials; ++Trial)
            {
                // Invoke the original decorator event, not a recreated RNG rule.
                Decorator->ProcessEvent(Function, Parameters.GetStructMemory());
                Accepted += Return->GetPropertyValue_InContainer(Parameters.GetStructMemory()) ? 1 : 0;
            }
            const double Rate = static_cast<double>(Accepted) / Trials;
            TSharedRef<FJsonObject> Sampling = MakeShared<FJsonObject>();
            Sampling->SetStringField(TEXT("function"), TEXT("BTD_RandomChance.PerformConditionCheck"));
            Sampling->SetStringField(TEXT("scope"),
                TEXT("Actual original decorator event on a runtime instance with the actual PIE player as OwnerActor. "
                     "Does not claim a full behavior-tree traversal or quest award."));
            Sampling->SetNumberField(TEXT("original_default_chance"), OriginalDefault);
            Sampling->SetNumberField(TEXT("configured_test_chance"), ConfiguredChance);
            Sampling->SetNumberField(TEXT("seed"), RandomSeed);
            Sampling->SetNumberField(TEXT("trials"), Trials);
            Sampling->SetNumberField(TEXT("accepted_count"), Accepted);
            Sampling->SetNumberField(TEXT("observed_rate"), Rate);
            Sampling->SetStringField(TEXT("comparison"), TEXT("random_draw < Chance"));
            Sampling->SetNumberField(TEXT("zero_chance_trials"), 100);
            Sampling->SetNumberField(TEXT("zero_chance_accepted_count"), ZeroChanceAccepted);
            Sampling->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - Started);
            Sampling->SetStringField(TEXT("acceptance_rule"),
                TEXT("Predefined broad sanity interval 0.07 <= observed rate <= 0.13; no full-quest execution claim."));
            Report->SetObjectField(TEXT("behavior_decorator_sampling"), Sampling);
            Check(TEXT("decorator.observed_frequency_sanity_bound"), Rate >= 0.07 && Rate <= 0.13,
                MakeShared<FJsonValueString>(TEXT("0.07 <= observed rate <= 0.13")),
                MakeShared<FJsonValueNumber>(Rate));
        }

        void WriteReport()
        {
            FString Path;
            if (!FParse::Value(FCommandLine::Get(), TEXT("NpcInteractionReport="), Path) || Path.IsEmpty())
            {
                Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NpcAudit/interaction-contracts.json"));
            }
            Path = FPaths::ConvertRelativePathToFull(Path);
            FString Json;
            const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
            const bool bSerialized = FJsonSerializer::Serialize(Report, Writer);
            const bool bDirectory = IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
            const bool bWritten = bSerialized && bDirectory && FFileHelper::SaveStringToFile(
                Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            Test->TestTrue(TEXT("Actual interaction JSON report was written"), bWritten);
        }

        FAutomationTestBase* Test;
        UWorld* World;
        APlayerController* Controller = nullptr;
        APawn* Player = nullptr;
        UDataTable* Table = nullptr;
        TArray<AActor*> Npcs;
        TArray<TUniquePtr<FPropertySnapshot>> Snapshots;
        TArray<TStrongObjectPtr<UUserWidget>> Widgets;
        TArray<TPair<TWeakObjectPtr<AActor>, FTransform>> ActorTransforms;
        TArray<FActorAvailability> ActorAvailability;
        TArray<TPair<TWeakObjectPtr<UPrimitiveComponent>, bool>> OverlapSettings;
        USphereComponent* OverlapFixture = nullptr;
        AActor* ItemActor = nullptr;
        TSharedRef<FJsonObject> Report;
        TArray<TSharedPtr<FJsonValue>> Checks;
        int32 PassedChecks = 0;
        double StartedAt = 0.0;
    };

    class FWaitAndRunInteraction final : public IAutomationLatentCommand
    {
    public:
        explicit FWaitAndRunInteraction(FAutomationTestBase* InTest) : Test(InTest) {}

        virtual bool Update() override
        {
            const double Now = FPlatformTime::Seconds();
            if (StartedAt < 0.0)
            {
                StartedAt = Now;
            }
            UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
            if (IsValid(World) && World->WorldType == EWorldType::PIE && World->HasBegunPlay())
            {
                if (InitialGameTime < 0.0)
                {
                    InitialGameTime = World->GetTimeSeconds();
                }
                if (World->GetTimeSeconds() - InitialGameTime >= 2.0)
                {
                    FInteractionFixture Fixture(Test, World);
                    Fixture.Run();
                    return true;
                }
            }
            if (Now - StartedAt >= 30.0)
            {
                Test->AddError(TEXT("InteractionContracts: original PIE did not begin and advance two game seconds within 30 real seconds"));
                return true;
            }
            return false;
        }

    private:
        FAutomationTestBase* Test;
        double StartedAt = -1.0;
        double InitialGameTime = -1.0;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNpcGameplayInteractionContracts,
    "NPC.Gameplay.InteractionContracts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNpcGameplayInteractionContracts::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("An Unreal Editor instance is available"), GEditor))
    {
        return false;
    }
    if (GEditor->PlayWorld)
    {
        AddError(TEXT("InteractionContracts requires no pre-existing PIE session"));
        return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(NpcInteractionAudit::OriginalMap));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(NpcInteractionAudit::FWaitAndRunInteraction(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
