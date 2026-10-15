#include "NpcAuditLibrary.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
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

namespace NpcGameplayAudit
{
    constexpr double WorldWaitLimitSeconds = 30.0;
    constexpr double RequiredGameTimeSeconds = 2.0;
    const TCHAR* const TestMap = TEXT("/Game/ThirdPerson/Maps/ThirdPersonMap");

    static FString ReportPath(const TCHAR* DefaultFile = TEXT("world-inventory.json"))
    {
        FString Result;
        if (!FParse::Value(FCommandLine::Get(), TEXT("NpcAuditReport="), Result)
            || Result.IsEmpty())
        {
            Result = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NpcAudit"), DefaultFile);
        }
        return FPaths::ConvertRelativePathToFull(Result);
    }

    static TSharedRef<FJsonObject> Dump(
        FAutomationTestBase* Test, UObject* Object, const FString& Role)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        if (!Test->TestNotNull(FString::Printf(TEXT("%s is a live object"), *Role), Object))
        {
            Result->SetBoolField(TEXT("ok"), false);
            Result->SetStringField(TEXT("error"), TEXT("required_object_missing"));
            return Result;
        }

        const FString Text = UNpcAuditLibrary::DumpObject(Object, false);
        TSharedPtr<FJsonObject> Parsed;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
        if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
        {
            Test->AddError(FString::Printf(TEXT("%s reflection returned invalid JSON"), *Role));
            Result->SetBoolField(TEXT("ok"), false);
            Result->SetStringField(TEXT("error"), TEXT("reflection_json_invalid"));
            return Result;
        }

