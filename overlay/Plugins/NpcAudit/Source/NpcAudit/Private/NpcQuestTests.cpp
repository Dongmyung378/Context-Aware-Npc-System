#include "NpcAuditLibrary.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "JsonObjectConverter.h"
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

#include <initializer_list>

namespace NpcQuestAudit
{
    const TCHAR* const TestMap = TEXT("/Game/ThirdPerson/Maps/ThirdPersonMap");

    static FString Encode(const TSharedPtr<FJsonValue>& Value)
    {
        FString Text;
        TArray<TSharedPtr<FJsonValue>> Envelope;
        Envelope.Add(Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
        if (!FJsonSerializer::Serialize(Envelope, Writer)
            || !Text.StartsWith(TEXT("[")) || !Text.EndsWith(TEXT("]")))
        {
            return FString();
        }
        return Text.Mid(1, Text.Len() - 2);
    }

    class FAudit
    {
    public:
        explicit FAudit(FAutomationTestBase* InTest) : Test(InTest) {}

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
            const TSharedPtr<FJsonObject> Result = Checked(
                UNpcAuditLibrary::ReadProperty(Object, Name), FString(TEXT("Read ")) + Name);
            const TSharedPtr<FJsonObject>* Property = nullptr;
            if (Result.IsValid() && Result->TryGetObjectField(TEXT("property"), Property))
            {
                const TSharedPtr<FJsonValue>* Value = (*Property)->Values.Find(TEXT("value"));
                if (Value)
                {
                    return *Value;
                }
            }
            Test->AddError(FString(TEXT("Property value missing: ")) + Name);
            return nullptr;
        }

        bool Write(UObject* Object, const TCHAR* Name, const TSharedPtr<FJsonValue>& Value)
        {
            return Value.IsValid() && Checked(UNpcAuditLibrary::WriteProperty(
                Object, Name, Encode(Value)), FString(TEXT("Write ")) + Name).IsValid();
        }

        bool Number(UObject* Object, const TCHAR* Name, double Value)
        {
            return Write(Object, Name, MakeShared<FJsonValueNumber>(Value));
        }

        bool Boolean(UObject* Object, const TCHAR* Name, bool Value)
        {
            return Write(Object, Name, MakeShared<FJsonValueBoolean>(Value));
        }

        TSharedPtr<FJsonObject> Call(UObject* Object, const TCHAR* Function,
            const FString& Arguments = TEXT("{}"))
        {
            return Checked(UNpcAuditLibrary::Invoke(Object, Function, Arguments),
                FString(TEXT("Invoke ")) + Function);
        }

        bool Interact(APawn* Npc, const TCHAR* Key)
        {
            return Call(Npc, TEXT("Interact"), FString::Printf(
                TEXT("{\"InteractionKey\":\"%s\"}"), Key)).IsValid();
        }

        UObject* Reference(const TSharedPtr<FJsonValue>& Value, const FString& Role,
            bool bRequired = true)
        {
            UObject* Object = Value.IsValid() && Value->Type == EJson::String
                ? FindObject<UObject>(nullptr, *Value->AsString()) : nullptr;
            if (bRequired)
            {
                Test->TestNotNull(Role, Object);
            }
            return Object;
        }

        TSharedPtr<FJsonValue> BlackboardValue(UObject* Board, const TCHAR* Key)
        {
            const TSharedPtr<FJsonObject> Result = Call(Board, TEXT("GetValueAsBool"),
                FString::Printf(TEXT("{\"KeyName\":\"%s\"}"), Key));
            const TSharedPtr<FJsonObject>* Parameters = nullptr;
            if (Result.IsValid() && Result->TryGetObjectField(TEXT("parameters"), Parameters))
            {
                const TSharedPtr<FJsonValue>* Value = (*Parameters)->Values.Find(TEXT("ReturnValue"));
                if (Value)
                {
                    return *Value;
                }
            }
            Test->AddError(TEXT("Blackboard GetValueAsBool did not return a value"));
            return nullptr;
        }

        void SetBlackboard(UObject* Board, const TCHAR* Key, bool Value)
        {
            Call(Board, TEXT("SetValueAsBool"), FString::Printf(
                TEXT("{\"KeyName\":\"%s\",\"BoolValue\":%s}"),
                Key, Value ? TEXT("true") : TEXT("false")));
        }

        FAutomationTestBase* Test;
    };

