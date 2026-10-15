#include "NpcAuditLibrary.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Blueprint/UserWidget.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "Components/PanelWidget.h"
#include "Components/SphereComponent.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "InputCoreTypes.h"
#include "JsonObjectConverter.h"
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
#include "UObject/UnrealType.h"

#include <initializer_list>

namespace NpcInventoryInputAudit
{
    const TCHAR* const TestMap = TEXT("/Game/ThirdPerson/Maps/ThirdPersonMap");

    static FString Encode(const TSharedPtr<FJsonValue>& Value)
    {
        FString Json;
        TArray<TSharedPtr<FJsonValue>> Envelope;
        Envelope.Add(Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
        if (!FJsonSerializer::Serialize(Envelope, Writer)
            || !Json.StartsWith(TEXT("[")) || !Json.EndsWith(TEXT("]"))) return FString();
        return Json.Mid(1, Json.Len() - 2);
    }

    static TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        for (const FString& Value : Values) Result.Add(MakeShared<FJsonValueString>(Value));
        return Result;
    }

    /** Actual input stack and original widgets; never calls a pickup callback directly. */
    class FInventoryInputContracts final : public IAutomationLatentCommand
    {
    public:
        explicit FInventoryInputContracts(FAutomationTestBase* InTest) : Test(InTest)
        {
            Report = MakeShared<FJsonObject>();
            Report->SetStringField(TEXT("schema"), TEXT("npc-gameplay-inventory-input-contracts/v1"));
            Report->SetStringField(TEXT("test"), TEXT("NPC.Gameplay.InventoryInputContracts"));
            Report->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
            Report->SetStringField(TEXT("scope"), TEXT("Fresh actual PIE. Spawn the canonical BP_Item_Master with Item_ID Apple before BeginPlay, enter its actual overlap, inject E press/release through the real PlayerController input stack, and observe original inventory and60-second latent respawn. Shop tests call original Charles Interact/Trade and inspect original generated widget rows, not a manually populated sales list. Does not prove physical keyboard/OS focus, every item row, or rendered visual layout."));
            Report->SetStringField(TEXT("isolation"), TEXT("Runtime inventory/player/NPC/clock properties are snapshotted; newly created widgets and spawned fixture items are removed before dedicated EndPIE. Input focus and native latent histories are not claimed generally reversible. No assets are saved; no time acceleration or timer-forcing is used."));
            Report->SetStringField(TEXT("original_pickup_contract"), TEXT("Original pickup hides the actor and disables collision, then Delay60 seconds restores visibility/collision and re-decides rarity. It does not destroy the pickup actor."));
            Report->SetStringField(TEXT("shop_contract"), TEXT("Charles ItemsToSell populates all4 catalog rows into NPC_SalesList at each trust level. Original WBP_ShopMain filters NPC_ShopList display rows by Required_Trust:3 normal rows at99,4 including Titan Blood at100 and101."));
            Report->SetNumberField(TEXT("whole_wait_limit_seconds"), 110.0);
            Report->SetNumberField(TEXT("expected_checkpoints"), 6);
        }

