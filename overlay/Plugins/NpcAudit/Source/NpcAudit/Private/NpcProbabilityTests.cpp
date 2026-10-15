#include "NpcAuditLibrary.h"
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace NpcQuestProbability
{
    class FSample final : public IAutomationLatentCommand
    {
    public:
        explicit FSample(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds()) {}
        virtual bool Update() override
        {
            if (FPlatformTime::Seconds() - Started > 45)
            { Test->AddError(TEXT("Actual quest probability PIE setup timed out")); return true; }
            UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
            if (!World || !World->HasBegunPlay() || World->GetTimeSeconds() < 2) return false;
            APlayerController* Controller = World->GetFirstPlayerController();
            APawn* Player = Controller ? Controller->GetPawn() : nullptr;
            UClass* Class = LoadClass<APawn>(nullptr, TEXT("/Game/ThirdPerson/Blueprints/BP_NPC_Standard.BP_NPC_Standard_C"));
            APawn* Npc = nullptr;
            if (Class) for (TActorIterator<APawn> It(World, Class); It; ++It)
                if (UNpcAuditLibrary::ReadProperty(*It, TEXT("NPC_Row_Name")).Contains(TEXT("NPC_01")))
                { Npc = *It; break; }
            if (!Test->TestNotNull(TEXT("Actual quest probability player"), Player)
                || !Test->TestNotNull(TEXT("Actual Charles quest actor"), Npc)) return true;
            auto Write = [this](UObject* Object, const TCHAR* Name, const TCHAR* Json)
            {
                TSharedPtr<FJsonObject> Result;
                bool bOk = false;
                FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(
                    UNpcAuditLibrary::WriteProperty(Object, Name, Json)), Result);
                return Test->TestTrue(FString(TEXT("Probability fixture write ")) + Name,
                    Result.IsValid() && Result->TryGetBoolField(TEXT("ok"), bOk) && bOk);
            };
            if (!Write(World->GetAuthGameMode(), TEXT("TimeSpeed"), TEXT("0"))
                || !Write(World->GetAuthGameMode(), TEXT("CurrentTime"), TEXT("600"))
                || !Write(Player, TEXT("Current_Stamina"), TEXT("100"))
                || !Write(Player, TEXT("Money"), TEXT("1000"))) return true;
            UNpcAuditLibrary::Invoke(Npc, TEXT("ReceiveTick"), TEXT("{\"DeltaSeconds\":0}"));
            UNpcAuditLibrary::Invoke(Npc, TEXT("Interact"), TEXT("{\"InteractionKey\":\"Interact\"}"));
            FBoolProperty* Quest = FindFProperty<FBoolProperty>(Class, TEXT("QuestStatus"));
            FBoolProperty* Gate = FindFProperty<FBoolProperty>(Class, TEXT("InteractionEnd"));
            FBoolProperty* Interacting = FindFProperty<FBoolProperty>(Class, TEXT("IsInteracting"));
            UFunction* Function = Class->FindFunctionByName(TEXT("Interact"));
            FNameProperty* Key = Function ? FindFProperty<FNameProperty>(Function, TEXT("InteractionKey")) : nullptr;
            if (!Quest || !Gate || !Interacting || !Function || !Key || !Interacting->GetPropertyValue_InContainer(Npc))
            { Test->AddError(TEXT("Actual open dialogue and original quest function fields are required")); return true; }
            FStructOnScope Arguments(Function);
            Key->SetPropertyValue_InContainer(Arguments.GetStructMemory(), FName(TEXT("Talk")));
            constexpr int32 Trials = 10000;
            FMath::RandInit(378);
            FMath::SRandInit(378);
            int32 Quests = 0;
            const double SampleStarted = FPlatformTime::Seconds();
            for (int32 Index = 0; Index < Trials; ++Index)
            {
                // An isolated new-conversation condition for each real Talk dispatch.
                Quest->SetPropertyValue_InContainer(Npc, false);
                Gate->SetPropertyValue_InContainer(Npc, false);
                Npc->ProcessEvent(Function, Arguments.GetStructMemory());
                Quests += Quest->GetPropertyValue_InContainer(Npc) ? 1 : 0;
            }
            const double Rate = static_cast<double>(Quests) / Trials;
            Test->TestTrue(TEXT("Actual conditional quest proposal rate is within predefined 7%-13% sanity bounds"), Rate >= .07 && Rate <= .13);
            TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
            Report->SetStringField(TEXT("schema"), TEXT("npc-actual-quest-probability/v1"));
            Report->SetStringField(TEXT("test"), TEXT("NPC.Gameplay.QuestProbability"));
            Report->SetStringField(TEXT("scope"), TEXT("Actual Charles Talk ProcessEvent with open real dialogue, stamina100, money1000, no active quest and InteractionEnd=false reset before each trial. Conditional proposal sampling, not 10000 full keyboard conversations or quest completions. Fresh PIE ends after this isolated synchronous loop; source assets are never saved."));
            Report->SetNumberField(TEXT("trials"), Trials);
            Report->SetNumberField(TEXT("quest_count"), Quests);
            Report->SetNumberField(TEXT("quest_rate"), Rate);
            Report->SetNumberField(TEXT("rng_seed"), 378);
            Report->SetNumberField(TEXT("configured_threshold"), .1);
            const double Z = 1.959963984540054;
            const double Denom = 1 + Z * Z / Trials;
            const double Center = (Rate + Z * Z / (2 * Trials)) / Denom;
            const double Half = Z * FMath::Sqrt(Rate * (1 - Rate) / Trials + Z * Z / (4.0 * Trials * Trials)) / Denom;
            Report->SetNumberField(TEXT("wilson_95_lower"), Center - Half);
            Report->SetNumberField(TEXT("wilson_95_upper"), Center + Half);
            Report->SetNumberField(TEXT("sampling_elapsed_seconds"), FPlatformTime::Seconds() - SampleStarted);
            Report->SetBoolField(TEXT("assertions_passed"), !Test->HasAnyErrors());
            FString Path;
            FParse::Value(FCommandLine::Get(), TEXT("NpcQuestProbabilityReport="), Path);
            if (Path.IsEmpty()) Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NpcAudit/quest-probability.json"));
            IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
            FString Json;
            FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json));
            Test->TestTrue(TEXT("Quest probability JSON report written"), FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
            return true;
        }
    private:
        FAutomationTestBase* Test;
        double Started;
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNpcQuestProbability, "NPC.Gameplay.QuestProbability",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNpcQuestProbability::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("Editor available"), GEditor) || GEditor->PlayWorld) return false;
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/ThirdPerson/Maps/ThirdPersonMap")));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(NpcQuestProbability::FSample(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}
#endif