    class FScopedProperties
    {
    public:
        FScopedProperties(FAudit& InAudit, UObject* InObject,
            std::initializer_list<const TCHAR*> Names) : Audit(InAudit), Object(InObject)
        {
            for (const TCHAR* Name : Names)
            {
                const TSharedPtr<FJsonValue> Value = Audit.Read(Object, Name);
                if (Value.IsValid())
                {
                    Values.Emplace(Name, Value);
                }
            }
        }
        ~FScopedProperties()
        {
            for (int32 Index = Values.Num() - 1; Index >= 0; --Index)
            {
                Audit.Write(Object, *Values[Index].Key, Values[Index].Value);
            }
        }
    private:
        FAudit& Audit;
        UObject* Object;
        TArray<TPair<FString, TSharedPtr<FJsonValue>>> Values;
    };

    /** Original inputs and widgets are exercised; cleanup is confined to this fresh PIE. */
    class FQuestFixture
    {
    public:
        FQuestFixture(FAudit& InAudit, APawn* InPlayer, APawn* InNpc)
            : Audit(InAudit), Player(InPlayer), Npc(InNpc),
              PlayerState(Audit, Player, {TEXT("PlayerIsInteracting"), TEXT("Current_Stamina"),
                  TEXT("Money"), TEXT("Inventory")}),
              NpcState(Audit, Npc, {TEXT("Trust_Level"), TEXT("IsInteracting"), TEXT("IsFollowing"),
                  TEXT("InteractionEnd"), TEXT("Dialogue_Widget_Ref"), TEXT("Shop_Widget_Ref"),
                  TEXT("QuestStatus"), TEXT("ActiveQuest"), TEXT("CheckQuestCondition"),
                  TEXT("Target_Index"), TEXT("Target_Quantity")})
        {
            bWasHidden = Npc->IsHidden();
            bHadCollision = Npc->GetActorEnableCollision();
            OriginalDialogue = Audit.Reference(Audit.Read(Npc, TEXT("Dialogue_Widget_Ref")), TEXT("Original dialogue"), false);
            OriginalShop = Audit.Reference(Audit.Read(Npc, TEXT("Shop_Widget_Ref")), TEXT("Original shop"), false);
            Board = Audit.Reference(Audit.Read(Npc->GetController(), TEXT("Blackboard")), TEXT("Charles Blackboard"));
            if (Board)
            {
                for (const TCHAR* Key : {TEXT("IsFollowing"), TEXT("IsInteracting"), TEXT("IsAtHome")})
                {
                    const TSharedPtr<FJsonValue> Value = Audit.BlackboardValue(Board, Key);
                    if (Value.IsValid() && Value->Type == EJson::Boolean)
                    {
                        BoardValues.Emplace(Key, Value->AsBool());
                    }
                }
                Audit.SetBlackboard(Board, TEXT("IsFollowing"), false);
                Audit.SetBlackboard(Board, TEXT("IsInteracting"), false);
            }
            bReady = Board
                && Audit.Boolean(Player, TEXT("PlayerIsInteracting"), false)
                && Audit.Boolean(Npc, TEXT("IsInteracting"), false)
                && Audit.Boolean(Npc, TEXT("IsFollowing"), false)
                && Audit.Boolean(Npc, TEXT("InteractionEnd"), false)
                && Audit.Boolean(Npc, TEXT("QuestStatus"), false)
                && Audit.Boolean(Npc, TEXT("CheckQuestCondition"), false)
                && Audit.Number(Npc, TEXT("Target_Index"), 0.0)
                && Audit.Number(Npc, TEXT("Target_Quantity"), 0.0)
                && Audit.Number(Npc, TEXT("Trust_Level"), 10.0)
                && Audit.Number(Player, TEXT("Current_Stamina"), 100.0)
                && Audit.Call(Npc, TEXT("ReceiveTick"), TEXT("{\"DeltaSeconds\":0.0}")).IsValid();
        }

        bool Open()
        {
            return bReady && Audit.Interact(Npc, TEXT("Interact"))
                && Audit.Reference(Audit.Read(Npc, TEXT("Dialogue_Widget_Ref")), TEXT("Actual quest dialogue"));
        }