        virtual bool Update() override
        {
            const double Now = FPlatformTime::Seconds();
            if (StartedAt < 0.0) StartedAt = Now;
            if (Now - StartedAt >= 110.0)
            {
                Test->AddError(TEXT("Inventory input contracts exceeded110 real seconds"));
                return Finish(TEXT("timeout"));
            }
            UWorld* Live = GEditor ? GEditor->PlayWorld : nullptr;
            if (Phase == EPhase::AwaitWorld)
            {
                if (Now - StartedAt >= 30.0)
                {
                    Test->AddError(TEXT("Inventory input PIE did not begin and advance two game seconds within30 real seconds"));
                    return Finish(TEXT("setup_failed"));
                }
                if (IsValid(Live) && Live->WorldType == EWorldType::PIE && Live->HasBegunPlay())
                {
                    if (!World.IsValid())
                    {
                        World = Live;
                        InitialWorldTime = Live->GetTimeSeconds();
                    }
                    if (World.Get() != Live)
                    {
                        Test->AddError(TEXT("The observed inventory PIE world changed"));
                        return Finish(TEXT("setup_failed"));
                    }
                    if (Live->GetTimeSeconds() - InitialWorldTime >= 2.0)
                    {
                        if (!Setup() || !SpawnPickup(false)) return Finish(TEXT("setup_failed"));
                    }
                }
                return false;
            }
            if (!World.IsValid() || World.Get() != Live || !Controller.IsValid()
                || !Player.IsValid() || !Charles.IsValid())
            {
                Test->AddError(TEXT("An actual inventory input fixture object became unavailable"));
                return Finish(TEXT("failed"));
            }
            if (Phase == EPhase::AwaitOverlap)
            {
                if (!CurrentItem.IsValid())
                {
                    Test->AddError(TEXT("Spawned pickup disappeared before input"));
                    return Finish(TEXT("failed"));
                }
                const bool bOverlap = CurrentItem->IsOverlappingActor(Player.Get());
                const UInputComponent* Input = CurrentItem->InputComponent;
                int32 EBindings = 0;
                if (Input)
                    for (const FInputKeyBinding& Binding : Input->KeyBindings)
                        EBindings += Binding.Chord.Key == EKeys::E && Binding.KeyEvent == IE_Pressed
                            && Binding.KeyDelegate.IsBound() ? 1 : 0;
                const bool bInStack = Input && Controller->IsInputComponentInStack(Input);
                if (bOverlap && bInStack && EBindings > 0 && GFrameCounter > SpawnFrame + 1)
                {
                    CurrentPickup->SetBoolField(TEXT("actual_player_overlap"), bOverlap);
                    CurrentPickup->SetBoolField(TEXT("input_component_in_controller_stack"), bInStack);
                    CurrentPickup->SetNumberField(TEXT("bound_E_pressed_callbacks"), EBindings);
                    CurrentPickup->SetNumberField(TEXT("overlap_wait_real_seconds"), Now - PhaseStarted);
                    CurrentPickup->SetBoolField(TEXT("press_InputKey_returned"), Controller->InputKey(
                        FInputKeyParams(EKeys::E, IE_Pressed, 1.0, false)));
                    PressFrame = GFrameCounter;
                    PressWorldTime = World->GetTimeSeconds();
                    CurrentPickup->SetNumberField(TEXT("pressed_world_time"), PressWorldTime);
                    if (bSecondPickup) SecondPressedWorldTime = PressWorldTime;
                    else FirstPressedWorldTime = PressWorldTime;
                    Phase = EPhase::ReleaseKey;
                    PhaseStarted = Now;
                }
                else if (Now - PhaseStarted >= 5.0)
                {
                    CurrentPickup->SetBoolField(TEXT("actual_player_overlap"), bOverlap);
                    CurrentPickup->SetBoolField(TEXT("input_component_in_controller_stack"), bInStack);
                    CurrentPickup->SetNumberField(TEXT("bound_E_pressed_callbacks"), EBindings);
                    AddCase(CurrentPickup.ToSharedRef(), false, TEXT("actual_overlap_and_E_input_binding_not_ready"));
                    return Finish(TEXT("failed"));
                }
            }
            else if (Phase == EPhase::ReleaseKey)
            {
                if (GFrameCounter > PressFrame)
                {
                    CurrentPickup->SetBoolField(TEXT("release_InputKey_returned"), Controller->InputKey(
                        FInputKeyParams(EKeys::E, IE_Released, 0.0, false)));
                    CurrentPickup->SetNumberField(TEXT("release_world_time"), World->GetTimeSeconds());
                    Phase = EPhase::AwaitPickup;
                    PhaseStarted = Now;
                }
            }
            else if (Phase == EPhase::AwaitPickup)
            {
                int32 Stacks = 0;
                const int32 Count = InventoryCount(Stacks);
                const int32 Expected = bSecondPickup ? 3 : 1;
                const bool bHidden = CurrentItem.IsValid() && CurrentItem->IsHidden();
                const bool bCollisionOff = CurrentItem.IsValid() && !CurrentItem->GetActorEnableCollision();
                if ((Count == Expected && Stacks == 1 && bHidden && bCollisionOff) || Now - PhaseStarted >= 5.0)
                {
                    CurrentPickup->SetNumberField(TEXT("expected_quantity"), Expected);
                    CurrentPickup->SetNumberField(TEXT("actual_quantity"), Count);
                    CurrentPickup->SetNumberField(TEXT("actual_matching_stacks"), Stacks);
                    CurrentPickup->SetBoolField(TEXT("original_actor_still_exists"), CurrentItem.IsValid());
                    CurrentPickup->SetBoolField(TEXT("hidden_after_pickup"), bHidden);
                    CurrentPickup->SetBoolField(TEXT("collision_disabled_after_pickup"), bCollisionOff);
                    CurrentPickup->SetNumberField(TEXT("observation_world_time"), World->GetTimeSeconds());
                    const TSharedPtr<FJsonValue> Inventory = Read(Player.Get(), TEXT("Inventory"));
                    CurrentPickup->SetField(TEXT("actual_inventory"), Inventory.IsValid() ? Inventory : MakeShared<FJsonValueNull>());
                    const bool bPassed = Count == Expected && Stacks == 1 && bHidden && bCollisionOff;
                    AddCase(CurrentPickup.ToSharedRef(), bPassed, TEXT("actual_E_input_inventory_and_hidden_cooldown"));
                    if (!bPassed) return Finish(TEXT("failed"));
                    if (!bSecondPickup)
                    {
                        if (!SpawnPickup(true)) return Finish(TEXT("setup_failed"));
                    }
                    else
                    {
                        ShopCases();
                        Phase = EPhase::AwaitRespawn;
                        PhaseStarted = Now;
                    }
                }
            }
            else if (Phase == EPhase::AwaitRespawn)
            {
                const double WorldElapsed = World->GetTimeSeconds() - SecondPressedWorldTime;
                const bool bFirstBack = FirstItem.IsValid() && !FirstItem->IsHidden() && FirstItem->GetActorEnableCollision();
                const bool bSecondBack = SecondItem.IsValid() && !SecondItem->IsHidden() && SecondItem->GetActorEnableCollision();
                if ((WorldElapsed >= 60.0 && bFirstBack && bSecondBack)
                    || WorldElapsed >= 75.0 || Now - PhaseStarted >= 85.0)
                {
                    int32 Stacks = 0;
                    const int32 Count = InventoryCount(Stacks);
                    TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
                    Row->SetStringField(TEXT("id"), TEXT("pickup.real_60_second_respawn"));
                    Row->SetStringField(TEXT("oracle"), TEXT("Original BP_Item_Master Delay60 restores visibility and collision without automatically adding inventory. Two actual E inputs already produced the expected3-item stack."));
                    Row->SetNumberField(TEXT("world_seconds_since_first_input"), World->GetTimeSeconds() - FirstPressedWorldTime);
                    Row->SetNumberField(TEXT("world_seconds_since_second_input"), WorldElapsed);
                    Row->SetNumberField(TEXT("real_respawn_wait_seconds"), Now - PhaseStarted);
                    Row->SetBoolField(TEXT("first_actor_visible_and_collision_enabled"), bFirstBack);
                    Row->SetBoolField(TEXT("second_actor_visible_and_collision_enabled"), bSecondBack);
                    Row->SetNumberField(TEXT("expected_inventory_quantity"), 3);
                    Row->SetNumberField(TEXT("actual_inventory_quantity"), Count);
                    Row->SetNumberField(TEXT("actual_matching_stacks"), Stacks);
                    AddCase(Row, WorldElapsed >= 60.0 && bFirstBack && bSecondBack && Count == 3 && Stacks == 1,
                        TEXT("actual_world_delay_respawn_without_extra_pickup"));
                    Test->TestEqual(TEXT("Two pickup, three shop boundary, one real respawn checkpoints executed"), Cases.Num(), 6);
                    return Finish(Test->HasAnyErrors() ? TEXT("failed") : TEXT("passed"));
                }
            }
            return false;
        }