        bool bOk = false;
        Test->TestTrue(FString::Printf(TEXT("%s reflection succeeded"), *Role),
            Parsed->TryGetBoolField(TEXT("ok"), bOk) && bOk);
        return Parsed.ToSharedRef();
    }

    static void WriteReport(FAutomationTestBase* Test, const TSharedRef<FJsonObject>& Report,
        const TCHAR* DefaultFile = TEXT("world-inventory.json"))
    {
        Report->SetBoolField(TEXT("assertions_passed"), !Test->HasAnyErrors());
        const FString Path = ReportPath(DefaultFile);
        const FString Directory = FPaths::GetPath(Path);
        if (!IFileManager::Get().MakeDirectory(*Directory, true))
        {
            Test->AddError(FString::Printf(TEXT("Cannot create NPC audit directory: %s"), *Directory));
            return;
        }

        FString Json;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
        if (!FJsonSerializer::Serialize(Report, Writer))
        {
            Test->AddError(TEXT("Cannot serialize the actual-world audit report"));
            return;
        }

        const bool bWritten = FFileHelper::SaveStringToFile(
            Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        Test->TestTrue(TEXT("Actual PIE world report was written"), bWritten);
        if (bWritten)
        {
            Test->AddInfo(FString::Printf(TEXT("NPC audit report: %s"), *Path));
        }
    }

    static FString EncodeValue(const TSharedPtr<FJsonValue>& Value)
    {
        FString Text;
        // UE 5.4's serializer expects an object or array at the root. Serialize
        // one array element, then remove only its enclosing brackets so scalar
        // fixture values retain their exact JSON type.
        TArray<TSharedPtr<FJsonValue>> Wrapped;
        Wrapped.Add(Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
        if (!FJsonSerializer::Serialize(Wrapped, Writer)
            || !Text.StartsWith(TEXT("[")) || !Text.EndsWith(TEXT("]")))
        {
            return FString();
        }
        return Text.Mid(1, Text.Len() - 2);
    }

    /** Assertions consume actual reflected results, never a replacement rule engine. */
    class FContractAudit
    {
    public:
        explicit FContractAudit(FAutomationTestBase* InTest) : Test(InTest) {}

        TSharedPtr<FJsonObject> Checked(const FString& Text, const FString& Operation)
        {
            TSharedPtr<FJsonObject> Result;
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
            bool bOk = false;
            if (!FJsonSerializer::Deserialize(Reader, Result) || !Result.IsValid()
                || !Result->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
            {
                Test->AddError(Operation + TEXT(" failed: ") + Text.Left(800));
                return nullptr;
            }
            return Result;
        }

        TSharedPtr<FJsonValue> Read(UObject* Object, const FString& Name)
        {
            const TSharedPtr<FJsonObject> Result = Checked(
                UNpcAuditLibrary::ReadProperty(Object, Name), TEXT("Read ") + Name);
            const TSharedPtr<FJsonObject>* Property = nullptr;
            if (!Result.IsValid() || !Result->TryGetObjectField(TEXT("property"), Property))
            {
                return nullptr;
            }
            const TSharedPtr<FJsonValue>* Value = (*Property)->Values.Find(TEXT("value"));
            if (!Value)
            {
                Test->AddError(TEXT("Read lacks a property value: ") + Name);
                return nullptr;
            }
            return *Value;
        }

        bool Write(UObject* Object, const FString& Name, const TSharedPtr<FJsonValue>& Value)
        {
            return Value.IsValid() && Checked(UNpcAuditLibrary::WriteProperty(
                Object, Name, EncodeValue(Value)), TEXT("Write ") + Name).IsValid();
        }

        bool Number(UObject* Object, const FString& Name, double Value)
        {
            return Write(Object, Name, MakeShared<FJsonValueNumber>(Value));
        }

        bool Boolean(UObject* Object, const FString& Name, bool Value)
        {
            return Write(Object, Name, MakeShared<FJsonValueBoolean>(Value));
        }

        TSharedPtr<FJsonObject> Call(UObject* Object, const FString& Function,
            const FString& Arguments = TEXT("{}"))
        {
            return Checked(UNpcAuditLibrary::Invoke(Object, Function, Arguments), TEXT("Invoke ") + Function);
        }

        UObject* ObjectValue(const TSharedPtr<FJsonValue>& Value, const FString& Role)
        {
            UObject* Result = Value.IsValid() && Value->Type == EJson::String
                ? FindObject<UObject>(nullptr, *Value->AsString()) : nullptr;
            Test->TestNotNull(Role, Result);
            return Result;
        }

        UObject* Blackboard(APawn* Npc)
        {
            return ObjectValue(Read(Npc ? Npc->GetController() : nullptr, TEXT("Blackboard")),
                TEXT("Actual NPC Blackboard"));
        }

        TSharedPtr<FJsonValue> ReturnValue(const TSharedPtr<FJsonObject>& Result)
        {
            const TSharedPtr<FJsonObject>* Parameters = nullptr;
            if (Result.IsValid() && Result->TryGetObjectField(TEXT("parameters"), Parameters))
            {
                const TSharedPtr<FJsonValue>* Value = (*Parameters)->Values.Find(TEXT("ReturnValue"));
                if (Value)
                {
                    return *Value;
                }
            }
            Test->AddError(TEXT("Actual reflected function did not return ReturnValue"));
            return nullptr;
        }

        TSharedPtr<FJsonValue> BlackboardValue(UObject* Board, const TCHAR* Key)
        {
            return ReturnValue(Call(Board, TEXT("GetValueAsBool"),
                FString::Printf(TEXT("{\"KeyName\":\"%s\"}"), Key)));
        }

        void SetBlackboard(UObject* Board, const TCHAR* Key, bool Value)
        {
            Call(Board, TEXT("SetValueAsBool"), FString::Printf(
                TEXT("{\"KeyName\":\"%s\",\"BoolValue\":%s}"), Key, Value ? TEXT("true") : TEXT("false")));
        }

        void Check(const FString& Id, const TSharedPtr<FJsonValue>& Expected,
            const TSharedPtr<FJsonValue>& Actual, bool bMatches, const FString& Reference)
        {
            TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("id"), Id);
            Row->SetStringField(TEXT("oracle"), Reference);
            Row->SetField(TEXT("expected"), Expected.IsValid() ? Expected : MakeShared<FJsonValueNull>());
            Row->SetField(TEXT("actual"), Actual.IsValid() ? Actual : MakeShared<FJsonValueNull>());
            Row->SetBoolField(TEXT("passed"), bMatches);
            Cases.Add(MakeShared<FJsonValueObject>(Row));
            Test->TestTrue(Id + TEXT(" expected ") + EncodeValue(Expected)
                + TEXT(" actual ") + EncodeValue(Actual.IsValid() ? Actual : MakeShared<FJsonValueNull>()), bMatches);
        }

        void CheckNumber(const FString& Id, double Expected,
            const TSharedPtr<FJsonValue>& Actual, const FString& Reference)
        {
            Check(Id, MakeShared<FJsonValueNumber>(Expected), Actual,
                Actual.IsValid() && Actual->Type == EJson::Number
                && FMath::IsNearlyEqual(Actual->AsNumber(), Expected, 0.000001), Reference);
        }

        void CheckBool(const FString& Id, bool Expected,
            const TSharedPtr<FJsonValue>& Actual, const FString& Reference)
        {
            Check(Id, MakeShared<FJsonValueBoolean>(Expected), Actual,
                Actual.IsValid() && Actual->Type == EJson::Boolean && Actual->AsBool() == Expected, Reference);
        }

        bool Interact(APawn* Npc, const TCHAR* Key)
        {
            return Call(Npc, TEXT("Interact"), FString::Printf(
                TEXT("{\"InteractionKey\":\"%s\"}"), Key)).IsValid();
        }

        UObject* Dialogue(APawn* Npc)
        {
            return ObjectValue(Read(Npc, TEXT("Dialogue_Widget_Ref")), TEXT("Actual dialogue widget"));
        }

        TSharedPtr<FJsonValue> DialogueText(APawn* Npc)
        {
            UObject* Widget = Dialogue(Npc);
            UObject* TextBlock = Widget ? ObjectValue(Read(Widget, TEXT("Text_Content")),
                TEXT("Actual dialogue Text_Content widget")) : nullptr;
            return TextBlock ? ReturnValue(Call(TextBlock, TEXT("GetText"))) : nullptr;
        }

        void CheckContext(const FString& Id, APawn* Npc, const TCHAR* Context)
        {
            const TSharedPtr<FJsonValue> Text = DialogueText(Npc);
            const TSharedPtr<FJsonValue> Map = Read(Npc, TEXT("My_Dialogue_Map"));
            bool bMatches = false;
            if (Text.IsValid() && Text->Type == EJson::String
                && Map.IsValid() && Map->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject>* LinesObject = nullptr;
                if (Map->AsObject()->TryGetObjectField(Context, LinesObject))
                {
                    const TArray<TSharedPtr<FJsonValue>>* Lines = nullptr;
                    if ((*LinesObject)->TryGetArrayField(TEXT("Lines"), Lines))
                    {
                        for (const TSharedPtr<FJsonValue>& Line : *Lines)
                        {
                            bMatches |= Line.IsValid() && Line->Type == EJson::String
                                && Line->AsString() == Text->AsString();
                        }
                    }
                }
            }
            Check(Id, MakeShared<FJsonValueString>(FString(TEXT("Actual NPC dialogue map context: ")) + Context),
                Text, bMatches, TEXT("Original report PDF44-45/60, actual DataTable lines as independent output oracle"));
        }

        FAutomationTestBase* Test;
        TArray<TSharedPtr<FJsonValue>> Cases;
    };

    /** Restore fixture fields even when an assertion fails; no asset/package save. */
    class FScopedProperties
    {
    public:
        FScopedProperties(FContractAudit& InAudit, UObject* InObject,
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
                Audit.Write(Object, Values[Index].Key, Values[Index].Value);
            }
        }
    private:
        FContractAudit& Audit;
        UObject* Object;
        TArray<TPair<FString, TSharedPtr<FJsonValue>>> Values;
    };

    class FScopedInteraction
    {
    public:
        FScopedInteraction(FContractAudit& InAudit, APawn* InPlayer, APawn* InNpc)
            : Audit(InAudit), Player(InPlayer), Npc(InNpc),
              PlayerState(Audit, Player, {TEXT("PlayerIsInteracting"), TEXT("Current_Stamina"), TEXT("Money"), TEXT("Inventory")}),
              NpcState(Audit, Npc, {TEXT("Trust_Level"), TEXT("IsInteracting"), TEXT("IsFollowing"),
                  TEXT("InteractionEnd"), TEXT("Dialogue_Widget_Ref"), TEXT("Shop_Widget_Ref"),
                  TEXT("QuestStatus"), TEXT("ActiveQuest"), TEXT("CheckQuestCondition"),
                  TEXT("Target_Index"), TEXT("Target_Quantity")})
        {
            bWasHidden = Npc->IsHidden();
            bHadCollision = Npc->GetActorEnableCollision();
            Board = Audit.Blackboard(Npc);
            for (const TCHAR* Key : {TEXT("IsFollowing"), TEXT("IsInteracting"), TEXT("IsAtHome")})
            {
                const TSharedPtr<FJsonValue> Value = Audit.BlackboardValue(Board, Key);
                if (Value.IsValid() && Value->Type == EJson::Boolean)
                {
                    BoardValues.Emplace(Key, Value->AsBool());
                }
            }
            Audit.Boolean(Player, TEXT("PlayerIsInteracting"), false);
            Audit.Boolean(Npc, TEXT("IsInteracting"), false);
            Audit.Boolean(Npc, TEXT("InteractionEnd"), false);
            Audit.Boolean(Npc, TEXT("IsFollowing"), false);
            Audit.Boolean(Npc, TEXT("QuestStatus"), false);
            Audit.SetBlackboard(Board, TEXT("IsFollowing"), false);
            Audit.SetBlackboard(Board, TEXT("IsInteracting"), false);
            Audit.Call(Npc, TEXT("ReceiveTick"), TEXT("{\"DeltaSeconds\":0.0}"));
        }
        bool Open()
        {
            return Audit.Interact(Npc, TEXT("Interact")) && Audit.Dialogue(Npc);
        }
        ~FScopedInteraction()
        {
            const TSharedPtr<FJsonValue> Active = Audit.Read(Npc, TEXT("IsInteracting"));
            if (Active.IsValid() && Active->Type == EJson::Boolean && Active->AsBool())
            {
                Audit.Interact(Npc, TEXT("Interact"));
            }
            for (const TPair<FString, bool>& Value : BoardValues)
            {
                Audit.SetBlackboard(Board, *Value.Key, Value.Value);
            }
            Npc->SetActorHiddenInGame(bWasHidden);
            Npc->SetActorEnableCollision(bHadCollision);
        }
        UObject* Board = nullptr;
    private:
        FContractAudit& Audit;
        APawn* Player;
        APawn* Npc;
        FScopedProperties PlayerState;
        FScopedProperties NpcState;
        TArray<TPair<FString, bool>> BoardValues;
        bool bWasHidden = false;
        bool bHadCollision = true;
    };

    static void RunOriginalContracts(FAutomationTestBase* Test, UWorld* World)
    {
        FContractAudit Audit(Test);
        TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
        Report->SetStringField(TEXT("schema"), TEXT("npc-original-gameplay-contracts/v1"));
        Report->SetStringField(TEXT("test"), TEXT("NPC.Gameplay.OriginalContracts"));
        Report->SetStringField(TEXT("scope"), TEXT("Actual PIE actors and original Blueprint ProcessEvent calls. In-memory fixture controls are not proof of keyboard, navigation completion, or user-study behavior."));
        Report->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
        Report->SetStringField(TEXT("map"), UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()));
        Report->SetStringField(TEXT("isolation"), TEXT("Per-case reflected property restoration; actor ticks disabled during synchronous probes; fresh PIE ends after suite. No source assets are saved."));
        Report->SetStringField(TEXT("rng_scope"), TEXT("Dialogue probes set explicit global Rand/SRand seeds in the isolated editor test process; they do not claim statistical accuracy."));

        APlayerController* Controller = World->GetFirstPlayerController();
        APawn* Player = Controller ? Controller->GetPawn() : nullptr;
        AGameModeBase* GameMode = World->GetAuthGameMode();
        UClass* NpcClass = LoadClass<APawn>(nullptr,
            TEXT("/Game/ThirdPerson/Blueprints/BP_NPC_Standard.BP_NPC_Standard_C"));
        Test->TestNotNull(TEXT("Contracts player pawn"), Player);
        Test->TestNotNull(TEXT("Contracts GameMode"), GameMode);
        Test->TestNotNull(TEXT("Contracts NPC class"), NpcClass);
        if (!Player || !GameMode || !NpcClass)
        {
            Report->SetStringField(TEXT("status"), TEXT("setup_failed"));
            WriteReport(Test, Report, TEXT("original-contracts.json"));
            return;
        }

        TMap<FString, APawn*> Named;
        TArray<TPair<AActor*, bool>> TickStates;
        TickStates.Emplace(Player, Player->IsActorTickEnabled());
        TickStates.Emplace(GameMode, GameMode->IsActorTickEnabled());
        for (TActorIterator<APawn> It(World, NpcClass); It; ++It)
        {
            APawn* Npc = *It;
            const TSharedPtr<FJsonValue> Data = Audit.Read(Npc, TEXT("My_NPC_Data"));
            FString Name;
            if (Data.IsValid() && Data->Type == EJson::Object
                && Data->AsObject()->TryGetStringField(TEXT("NPC_Name"), Name))
            {
                Named.Add(Name, Npc);
            }
            TickStates.Emplace(Npc, Npc->IsActorTickEnabled());
        }
        Test->TestEqual(TEXT("Five named runtime NPCs initialized from DataTable"), Named.Num(), 5);
        struct FNpcContract
        {
            const TCHAR* Name;
            const TCHAR* Row;
            double FollowThreshold;
            double Start;
            double End;
        };
        // Fixed expected data from the original editor's DT_NPC_Data export.
        // Each actor is bound to its authored row before the behavior probes.
        const FNpcContract NpcContracts[] = {
            {TEXT("Charles"), TEXT("NPC_01"), 80.0, 420.0, 1140.0},
            {TEXT("George"), TEXT("NPC_02"), 40.0, 330.0, 1050.0},
            {TEXT("Leon"), TEXT("NPC_03"), 60.0, 390.0, 1110.0},
            {TEXT("Lucia"), TEXT("NPC_04"), 60.0, 0.0, 720.0},
            {TEXT("Rose"), TEXT("NPC_05"), 20.0, 420.0, 1140.0}};
        TArray<TSharedPtr<FJsonValue>> DataOracles;
        for (const FNpcContract& Contract : NpcContracts)
        {
            TSharedRef<FJsonObject> Oracle = MakeShared<FJsonObject>();
            Oracle->SetStringField(TEXT("asset"), TEXT("/Game/AI/DT_NPC_Data"));
            Oracle->SetStringField(TEXT("row"), Contract.Row);
            Oracle->SetStringField(TEXT("npc"), Contract.Name);
            Oracle->SetNumberField(TEXT("required_trust_follow"), Contract.FollowThreshold);
            Oracle->SetNumberField(TEXT("start_time"), Contract.Start);
            Oracle->SetNumberField(TEXT("end_time"), Contract.End);
            Oracle->SetStringField(TEXT("source"), TEXT("Frozen original editor DataTable export; report PDF32/46 supplies interval and inclusive follow rules."));
            DataOracles.Add(MakeShared<FJsonValueObject>(Oracle));
            APawn** Found = Named.Find(Contract.Name);
            if (!Found)
            {
                Test->AddError(FString(TEXT("Required original NPC missing: ")) + Contract.Name);
                continue;
            }
            const TSharedPtr<FJsonValue> Row = Audit.Read(*Found, TEXT("NPC_Row_Name"));
            Test->TestTrue(FString(Contract.Name) + TEXT(" is bound to its original DataTable row"),
                Row.IsValid() && Row->Type == EJson::String && Row->AsString() == Contract.Row);
            const TSharedPtr<FJsonValue> Data = Audit.Read(*Found, TEXT("My_NPC_Data"));
            if (Data.IsValid() && Data->Type == EJson::Object)
            {
                for (const TPair<FString, double>& Expected : {
                    TPair<FString, double>(TEXT("Required_Trust_Follow"), Contract.FollowThreshold),
                    TPair<FString, double>(TEXT("Start_Time"), Contract.Start),
                    TPair<FString, double>(TEXT("End_Time"), Contract.End)})
                {
                    double Actual = 0.0;
                    Test->TestTrue(FString(Contract.Name) + TEXT(" original row field ") + Expected.Key,
                        Data->AsObject()->TryGetNumberField(Expected.Key, Actual)
                        && FMath::IsNearlyEqual(Actual, Expected.Value, 0.000001));
                }
            }
            else
            {
                Test->AddError(FString(Contract.Name) + TEXT(" lacks initialized My_NPC_Data"));
            }
        }
        Report->SetArrayField(TEXT("npc_data_oracles"), DataOracles);
        for (const TPair<AActor*, bool>& State : TickStates)
        {
            State.Key->SetActorTickEnabled(false);
        }

        {
            FScopedProperties Global(Audit, GameMode, {TEXT("CurrentTime"), TEXT("TimeSpeed"), TEXT("ActiveBGM")});
            Audit.Number(GameMode, TEXT("TimeSpeed"), 0.0);

            for (const TTuple<double, double, double>& Example : {
                MakeTuple(1439.5, 0.5, 0.0), MakeTuple(1439.5, 1.0, 0.5),
                MakeTuple(0.0, 60.0, 60.0), MakeTuple(360.0, 120.0, 480.0)})
            {
                FScopedProperties State(Audit, GameMode, {TEXT("CurrentTime"), TEXT("TimeSpeed")});
                Audit.Number(GameMode, TEXT("CurrentTime"), Example.Get<0>());
                Audit.Number(GameMode, TEXT("TimeSpeed"), 1.0);
                Audit.Call(GameMode, TEXT("ReceiveTick"), FString::Printf(
                    TEXT("{\"DeltaSeconds\":%.9f}"), Example.Get<1>()));
                Audit.CheckNumber(FString::Printf(TEXT("clock.%.3f_plus_%.3f"), Example.Get<0>(), Example.Get<1>()),
                    Example.Get<2>(), Audit.Read(GameMode, TEXT("CurrentTime")), TEXT("Report PDF28 modulo formula, actual ReceiveTick"));
            }

            UObject* Movement = Audit.ObjectValue(Audit.Read(Player, TEXT("CharacterMovement")), TEXT("Actual player CharacterMovement"));
            if (Movement)
            {
                for (double Stamina : {0.0, 19.0, 20.0, 21.0, 30.0})
                {
                    FScopedProperties State(Audit, Player, {TEXT("Current_Stamina"), TEXT("Base_Speed")});
                    FScopedProperties MovementState(Audit, Movement, {TEXT("MaxWalkSpeed")});
                    Audit.Number(Player, TEXT("Current_Stamina"), Stamina);
                    Audit.Number(Player, TEXT("Base_Speed"), 300.0);
                    Audit.Call(Player, TEXT("UpdateMovementSpeedByStamina"));
                    const double Expected = Stamina < 20.0 ? 200.0 : 300.0;
                    Audit.CheckNumber(FString::Printf(TEXT("movement.stamina_%.0f"), Stamina), Expected,
                        Audit.Read(Movement, TEXT("MaxWalkSpeed")), TEXT("Report PDF34/56; fixed expected boundary values"));
                }
                FScopedProperties State(Audit, Player, {TEXT("Current_Stamina"), TEXT("Base_Speed")});
                FScopedProperties MovementState(Audit, Movement, {TEXT("MaxWalkSpeed")});
                Audit.Number(Player, TEXT("Base_Speed"), 300.0);
                Audit.Number(Player, TEXT("Current_Stamina"), 19.0);
                Audit.Call(Player, TEXT("UpdateMovementSpeedByStamina"));
                Audit.Number(Player, TEXT("Current_Stamina"), 20.0);
                Audit.Call(Player, TEXT("UpdateMovementSpeedByStamina"));
                Audit.CheckNumber(TEXT("movement.recovers_at_20"), 300.0,
                    Audit.Read(Movement, TEXT("MaxWalkSpeed")), TEXT("Report PDF34 false branch restores Base_Speed"));
            }

            for (const TTuple<double, double, double, double>& Example : {
                MakeTuple(3.0, 100.0, 10.0, 0.0), MakeTuple(100.0, 100.0, 10.0, 90.0),
                MakeTuple(109.0, 110.0, 1.0, 108.0), MakeTuple(0.0, 100.0, 10.0, 0.0)})
            {
                FScopedProperties State(Audit, Player, {TEXT("Current_Stamina"), TEXT("Max_Stamina"), TEXT("Reduce_Stamina")});
                FScopedProperties MovementState(Audit, Movement, {TEXT("MaxWalkSpeed")});
                Audit.Number(Player, TEXT("Current_Stamina"), Example.Get<0>());
                Audit.Number(Player, TEXT("Max_Stamina"), Example.Get<1>());
                Audit.Number(Player, TEXT("Reduce_Stamina"), Example.Get<2>());
                Audit.Call(Player, TEXT("DecreaseStaminaOverTime"));
                Audit.CheckNumber(FString::Printf(TEXT("decay.current_%.0f_max_%.0f"), Example.Get<0>(), Example.Get<1>()),
                    Example.Get<3>(), Audit.Read(Player, TEXT("Current_Stamina")), TEXT("Report PDF34 Figure12 dynamic MaxStamina clamp"));
            }

            Audit.Number(GameMode, TEXT("CurrentTime"), 600.0);
            for (const FNpcContract& Contract : NpcContracts)
            {
                APawn** Found = Named.Find(Contract.Name);
                if (!Found)
                {
                    continue;
                }
                APawn* Npc = *Found;
                const FString Reference = FString::Printf(TEXT("Original /Game/AI/DT_NPC_Data row %s: Required_Trust_Follow=%.0f; report PDF46 inclusive >= rule"),
                    Contract.Row, Contract.FollowThreshold);
                for (double Trust : {Contract.FollowThreshold - 1.0, Contract.FollowThreshold, Contract.FollowThreshold + 1.0})
                {
                    FScopedInteraction Fixture(Audit, Player, Npc);
                    Audit.Number(Npc, TEXT("Trust_Level"), Trust);
                    if (Fixture.Open())
                    {
                        Audit.Interact(Npc, TEXT("Follow"));
                        const bool Expected = Trust >= Contract.FollowThreshold;
                        const FString Id = FString::Printf(TEXT("follow.%s.trust_%.0f"), Contract.Name, Trust);
                        Audit.CheckBool(Id, Expected, Audit.Read(Npc, TEXT("IsFollowing")), Reference);
                        Audit.CheckBool(Id + TEXT(".blackboard"), Expected,
                            Audit.BlackboardValue(Fixture.Board, TEXT("IsFollowing")), TEXT("Actual BehaviorTree Blackboard agrees with original follow branch"));
                    }
                }
                {
                    FScopedInteraction Fixture(Audit, Player, Npc);
                    Audit.Number(Npc, TEXT("Trust_Level"), Contract.FollowThreshold);
                    if (Fixture.Open())
                    {
                        Audit.Interact(Npc, TEXT("Follow"));
                        // Use the actual close/reopen flow, not an artificial reset of its input gate.
                        Audit.Interact(Npc, TEXT("Interact"));
                        Audit.Interact(Npc, TEXT("Interact"));
                        Audit.Interact(Npc, TEXT("Follow"));
                        Audit.CheckBool(FString(TEXT("follow.")) + Contract.Name + TEXT(".toggle_off"), false,
                            Audit.Read(Npc, TEXT("IsFollowing")), TEXT("Existing Follow input toggles an already-following NPC off"));
                    }
                }
            }

            // These synchronous checks establish schedule intent only. The home
            // movement request and actual arrival require a separate latent test.
            for (const FNpcContract& Contract : NpcContracts)
            {
                APawn** Found = Named.Find(Contract.Name);
                if (!Found)
                {
                    continue;
                }
                const double BeforeStart = Contract.Start > 0.0 ? Contract.Start - 0.001 : 1439.999;
                const FString Reference = FString::Printf(TEXT("Original /Game/AI/DT_NPC_Data row %s: active %.0f<=t<%.0f; report PDF32; IsAtHome is home intent, not completed navigation"),
                    Contract.Row, Contract.Start, Contract.End);
                for (const TPair<double, bool>& Boundary : {
                    TPair<double, bool>(BeforeStart, true), TPair<double, bool>(Contract.Start, false),
                    TPair<double, bool>(Contract.End - 0.001, false), TPair<double, bool>(Contract.End, true)})
                {
                    FScopedInteraction Fixture(Audit, Player, *Found);
                    FScopedProperties Clock(Audit, GameMode, {TEXT("CurrentTime")});
                    Audit.Number(GameMode, TEXT("CurrentTime"), Boundary.Key);
                    Audit.Call(*Found, TEXT("ReceiveTick"), TEXT("{\"DeltaSeconds\":0.0}"));
                    Audit.CheckBool(FString::Printf(TEXT("schedule.%s.time_%.3f"), Contract.Name, Boundary.Key), Boundary.Value,
                        Audit.BlackboardValue(Fixture.Board, TEXT("IsAtHome")), Reference);
                }
            }

            APawn** Charles = Named.Find(TEXT("Charles"));
            if (Charles)
            {
                // Full original Talk path: seed controls its quest gate rather than bypassing it.
                for (int32 Seed : {378, 17, 42, 123})
                {
                    for (const TPair<double, double>& State : {
                        TPair<double, double>(30.0, 0.0), TPair<double, double>(31.0, 0.0)})
                    {
                        FScopedInteraction Fixture(Audit, Player, *Charles);
                        Audit.Number(Player, TEXT("Current_Stamina"), State.Key);
                        Audit.Number(Player, TEXT("Money"), State.Value);
                        if (Fixture.Open())
                        {
                            FMath::RandInit(Seed);
                            FMath::SRandInit(Seed);
                            Audit.Interact(*Charles, TEXT("Talk"));
                            Audit.CheckContext(FString::Printf(TEXT("dialogue.stamina_%.0f.money_0.seed_%d"), State.Key, Seed),
                                *Charles, State.Key <= 30.0 ? TEXT("Stamina") : TEXT("Money"));
                        }
                    }
                }

                {
                    FScopedInteraction Fixture(Audit, Player, *Charles);
                    FScopedProperties Clock(Audit, GameMode, {TEXT("CurrentTime")});
                    Audit.Number(GameMode, TEXT("CurrentTime"), 1139.0);
                    if (Fixture.Open())
                    {
                        Audit.Number(GameMode, TEXT("CurrentTime"), 1140.0);
                        Audit.Call(*Charles, TEXT("ReceiveTick"), TEXT("{\"DeltaSeconds\":0.0}"));
                        Audit.CheckBool(TEXT("schedule.active_dialogue_blocks_home_intent"), false,
                            Audit.BlackboardValue(Fixture.Board, TEXT("IsAtHome")), TEXT("Report PDF61 claims active interaction prevents scheduled departure"));
                    }
                }
            }
        }

        for (const TPair<AActor*, bool>& State : TickStates)
        {
            State.Key->SetActorTickEnabled(State.Value);
        }
        int32 Passed = 0;
        for (const TSharedPtr<FJsonValue>& Row : Audit.Cases)
        {
            Passed += Row->AsObject()->GetBoolField(TEXT("passed")) ? 1 : 0;
        }
        Report->SetArrayField(TEXT("cases"), Audit.Cases);
        Report->SetNumberField(TEXT("case_count"), Audit.Cases.Num());
        Report->SetNumberField(TEXT("passed_cases"), Passed);
        Report->SetNumberField(TEXT("failed_cases"), Audit.Cases.Num() - Passed);
        Report->SetStringField(TEXT("status"), Test->HasAnyErrors() ? TEXT("failed") : TEXT("passed"));
        WriteReport(Test, Report, TEXT("original-contracts.json"));
    }

    /** Wait for the editor's real PIE world. Never creates or ticks a substitute world. */
    class FWaitAndRecordWorld final : public IAutomationLatentCommand
    {
    public:
        explicit FWaitAndRecordWorld(FAutomationTestBase* InTest, bool bInRunContracts = false)
            : Test(InTest), bRunContracts(bInRunContracts)
        {
        }

        virtual bool Update() override
        {
            const double Now = FPlatformTime::Seconds();
            if (StartedAt < 0.0)
            {
                StartedAt = Now;
            }
            if (Now - StartedAt >= WorldWaitLimitSeconds)
            {
                Fail(TEXT("PIE did not begin and advance two game seconds within 30 real seconds"), Now);
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
                else if (ObservedWorld.Get() != World)
                {
                    Fail(TEXT("PIE world changed while waiting for actual gameplay ticks"), Now);
                    return true;
                }

                const double GameTimeAdvanced = World->GetTimeSeconds() - InitialGameTime;
                if (GameTimeAdvanced >= RequiredGameTimeSeconds)
                {
                    if (bRunContracts)
                    {
                        RunOriginalContracts(Test, World);
                    }
                    else
                    {
                        Record(World, Now, GameTimeAdvanced);
                    }
                    return true;
                }
            }

            return false;
        }

    private:
        TSharedRef<FJsonObject> BaseReport(double Now) const
        {
            TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
            Report->SetStringField(TEXT("schema"), TEXT("npc-gameplay-world-inventory/v1"));
            Report->SetStringField(TEXT("test"), TEXT("NPC.Gameplay.WorldInventory"));
            Report->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
            Report->SetStringField(TEXT("requested_map"), TestMap);
            Report->SetStringField(TEXT("scope"),
                TEXT("Actual editor PIE world after BeginPlay and two seconds of world ticks; no substitute game simulation."));
            Report->SetNumberField(TEXT("real_wait_seconds"), Now - StartedAt);
            Report->SetNumberField(TEXT("required_game_seconds"), RequiredGameTimeSeconds);
            Report->SetNumberField(TEXT("wait_limit_seconds"), WorldWaitLimitSeconds);
            return Report;
        }

        void Fail(const FString& Reason, double Now)
        {
            Test->AddError(Reason);
            TSharedRef<FJsonObject> Report = BaseReport(Now);
            Report->SetStringField(TEXT("status"), TEXT("failed"));
            Report->SetStringField(TEXT("error"), Reason);
            WriteReport(Test, Report);
        }

        void Record(UWorld* World, double Now, double GameTimeAdvanced)
        {
            TSharedRef<FJsonObject> Report = BaseReport(Now);
            const FString MapPackage = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
            Test->TestEqual(TEXT("The requested original map is running"), MapPackage, FString(TestMap));
            Test->TestTrue(TEXT("The actual PIE world has begun play"), World->HasBegunPlay());
            Test->TestTrue(TEXT("The actual PIE world advanced at least two game seconds"),
                GameTimeAdvanced >= RequiredGameTimeSeconds);
            Report->SetStringField(TEXT("world"), World->GetPathName());
            Report->SetStringField(TEXT("map_package"), MapPackage);
            Report->SetBoolField(TEXT("has_begun_play"), World->HasBegunPlay());
            Report->SetNumberField(TEXT("world_time_seconds"), World->GetTimeSeconds());
            Report->SetNumberField(TEXT("world_time_advanced_seconds"), GameTimeAdvanced);

            APlayerController* Controller = World->GetFirstPlayerController();
            APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
            AGameModeBase* GameMode = World->GetAuthGameMode();
            UClass* NpcClass = LoadClass<AActor>(nullptr,
                TEXT("/Game/ThirdPerson/Blueprints/BP_NPC_Standard.BP_NPC_Standard_C"));
            UClass* GameModeClass = LoadClass<AGameModeBase>(nullptr,
                TEXT("/Game/AI/BP_GameMode.BP_GameMode_C"));
            UClass* PlayerClass = LoadClass<APawn>(nullptr,
                TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter_C"));

            Test->TestNotNull(TEXT("Original NPC Blueprint class loads"), NpcClass);
            Test->TestNotNull(TEXT("Original GameMode Blueprint class loads"), GameModeClass);
            Test->TestNotNull(TEXT("Original player Blueprint class loads"), PlayerClass);
            Test->TestTrue(TEXT("The live authority GameMode is BP_GameMode"),
                GameMode && GameModeClass && GameMode->IsA(GameModeClass));
            Test->TestTrue(TEXT("The possessed live pawn is BP_ThirdPersonCharacter"),
                Pawn && PlayerClass && Pawn->IsA(PlayerClass));
            if (Pawn)
            {
                Test->TestTrue(TEXT("The player pawn completed BeginPlay"), Pawn->HasActorBegunPlay());
            }

            Report->SetObjectField(TEXT("player_controller"), Dump(Test, Controller, TEXT("PlayerController")));
            Report->SetObjectField(TEXT("player_pawn"), Dump(Test, Pawn, TEXT("PlayerPawn")));
            Report->SetObjectField(TEXT("game_mode"), Dump(Test, GameMode, TEXT("GameMode")));

            TArray<TSharedPtr<FJsonValue>> Npcs;
            if (NpcClass)
            {
                for (TActorIterator<AActor> It(World, NpcClass); It; ++It)
                {
                    AActor* Npc = *It;
                    Test->TestTrue(FString::Printf(TEXT("NPC %s completed BeginPlay"), *Npc->GetName()),
                        Npc->HasActorBegunPlay());
                    TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
                    Entry->SetObjectField(TEXT("actor"), Dump(Test, Npc, TEXT("NPC")));
                    if (APawn* NpcPawn = Cast<APawn>(Npc))
                    {
                        Entry->SetObjectField(TEXT("controller"),
                            Dump(Test, NpcPawn->GetController(), TEXT("NPCController")));
                    }
                    else
                    {
                        Test->AddError(FString::Printf(TEXT("Original NPC %s is not a pawn"), *Npc->GetName()));
                    }
                    Npcs.Add(MakeShared<FJsonValueObject>(Entry));
                }
            }
            Test->TestEqual(TEXT("The original map contains five live NPC Blueprint actors"), Npcs.Num(), 5);
            Report->SetNumberField(TEXT("npc_count"), Npcs.Num());
            Report->SetArrayField(TEXT("npcs"), Npcs);
            Report->SetStringField(TEXT("status"), Test->HasAnyErrors() ? TEXT("failed") : TEXT("passed"));
            WriteReport(Test, Report);
        }

        FAutomationTestBase* Test;
        TWeakObjectPtr<UWorld> ObservedWorld;
        double StartedAt = -1.0;
        double InitialGameTime = 0.0;
        bool bRunContracts = false;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNpcGameplayWorldInventory,
    "NPC.Gameplay.WorldInventory",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNpcGameplayWorldInventory::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("An Unreal Editor instance is available"), GEditor))
    {
        return false;
    }
    if (GEditor->PlayWorld)
    {
        AddError(TEXT("WorldInventory requires no pre-existing PIE session"));
        return false;
    }

    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(NpcGameplayAudit::TestMap));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(NpcGameplayAudit::FWaitAndRecordWorld(this));
    // Even a bounded wait/assertion failure returns to this cleanup command.
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNpcGameplayOriginalContracts,
    "NPC.Gameplay.OriginalContracts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNpcGameplayOriginalContracts::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("An Unreal Editor instance is available"), GEditor))
    {
        return false;
    }
    if (GEditor->PlayWorld)
    {
        AddError(TEXT("OriginalContracts requires no pre-existing PIE session"));
        return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(NpcGameplayAudit::TestMap));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(NpcGameplayAudit::FWaitAndRecordWorld(this, true));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