        ~FQuestFixture()
        {
            UObject* CreatedDialogue = Audit.Reference(Audit.Read(Npc, TEXT("Dialogue_Widget_Ref")), TEXT("Fixture dialogue"), false);
            UObject* CreatedShop = Audit.Reference(Audit.Read(Npc, TEXT("Shop_Widget_Ref")), TEXT("Fixture shop"), false);
            const TSharedPtr<FJsonValue> Interacting = Audit.Read(Npc, TEXT("IsInteracting"));
            if (Interacting.IsValid() && Interacting->Type == EJson::Boolean && Interacting->AsBool())
            {
                Audit.Interact(Npc, TEXT("Interact"));
            }
            // Preserve any pre-existing widget. Remove only newly created fixture
            // widgets, even when an original close branch leaves a stale reference.
            if (IsValid(CreatedDialogue) && CreatedDialogue != OriginalDialogue)
            {
                Audit.Call(CreatedDialogue, TEXT("RemoveFromParent"));
            }
            if (IsValid(CreatedShop) && CreatedShop != OriginalShop)
            {
                Audit.Call(CreatedShop, TEXT("RemoveFromParent"));
            }
            for (const TPair<FString, bool>& Value : BoardValues)
            {
                Audit.SetBlackboard(Board, *Value.Key, Value.Value);
            }
            Npc->SetActorHiddenInGame(bWasHidden);
            Npc->SetActorEnableCollision(bHadCollision);
        }
    private:
        FAudit& Audit;
        APawn* Player;
        APawn* Npc;
        FScopedProperties PlayerState;
        FScopedProperties NpcState;
        UObject* Board = nullptr;
        UObject* OriginalDialogue = nullptr;
        UObject* OriginalShop = nullptr;
        TArray<TPair<FString, bool>> BoardValues;
        bool bWasHidden = false;
        bool bHadCollision = true;
        bool bReady = false;
    };

    static TSharedPtr<FJsonObject> ActualRow(FAudit& Audit, UDataTable* Table, const TCHAR* Row)
    {
        if (!Table || !Table->GetRowStruct())
        {
            Audit.Test->AddError(TEXT("Required original DataTable is unavailable"));
            return nullptr;
        }
        const uint8* Data = Table->FindRowUnchecked(FName(Row));
        if (!Data)
        {
            Audit.Test->AddError(FString(TEXT("Original DataTable row missing: ")) + Row);
            return nullptr;
        }
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        if (!FJsonObjectConverter::UStructToJsonObject(Table->GetRowStruct(), Data, Json,
            0, 0, nullptr, EJsonObjectConversionFlags::SkipStandardizeCase))
        {
            Audit.Test->AddError(FString(TEXT("Cannot export actual DataTable row: ")) + Row);
            return nullptr;
        }
        return Json;
    }

    static void VerifyNumber(FAudit& Audit, const TSharedPtr<FJsonObject>& Row,
        const TCHAR* Field, double Expected, const FString& Label)
    {
        double Actual = 0.0;
        Audit.Test->TestTrue(Label + TEXT(" ") + Field,
            Row.IsValid() && Row->TryGetNumberField(Field, Actual)
            && FMath::IsNearlyEqual(Actual, Expected, 0.000001));
    }

    static void VerifyString(FAudit& Audit, const TSharedPtr<FJsonObject>& Row,
        const TCHAR* Field, const TCHAR* Expected, const FString& Label)
    {
        FString Actual;
        Audit.Test->TestTrue(Label + TEXT(" ") + Field,
            Row.IsValid() && Row->TryGetStringField(Field, Actual) && Actual == Expected);
    }

    static TSharedRef<FJsonObject> ExpectedState(double Money, double Trust,
        bool bQuestActive, int32 AppleCount, int32 InventoryEntries)
    {
        TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
        State->SetNumberField(TEXT("money"), Money);
        State->SetNumberField(TEXT("trust"), Trust);
        State->SetBoolField(TEXT("quest_active"), bQuestActive);
        State->SetNumberField(TEXT("apple_count"), AppleCount);
        State->SetNumberField(TEXT("inventory_entries"), InventoryEntries);
        return State;
    }