    private:
        enum class EPhase { AwaitWorld, AwaitOverlap, ReleaseKey, AwaitPickup, AwaitRespawn, Done };
        struct FSavedProperty
        {
            TWeakObjectPtr<UObject> Owner;
            FString Name;
            TSharedPtr<FJsonValue> Value;
        };

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

        TSharedPtr<FJsonValue> Read(UObject* Owner, const TCHAR* Name)
        {
            const TSharedPtr<FJsonObject> Result = Checked(UNpcAuditLibrary::ReadProperty(Owner, Name), FString(TEXT("Read ")) + Name);
            const TSharedPtr<FJsonObject>* Property = nullptr;
            if (Result.IsValid() && Result->TryGetObjectField(TEXT("property"), Property))
            {
                const TSharedPtr<FJsonValue>* Value = (*Property)->Values.Find(TEXT("value"));
                if (Value) return *Value;
            }
            Test->AddError(FString(TEXT("Missing actual property value: ")) + Name);
            return nullptr;
        }

        bool Write(UObject* Owner, const TCHAR* Name, const TSharedPtr<FJsonValue>& Value)
        {
            return Value.IsValid() && Checked(UNpcAuditLibrary::WriteProperty(Owner, Name, Encode(Value)),
                FString(TEXT("Write ")) + Name).IsValid();
        }

        bool Number(UObject* Owner, const TCHAR* Name, double Value)
        { return Write(Owner, Name, MakeShared<FJsonValueNumber>(Value)); }
        bool Boolean(UObject* Owner, const TCHAR* Name, bool Value)
        { return Write(Owner, Name, MakeShared<FJsonValueBoolean>(Value)); }

        bool IsBool(UObject* Owner, const TCHAR* Name, bool Expected)
        {
            const TSharedPtr<FJsonValue> Value = Read(Owner, Name);
            return Value.IsValid() && Value->Type == EJson::Boolean && Value->AsBool() == Expected;
        }

        bool Interact(const TCHAR* Key)
        {
            return Checked(UNpcAuditLibrary::Invoke(Charles.Get(), TEXT("Interact"),
                FString::Printf(TEXT("{\"InteractionKey\":\"%s\"}"), Key)), FString(TEXT("Original Interact ")) + Key).IsValid();
        }

        UObject* Object(UObject* Owner, const TCHAR* Name)
        {
            FObjectPropertyBase* Property = Owner ? FindFProperty<FObjectPropertyBase>(Owner->GetClass(), Name) : nullptr;
            return Property ? Property->GetObjectPropertyValue_InContainer(Owner) : nullptr;
        }

        void Snapshot(UObject* Owner, std::initializer_list<const TCHAR*> Names)
        {
            for (const TCHAR* Name : Names)
            {
                const TSharedPtr<FJsonValue> Value = Read(Owner, Name);
                if (Value.IsValid()) Saved.Add({Owner, Name, Value});
            }
        }

        TSharedPtr<FJsonObject> Row(const TCHAR* Name)
        {
            const uint8* Data = Table.IsValid() ? Table->FindRowUnchecked(FName(Name)) : nullptr;
            if (!Data)
            {
                Test->AddError(FString(TEXT("Original DT_Item_Data row missing: ")) + Name);
                return nullptr;
            }
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            if (!FJsonObjectConverter::UStructToJsonObject(Table->GetRowStruct(), Data, Result,
                0, 0, nullptr, EJsonObjectConversionFlags::SkipStandardizeCase))
            {
                Test->AddError(FString(TEXT("Cannot export original item row: ")) + Name);
                return nullptr;
            }
            return Result;
        }