    static void CheckState(FAudit& Audit, APawn* Player, APawn* Npc,
        const FString& Id, const FString& QuestRow, const TSharedRef<FJsonObject>& Expected,
        TArray<TSharedPtr<FJsonValue>>& Cases)
    {
        TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
        for (const TPair<const TCHAR*, TSharedPtr<FJsonValue>>& Field : {
            TPair<const TCHAR*, TSharedPtr<FJsonValue>>(TEXT("money"), Audit.Read(Player, TEXT("Money"))),
            TPair<const TCHAR*, TSharedPtr<FJsonValue>>(TEXT("trust"), Audit.Read(Npc, TEXT("Trust_Level"))),
            TPair<const TCHAR*, TSharedPtr<FJsonValue>>(TEXT("quest_active"), Audit.Read(Npc, TEXT("QuestStatus")))})
        {
            Actual->SetField(Field.Key, Field.Value.IsValid() ? Field.Value : MakeShared<FJsonValueNull>());
        }
        const TSharedPtr<FJsonValue> Inventory = Audit.Read(Player, TEXT("Inventory"));
        bool bInventoryValid = Inventory.IsValid() && Inventory->Type == EJson::Array;
        int32 AppleCount = 0;
        int32 Entries = -1;
        if (bInventoryValid)
        {
            Entries = Inventory->AsArray().Num();
            for (const TSharedPtr<FJsonValue>& Item : Inventory->AsArray())
            {
                FString Name;
                double Count = 0.0;
                if (!Item.IsValid() || Item->Type != EJson::Object
                    || !Item->AsObject()->TryGetStringField(TEXT("Item_Name"), Name)
                    || !Item->AsObject()->TryGetNumberField(TEXT("Count"), Count))
                {
                    bInventoryValid = false;
                    break;
                }
                if (Name == TEXT("Apple"))
                {
                    AppleCount += static_cast<int32>(Count);
                }
            }
        }
        Actual->SetNumberField(TEXT("apple_count"), AppleCount);
        Actual->SetNumberField(TEXT("inventory_entries"), Entries);
        bool bMatches = bInventoryValid;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Expected->Values)
        {
            const TSharedPtr<FJsonValue>* Observed = Actual->Values.Find(Field.Key);
            const bool bEqual = Observed && Observed->IsValid() && Field.Value.IsValid()
                && Field.Value->Type == (*Observed)->Type
                && ((Field.Value->Type == EJson::Number
                    && FMath::IsNearlyEqual(Field.Value->AsNumber(), (*Observed)->AsNumber(), 0.000001))
                    || (Field.Value->Type == EJson::Boolean && Field.Value->AsBool() == (*Observed)->AsBool()));
            Audit.Test->TestTrue(Id + TEXT(" ") + Field.Key + TEXT(" expected ")
                + Encode(Field.Value) + TEXT(" actual ") + Encode(Observed ? *Observed : nullptr), bEqual);
            bMatches &= bEqual;
        }
        Audit.Test->TestTrue(Id + TEXT(" reads a valid original inventory"), bInventoryValid);
        TSharedRef<FJsonObject> Case = MakeShared<FJsonObject>();
        Case->SetStringField(TEXT("id"), Id);
        Case->SetStringField(TEXT("quest_row"), QuestRow);
        Case->SetStringField(TEXT("oracle"), TEXT("Original DT_QuestList authored requirements/rewards; fixed independent expected amounts. Duplicate input is within the same interaction, not a newly assigned quest."));
        Case->SetObjectField(TEXT("expected"), Expected);
        Case->SetObjectField(TEXT("actual"), Actual);
        Case->SetBoolField(TEXT("passed"), bMatches);
        Cases.Add(MakeShared<FJsonValueObject>(Case));
    }

    static void WriteReport(FAutomationTestBase* Test, const TSharedRef<FJsonObject>& Report)
    {
        Report->SetBoolField(TEXT("assertions_passed"), !Test->HasAnyErrors());
        FString Path;
        if (!FParse::Value(FCommandLine::Get(), TEXT("NpcQuestReport="), Path) || Path.IsEmpty())
        {
            Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NpcAudit"), TEXT("quest-contracts.json"));
        }
        Path = FPaths::ConvertRelativePathToFull(Path);
        FString Json;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
        const bool bSaved = IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true)
            && FJsonSerializer::Serialize(Report, Writer)
            && FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        Test->TestTrue(TEXT("Actual quest report was written"), bSaved);
        if (bSaved)
        {
            Test->AddInfo(TEXT("NPC quest audit report: ") + Path);
        }
    }

    static void Run(FAutomationTestBase* Test, UWorld* World)
    {
        FAudit Audit(Test);
        TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
        Report->SetStringField(TEXT("schema"), TEXT("npc-gameplay-quest-contracts/v1"));
        Report->SetStringField(TEXT("test"), TEXT("NPC.Gameplay.QuestContracts"));
        Report->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
        Report->SetStringField(TEXT("map"), UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()));
        Report->SetStringField(TEXT("scope"), TEXT("Fresh actual PIE, Charles, original Talk/quest logic. Real authored DataTable rows seed active quests; no replacement quest implementation or quest-generation RNG. Does not cover every quest row, keyboard input, long-running UI, or navigation."));
        Report->SetStringField(TEXT("isolation"), TEXT("Synchronous cases snapshot player/NPC/Blackboard properties and remove their newly created dialogue/shop widgets. Controller input focus and native latent/navigation state are not claimed fully restored; this dedicated PIE session is ended after the suite. No assets are saved."));
        APlayerController* Controller = World->GetFirstPlayerController();
        APawn* Player = Controller ? Controller->GetPawn() : nullptr;
        AGameModeBase* GameMode = World->GetAuthGameMode();
        UClass* NpcClass = LoadClass<APawn>(nullptr,
            TEXT("/Game/ThirdPerson/Blueprints/BP_NPC_Standard.BP_NPC_Standard_C"));
        UDataTable* QuestTable = LoadObject<UDataTable>(nullptr, TEXT("/Game/AI/DT_QuestList.DT_QuestList"));
        UDataTable* ItemTable = LoadObject<UDataTable>(nullptr, TEXT("/Game/AI/DT_Item_Data.DT_Item_Data"));
        APawn* Charles = nullptr;
        TArray<TPair<AActor*, bool>> TickStates;
        if (Player) TickStates.Emplace(Player, Player->IsActorTickEnabled());
        if (GameMode) TickStates.Emplace(GameMode, GameMode->IsActorTickEnabled());
        if (NpcClass)
        {
            for (TActorIterator<APawn> It(World, NpcClass); It; ++It)
            {
                const TSharedPtr<FJsonValue> Data = Audit.Read(*It, TEXT("My_NPC_Data"));
                FString Name;
                if (Data.IsValid() && Data->Type == EJson::Object
                    && Data->AsObject()->TryGetStringField(TEXT("NPC_Name"), Name) && Name == TEXT("Charles"))
                {
                    Charles = *It;
                }
                TickStates.Emplace(*It, It->IsActorTickEnabled());
            }
        }
        Test->TestNotNull(TEXT("Actual quest player"), Player);
        Test->TestNotNull(TEXT("Actual quest GameMode"), GameMode);
        Test->TestNotNull(TEXT("Actual original Charles actor"), Charles);
        Test->TestNotNull(TEXT("Original DT_QuestList"), QuestTable);
        Test->TestNotNull(TEXT("Original DT_Item_Data"), ItemTable);
        const TSharedPtr<FJsonObject> AppleQuest = ActualRow(Audit, QuestTable, TEXT("Q_Apple"));
        const TSharedPtr<FJsonObject> LoanQuest = ActualRow(Audit, QuestTable, TEXT("Q_Gold_Loan"));
        const TSharedPtr<FJsonObject> Apple = ActualRow(Audit, ItemTable, TEXT("Apple"));
        if (!Player || !GameMode || !Charles || !AppleQuest.IsValid() || !LoanQuest.IsValid() || !Apple.IsValid())
        {
            Report->SetStringField(TEXT("status"), TEXT("setup_failed"));
            WriteReport(Test, Report);
            return;
        }
        VerifyNumber(Audit, AppleQuest, TEXT("Target_Amount"), 5.0, TEXT("Q_Apple"));
        VerifyNumber(Audit, AppleQuest, TEXT("Reward_Gold"), 10.0, TEXT("Q_Apple"));
        VerifyNumber(Audit, AppleQuest, TEXT("Reward_Trust"), 5.0, TEXT("Q_Apple"));
        VerifyNumber(Audit, LoanQuest, TEXT("Target_Amount"), 30.0, TEXT("Q_Gold_Loan"));
        VerifyNumber(Audit, LoanQuest, TEXT("Reward_Gold"), 0.0, TEXT("Q_Gold_Loan"));
        VerifyNumber(Audit, LoanQuest, TEXT("Reward_Trust"), 9.0, TEXT("Q_Gold_Loan"));
        VerifyString(Audit, AppleQuest, TEXT("Quest_Type"), TEXT("Item_Collection"), TEXT("Q_Apple"));
        VerifyString(Audit, AppleQuest, TEXT("Target_Item"), TEXT("Apple"), TEXT("Q_Apple"));
        VerifyString(Audit, LoanQuest, TEXT("Quest_Type"), TEXT("Gold_Donation"), TEXT("Q_Gold_Loan"));
        VerifyString(Audit, Apple, TEXT("Item_Name"), TEXT("Apple"), TEXT("Apple item row"));
        const TSharedPtr<FJsonValue> CharlesRow = Audit.Read(Charles, TEXT("NPC_Row_Name"));
        Test->TestTrue(TEXT("Charles is bound to original NPC_01"),
            CharlesRow.IsValid() && CharlesRow->Type == EJson::String && CharlesRow->AsString() == TEXT("NPC_01"));
        Report->SetStringField(TEXT("quest_table"), QuestTable->GetPathName());
        Report->SetObjectField(TEXT("Q_Apple_authored_row"), AppleQuest.ToSharedRef());
        Report->SetObjectField(TEXT("Q_Gold_Loan_authored_row"), LoanQuest.ToSharedRef());
        Report->SetObjectField(TEXT("Apple_authored_item_row"), Apple.ToSharedRef());
        for (const TPair<AActor*, bool>& State : TickStates) State.Key->SetActorTickEnabled(false);
        TArray<TSharedPtr<FJsonValue>> Cases;
        {
            FScopedProperties Clock(Audit, GameMode, {TEXT("CurrentTime"), TEXT("TimeSpeed"), TEXT("ActiveBGM")});
            Audit.Number(GameMode, TEXT("CurrentTime"), 600.0);
            Audit.Number(GameMode, TEXT("TimeSpeed"), 0.0);
            for (int32 Count : {4, 5, 6})
            {
                FQuestFixture Fixture(Audit, Player, Charles);
                const TSharedPtr<FJsonObject> Item = ActualRow(Audit, ItemTable, TEXT("Apple"));
                if (!Item.IsValid()) continue;
                Item->SetNumberField(TEXT("Count"), Count);
                TArray<TSharedPtr<FJsonValue>> Inventory;
                Inventory.Add(MakeShared<FJsonValueObject>(Item));
                const bool bReady = Audit.Write(Player, TEXT("Inventory"), MakeShared<FJsonValueArray>(Inventory))
                    && Audit.Number(Player, TEXT("Money"), 100.0) && Fixture.Open()
                    && Audit.Write(Charles, TEXT("ActiveQuest"), MakeShared<FJsonValueObject>(AppleQuest))
                    && Audit.Boolean(Charles, TEXT("QuestStatus"), true);
                if (!bReady)
                {
                    Test->AddError(TEXT("Q_Apple active fixture could not be established"));
                    continue;
                }
                Audit.Interact(Charles, TEXT("Talk"));
                const bool bSuccess = Count >= 5;
                const TSharedRef<FJsonObject> Expected = ExpectedState(
                    bSuccess ? 110.0 : 100.0, bSuccess ? 15.0 : 10.0, !bSuccess,
                    bSuccess ? Count - 5 : Count, Count == 5 ? 0 : 1);
                const FString Id = FString::Printf(TEXT("quest.apple.count_%d"), Count);
                CheckState(Audit, Player, Charles, Id, TEXT("Q_Apple"), Expected, Cases);
                if (bSuccess)
                {
                    // A real second Talk in the same interaction must not grant
                    // that completed quest's money or trust reward a second time.
                    Audit.Interact(Charles, TEXT("Talk"));
                    CheckState(Audit, Player, Charles, Id + TEXT(".duplicate_talk"), TEXT("Q_Apple"), Expected, Cases);
                }
            }
            for (int32 Money : {29, 30})
            {
                FQuestFixture Fixture(Audit, Player, Charles);
                const bool bReady = Audit.Write(Player, TEXT("Inventory"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>()))
                    && Audit.Number(Player, TEXT("Money"), Money) && Fixture.Open()
                    && Audit.Write(Charles, TEXT("ActiveQuest"), MakeShared<FJsonValueObject>(LoanQuest))
                    && Audit.Boolean(Charles, TEXT("QuestStatus"), true);
                if (!bReady)
                {
                    Test->AddError(TEXT("Q_Gold_Loan active fixture could not be established"));
                    continue;
                }
                Audit.Interact(Charles, TEXT("Talk"));
                const bool bSuccess = Money == 30;
                const TSharedRef<FJsonObject> Expected = ExpectedState(
                    bSuccess ? 0.0 : 29.0, bSuccess ? 19.0 : 10.0, !bSuccess, 0, 0);
                const FString Id = FString::Printf(TEXT("quest.loan.money_%d"), Money);
                CheckState(Audit, Player, Charles, Id, TEXT("Q_Gold_Loan"), Expected, Cases);
                if (bSuccess)
                {
                    Audit.Interact(Charles, TEXT("Talk"));
                    CheckState(Audit, Player, Charles, Id + TEXT(".duplicate_talk"), TEXT("Q_Gold_Loan"), Expected, Cases);
                }
            }
        }
        for (const TPair<AActor*, bool>& State : TickStates) State.Key->SetActorTickEnabled(State.Value);
        Test->TestEqual(TEXT("Five quest fixtures and three duplicate-input checkpoints executed"), Cases.Num(), 8);
        int32 Passed = 0;
        for (const TSharedPtr<FJsonValue>& Case : Cases)
            Passed += Case->AsObject()->GetBoolField(TEXT("passed")) ? 1 : 0;
        Report->SetNumberField(TEXT("base_scenarios"), 5);
        Report->SetNumberField(TEXT("case_count"), Cases.Num());
        Report->SetNumberField(TEXT("passed_cases"), Passed);
        Report->SetNumberField(TEXT("failed_cases"), Cases.Num() - Passed);
        Report->SetArrayField(TEXT("cases"), Cases);
        Report->SetStringField(TEXT("status"), Test->HasAnyErrors() ? TEXT("failed") : TEXT("passed"));
        WriteReport(Test, Report);
    }

    class FWaitForQuestWorld final : public IAutomationLatentCommand
    {
    public:
        explicit FWaitForQuestWorld(FAutomationTestBase* InTest) : Test(InTest) {}
        virtual bool Update() override
        {
            const double Now = FPlatformTime::Seconds();
            if (StartedAt < 0.0) StartedAt = Now;
            if (Now - StartedAt >= 30.0)
            {
                Test->AddError(TEXT("Quest PIE did not begin and advance two game seconds within 30 real seconds"));
                TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
                Report->SetStringField(TEXT("schema"), TEXT("npc-gameplay-quest-contracts/v1"));
                Report->SetStringField(TEXT("test"), TEXT("NPC.Gameplay.QuestContracts"));
                Report->SetStringField(TEXT("status"), TEXT("setup_failed"));
                Report->SetStringField(TEXT("error"), TEXT("actual_pie_world_wait_timeout"));
                WriteReport(Test, Report);
                return true;
            }
            UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
            if (IsValid(World) && World->WorldType == EWorldType::PIE && World->HasBegunPlay())
            {
                if (!ObservedWorld.IsValid())
                {
                    ObservedWorld = World;
                    InitialGameTime = World->GetTimeSeconds();
                }
                if (!Test->TestTrue(TEXT("The quest PIE world remains the observed original world"), ObservedWorld.Get() == World))
                    return true;
                if (World->GetTimeSeconds() - InitialGameTime >= 2.0)
                {
                    Test->TestEqual(TEXT("The original map runs in fresh quest PIE"),
                        UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()), FString(TestMap));
                    Run(Test, World);
                    return true;
                }
            }
            return false;
        }
    private:
        FAutomationTestBase* Test;
        TWeakObjectPtr<UWorld> ObservedWorld;
        double StartedAt = -1.0;
        double InitialGameTime = 0.0;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNpcGameplayQuestContracts,
    "NPC.Gameplay.QuestContracts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNpcGameplayQuestContracts::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("An Unreal Editor instance is available for quest contracts"), GEditor))
        return false;
    if (GEditor->PlayWorld)
    {
        AddError(TEXT("QuestContracts requires no pre-existing PIE session"));
        return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(NpcQuestAudit::TestMap));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(NpcQuestAudit::FWaitForQuestWorld(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