        bool Setup()
        {
            Test->TestEqual(TEXT("Inventory input uses original map"),
                UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()), FString(TestMap));
            Controller = World->GetFirstPlayerController();
            Player = Controller.IsValid() ? Cast<ACharacter>(Controller->GetPawn()) : nullptr;
            GameMode = World->GetAuthGameMode();
            Table = LoadObject<UDataTable>(nullptr, TEXT("/Game/AI/DT_Item_Data.DT_Item_Data"));
            ItemClass = LoadClass<AActor>(nullptr, TEXT("/Game/AI/BP_Item_Master.BP_Item_Master_C"));
            UClass* NpcClass = LoadClass<APawn>(nullptr,
                TEXT("/Game/ThirdPerson/Blueprints/BP_NPC_Standard.BP_NPC_Standard_C"));
            if (NpcClass)
                for (TActorIterator<APawn> It(World.Get(), NpcClass); It; ++It)
                {
                    const TSharedPtr<FJsonValue> Name = Read(*It, TEXT("NPC_Row_Name"));
                    if (Name.IsValid() && Name->Type == EJson::String && Name->AsString() == TEXT("NPC_01")) Charles = *It;
                }
            Test->TestNotNull(TEXT("Actual input PlayerController"), Controller.Get());
            Test->TestNotNull(TEXT("Actual input player character"), Player.Get());
            Test->TestNotNull(TEXT("Actual input GameMode"), GameMode.Get());
            Test->TestNotNull(TEXT("Actual Charles NPC_01"), Charles.Get());
            Test->TestNotNull(TEXT("Actual DT_Item_Data"), Table.Get());
            Test->TestNotNull(TEXT("Canonical BP_Item_Master class"), ItemClass.Get());
            if (!Controller.IsValid() || !Player.IsValid() || !GameMode.IsValid()
                || !Charles.IsValid() || !Table.IsValid() || !ItemClass.IsValid()) return false;
            Snapshot(GameMode.Get(), {TEXT("TimeSpeed"), TEXT("CurrentTime"), TEXT("ActiveBGM")});
            Snapshot(Player.Get(), {TEXT("Inventory"), TEXT("Money"), TEXT("PlayerIsInteracting"), TEXT("InventoryOpen")});
            Snapshot(Charles.Get(), {TEXT("Trust_Level"), TEXT("IsInteracting"), TEXT("IsFollowing"),
                TEXT("InteractionEnd"), TEXT("Dialogue_Widget_Ref"), TEXT("Shop_Widget_Ref")});
            Number(GameMode.Get(), TEXT("TimeSpeed"), 0.0);
            Number(GameMode.Get(), TEXT("CurrentTime"), 600.0);
            Number(Player.Get(), TEXT("Money"), 100.0);
            Boolean(Player.Get(), TEXT("PlayerIsInteracting"), false);
            Boolean(Player.Get(), TEXT("InventoryOpen"), false);
            Boolean(Charles.Get(), TEXT("IsInteracting"), false);
            Boolean(Charles.Get(), TEXT("InteractionEnd"), false);
            Controller->SetInputMode(FInputModeGameOnly());
            Controller->InputKey(FInputKeyParams(EKeys::E, IE_Released, 0.0, false));
            const TSharedPtr<FJsonValue> Sales = Read(Charles.Get(), TEXT("ItemsToSell"));
            Report->SetField(TEXT("authored_Charles_ItemsToSell"), Sales.IsValid() ? Sales : MakeShared<FJsonValueNull>());
            TArray<FString> Rows;
            if (Sales.IsValid() && Sales->Type == EJson::Array)
                for (const TSharedPtr<FJsonValue>& Value : Sales->AsArray())
                    if (Value.IsValid() && Value->Type == EJson::String) Rows.Add(Value->AsString());
            Rows.Sort();
            TArray<FString> ExpectedRows = {TEXT("Beer"), TEXT("Juice"), TEXT("Titan_Blood"), TEXT("Whiskey")};
            Test->TestTrue(TEXT("Charles real authored item catalog matches the independent four-row oracle"), Rows == ExpectedRows);
            const TSharedPtr<FJsonObject> Titan = Row(TEXT("Titan_Blood"));
            double Required = 0.0;
            Test->TestTrue(TEXT("Authored Titan Blood Required_Trust is100"), Titan.IsValid()
                && Titan->TryGetNumberField(TEXT("Required_Trust"), Required) && Required == 100.0);
            if (Titan.IsValid()) Report->SetObjectField(TEXT("Titan_Blood_authored_row"), Titan.ToSharedRef());
            Report->SetStringField(TEXT("canonical_pickup_class"), ItemClass->GetPathName());
            return !Test->HasAnyErrors();
        }

        bool SpawnPickup(bool bSecond)
        {
            bSecondPickup = bSecond;
            TArray<TSharedPtr<FJsonValue>> InitialInventory;
            if (bSecond)
            {
                const TSharedPtr<FJsonObject> Existing = Row(*SelectedRow);
                if (!Existing.IsValid()) return false;
                Existing->SetNumberField(TEXT("Count"), 2.0);
                InitialInventory.Add(MakeShared<FJsonValueObject>(Existing));
            }
            if (!Write(Player.Get(), TEXT("Inventory"), MakeShared<FJsonValueArray>(InitialInventory))) return false;
            const FVector Far = Player->GetActorLocation() + FVector(1500.0, 0.0, 0.0);
            const FTransform Transform(FRotator::ZeroRotator, Far);
            AActor* Spawned = World->SpawnActorDeferred<AActor>(ItemClass.Get(), Transform, nullptr, nullptr,
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
            Test->TestNotNull(TEXT("Actual deferred pickup fixture actor"), Spawned);
            if (!Spawned) return false;
            SpawnedItems.Add(Spawned);
            if (!Write(Spawned, TEXT("Item_ID"), MakeShared<FJsonValueString>(TEXT("Apple")))) return false;
            FMath::RandInit(378);
            FMath::SRandInit(378);
            UGameplayStatics::FinishSpawningActor(Spawned, Transform);
            const TSharedPtr<FJsonValue> Final = Read(Spawned, TEXT("Final_ID"));
            const FString FinalRow = Final.IsValid() && Final->Type == EJson::String ? Final->AsString() : FString();
            Test->TestTrue(TEXT("Original DecideItem chooses an authored Apple family row"),
                FinalRow == TEXT("Apple") || FinalRow == TEXT("Rare_Apple"));
            if (bSecond) Test->TestEqual(TEXT("Re-seeded original initialization produces the same pickup row for stacking fixture"), FinalRow, SelectedRow);
            else SelectedRow = FinalRow;
            const TSharedPtr<FJsonObject> Authored = Row(*SelectedRow);
            double AuthoredCount = 0.0;
            if (!Authored.IsValid() || !Authored->TryGetStringField(TEXT("Item_Name"), SelectedItemName)
                || !Authored->TryGetNumberField(TEXT("Count"), AuthoredCount) || AuthoredCount != 1.0)
            {
                Test->AddError(TEXT("Authored initialized pickup must have one item"));
                return false;
            }
            if (Test->HasAnyErrors()) return false;
            USphereComponent* Sphere = Cast<USphereComponent>(Object(Spawned, TEXT("Sphere")));
            Test->TestNotNull(TEXT("Original pickup overlap sphere"), Sphere);
            if (!Sphere) return false;
            const double Offset = FMath::Clamp((Sphere->GetScaledSphereRadius()
                + Player->GetCapsuleComponent()->GetScaledCapsuleRadius()) * 0.5, 10.0, 80.0);
            const FVector Near = Player->GetActorLocation() + Player->GetActorForwardVector() * Offset
                - FVector(0.0, 0.0, Player->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - 20.0);
            Spawned->SetActorLocation(Near, false, nullptr, ETeleportType::TeleportPhysics);
            Sphere->UpdateOverlaps();
            CurrentItem = Spawned;
            if (bSecond) SecondItem = Spawned;
            else FirstItem = Spawned;
            CurrentPickup = MakeShared<FJsonObject>();
            CurrentPickup->SetStringField(TEXT("id"), bSecond ? TEXT("pickup.E_existing_stack") : TEXT("pickup.E_new_stack"));
            CurrentPickup->SetStringField(TEXT("oracle"), TEXT("Actual overlap enables original actor E binding; real PlayerController E press/release must add exactly one authored item and enter the original hidden/collision-off cooldown."));
            CurrentPickup->SetStringField(TEXT("actor"), Spawned->GetPathName());
            CurrentPickup->SetStringField(TEXT("requested_base_Item_ID"), TEXT("Apple"));
            CurrentPickup->SetStringField(TEXT("actual_original_DecideItem_Final_ID"), FinalRow);
            CurrentPickup->SetObjectField(TEXT("authored_selected_item_row"), Authored.ToSharedRef());
            CurrentPickup->SetNumberField(TEXT("initial_inventory_quantity"), bSecond ? 2 : 0);
            CurrentPickup->SetNumberField(TEXT("original_sphere_radius"), Sphere->GetScaledSphereRadius());
            CurrentPickup->SetBoolField(TEXT("original_sphere_generates_overlap_events"), Sphere->GetGenerateOverlapEvents());
            Phase = EPhase::AwaitOverlap;
            PhaseStarted = FPlatformTime::Seconds();
            SpawnFrame = GFrameCounter;
            return true;
        }

        int32 InventoryCount(int32& Stacks)
        {
            Stacks = 0;
            int32 Count = 0;
            const TSharedPtr<FJsonValue> Inventory = Read(Player.Get(), TEXT("Inventory"));
            if (!Inventory.IsValid() || Inventory->Type != EJson::Array) return -1;
            for (const TSharedPtr<FJsonValue>& Item : Inventory->AsArray())
            {
                FString Name;
                double Amount = 0.0;
                if (!Item.IsValid() || Item->Type != EJson::Object
                    || !Item->AsObject()->TryGetStringField(TEXT("Item_Name"), Name)
                    || !Item->AsObject()->TryGetNumberField(TEXT("Count"), Amount)) return -1;
                if (Name == SelectedItemName)
                {
                    Count += static_cast<int32>(Amount);
                    ++Stacks;
                }
            }
            return Count;
        }

        void ShopCases()
        {
            for (int32 Trust : {99, 100, 101})
            {
                TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
                Result->SetStringField(TEXT("id"), FString::Printf(TEXT("shop.original_Trade.trust_%d"), Trust));
                Result->SetStringField(TEXT("oracle"), TEXT("Original NPC_01 ItemsToSell and DT_Item_Data Titan_Blood Required_Trust100: underlying catalog4, displayed normal rows3, special row visible only at trust>=100."));
                Result->SetNumberField(TEXT("trust_fixture"), Trust);
                bool bPassed = Number(Charles.Get(), TEXT("Trust_Level"), Trust)
                    && Interact(TEXT("Interact")) && IsBool(Charles.Get(), TEXT("IsInteracting"), true)
                    && Interact(TEXT("Trade"));
                UUserWidget* Shop = Cast<UUserWidget>(Object(Charles.Get(), TEXT("Shop_Widget_Ref")));
                UUserWidget* Dialogue = Cast<UUserWidget>(Object(Charles.Get(), TEXT("Dialogue_Widget_Ref")));
                if (Shop) CreatedWidgets.Add(Shop);
                if (Dialogue) CreatedWidgets.Add(Dialogue);
                const TSharedPtr<FJsonValue> Catalog = Shop ? Read(Shop, TEXT("NPC_SalesList")) : nullptr;
                TArray<FString> CatalogNames;
                if (Catalog.IsValid() && Catalog->Type == EJson::Array)
                    for (const TSharedPtr<FJsonValue>& Item : Catalog->AsArray())
                    {
                        FString Name;
                        if (Item.IsValid() && Item->Type == EJson::Object
                            && Item->AsObject()->TryGetStringField(TEXT("Item_Name"), Name)) CatalogNames.Add(Name);
                    }
                UPanelWidget* Panel = Shop ? Cast<UPanelWidget>(Object(Shop, TEXT("NPC_ShopList"))) : nullptr;
                TArray<FString> DisplayNames;
                if (Panel)
                    for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
                    {
                        UUserWidget* Line = Cast<UUserWidget>(Panel->GetChildAt(Index));
                        const TSharedPtr<FJsonValue> Item = Line ? Read(Line, TEXT("TargetItemData")) : nullptr;
                        FString Name;
                        if (Item.IsValid() && Item->Type == EJson::Object
                            && Item->AsObject()->TryGetStringField(TEXT("Item_Name"), Name)) DisplayNames.Add(Name);
                    }
                CatalogNames.Sort();
                DisplayNames.Sort();
                const TArray<FString> ExpectedCatalog = {TEXT("Beer"), TEXT("Juice"), TEXT("Titan Blood"), TEXT("Whiskey")};
                TArray<FString> ExpectedDisplay = {TEXT("Beer"), TEXT("Juice"), TEXT("Whiskey")};
                if (Trust >= 100) ExpectedDisplay.Add(TEXT("Titan Blood"));
                ExpectedDisplay.Sort();
                const bool bShopVisible = Shop && Shop->IsInViewport();
                bPassed &= bShopVisible && Panel && CatalogNames == ExpectedCatalog && DisplayNames == ExpectedDisplay
                    && Panel->GetChildrenCount() == ExpectedDisplay.Num();
                Result->SetBoolField(TEXT("actual_shop_in_viewport"), bShopVisible);
                Result->SetNumberField(TEXT("catalog_row_count"), CatalogNames.Num());
                Result->SetNumberField(TEXT("displayed_widget_count"), Panel ? Panel->GetChildrenCount() : -1);
                Result->SetArrayField(TEXT("actual_catalog_item_names"), Strings(CatalogNames));
                Result->SetArrayField(TEXT("expected_displayed_item_names"), Strings(ExpectedDisplay));
                Result->SetArrayField(TEXT("actual_displayed_item_names"), Strings(DisplayNames));
                const bool bClosed = Interact(TEXT("Interact"))
                    && IsBool(Charles.Get(), TEXT("IsInteracting"), false)
                    && IsBool(Player.Get(), TEXT("PlayerIsInteracting"), false)
                    && Shop && !Shop->IsInViewport() && Dialogue && !Dialogue->IsInViewport();
                Result->SetBoolField(TEXT("original_Interact_closed_widgets_and_flags"), bClosed);
                bPassed &= bClosed;
                AddCase(Result, bPassed, TEXT("actual_Trade_population_and_original_UI_trust_filter"));
            }
        }

        void AddCase(const TSharedRef<FJsonObject>& Row, bool bPassed, const FString& Reason)
        {
            Row->SetBoolField(TEXT("passed"), bPassed);
            Row->SetStringField(TEXT("reason"), Reason);
            Row->SetNumberField(TEXT("real_elapsed_seconds"), FPlatformTime::Seconds() - StartedAt);
            Cases.Add(MakeShared<FJsonValueObject>(Row));
            Test->TestTrue(Row->GetStringField(TEXT("id")) + TEXT(": ") + Reason, bPassed);
            CurrentPickup.Reset();
        }

        bool Finish(const FString& Status)
        {
            if (Phase == EPhase::Done) return true;
            if (Controller.IsValid()) Controller->InputKey(FInputKeyParams(EKeys::E, IE_Released, 0.0, false));
            if (Charles.IsValid() && IsBool(Charles.Get(), TEXT("IsInteracting"), true)) Interact(TEXT("Interact"));
            for (const TWeakObjectPtr<UUserWidget>& Widget : CreatedWidgets)
                if (Widget.IsValid()) Widget->RemoveFromParent();
            for (const TWeakObjectPtr<AActor>& Actor : SpawnedItems)
                if (Actor.IsValid()) Actor->Destroy();
            for (int32 Index = Saved.Num() - 1; Index >= 0; --Index)
                if (Saved[Index].Owner.IsValid()) Write(Saved[Index].Owner.Get(), *Saved[Index].Name, Saved[Index].Value);
            int32 Passed = 0;
            for (const TSharedPtr<FJsonValue>& Case : Cases) Passed += Case->AsObject()->GetBoolField(TEXT("passed")) ? 1 : 0;
            Report->SetStringField(TEXT("status"), Test->HasAnyErrors() ? TEXT("failed") : Status);
            Report->SetNumberField(TEXT("real_elapsed_seconds"), FPlatformTime::Seconds() - StartedAt);
            Report->SetNumberField(TEXT("case_count"), Cases.Num());
            Report->SetNumberField(TEXT("passed_cases"), Passed);
            Report->SetNumberField(TEXT("failed_cases"), Cases.Num() - Passed);
            Report->SetArrayField(TEXT("cases"), Cases);
            Report->SetBoolField(TEXT("assertions_passed"), !Test->HasAnyErrors());
            FString Path;
            if (!FParse::Value(FCommandLine::Get(), TEXT("NpcInventoryInputReport="), Path) || Path.IsEmpty())
                Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NpcAudit"), TEXT("inventory-input-contracts.json"));
            Path = FPaths::ConvertRelativePathToFull(Path);
            FString Json;
            const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
            const bool bSaved = IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true)
                && FJsonSerializer::Serialize(Report.ToSharedRef(), Writer)
                && FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            Test->TestTrue(TEXT("Actual inventory input report was written"), bSaved);
            if (bSaved) Test->AddInfo(TEXT("NPC inventory input report: ") + Path);
            Phase = EPhase::Done;
            return true;
        }

        FAutomationTestBase* Test;
        EPhase Phase = EPhase::AwaitWorld;
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<APlayerController> Controller;
        TWeakObjectPtr<ACharacter> Player;
        TWeakObjectPtr<AGameModeBase> GameMode;
        TWeakObjectPtr<APawn> Charles;
        TWeakObjectPtr<UDataTable> Table;
        TWeakObjectPtr<UClass> ItemClass;
        TWeakObjectPtr<AActor> CurrentItem;
        TWeakObjectPtr<AActor> FirstItem;
        TWeakObjectPtr<AActor> SecondItem;
        TArray<TWeakObjectPtr<AActor>> SpawnedItems;
        TArray<TWeakObjectPtr<UUserWidget>> CreatedWidgets;
        TArray<FSavedProperty> Saved;
        TArray<TSharedPtr<FJsonValue>> Cases;
        TSharedPtr<FJsonObject> Report;
        TSharedPtr<FJsonObject> CurrentPickup;
        FString SelectedRow;
        FString SelectedItemName;
        double StartedAt = -1.0;
        double InitialWorldTime = 0.0;
        double PhaseStarted = 0.0;
        double PressWorldTime = 0.0;
        double FirstPressedWorldTime = 0.0;
        double SecondPressedWorldTime = 0.0;
        uint64 SpawnFrame = 0;
        uint64 PressFrame = 0;
        bool bSecondPickup = false;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNpcGameplayInventoryInputContracts,
    "NPC.Gameplay.InventoryInputContracts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNpcGameplayInventoryInputContracts::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("Unreal Editor is available for inventory input contracts"), GEditor)) return false;
    if (GEditor->PlayWorld)
    {
        AddError(TEXT("InventoryInputContracts requires no pre-existing PIE session"));
        return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(NpcInventoryInputAudit::TestMap));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(NpcInventoryInputAudit::FInventoryInputContracts(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
