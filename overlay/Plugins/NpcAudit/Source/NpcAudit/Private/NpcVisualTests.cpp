#include "NpcAuditLibrary.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Editor.h"
#include "AIController.h"
#include "BrainComponent.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputCoreTypes.h"
#include "CoreGlobals.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HighResScreenshot.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Tests/AutomationEditorCommon.h"
#include "Slate/SceneViewport.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UnrealClient.h"

namespace NpcVisualAudit
{
    class FStartWindow final : public IAutomationLatentCommand
    {
    public:
        virtual bool Update() override
        {
            ULevelEditorPlaySettings* Settings = GetMutableDefault<ULevelEditorPlaySettings>();
            Settings->NewWindowWidth = 1280;
            Settings->NewWindowHeight = 720;
            FRequestPlaySessionParams Params;
            Params.bAllowOnlineSubsystem = false;
            // No destination editor viewport: screenshot only the actual PIE game window.
            GEditor->RequestPlaySession(Params);
            return true;
        }
    };
    class FCapture final : public IAutomationLatentCommand
    {
    public:
        explicit FCapture(FAutomationTestBase* InTest) : Test(InTest) {}
        virtual bool Update() override
        {
            const double Now = FPlatformTime::Seconds();
            if (Started < 0) { Started = Now; LastAction = Now; }
            if (Now - Started > 50)
            {
                Test->AddError(TEXT("Actual PIE screenshot sequence timed out"));
                return true;
            }
            UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
            if (!World || !World->HasBegunPlay() || World->GetTimeSeconds() < 2) return false;
            if (bReleaseCameraInput && GFrameCounter > CameraInputFrame)
            {
                World->GetFirstPlayerController()->InputKey(FInputKeyParams(EKeys::V, IE_Released, 0.0, false));
                bReleaseCameraInput = false;
            }
            if (Stage == 0)
            {
                FParse::Value(FCommandLine::Get(), TEXT("NpcScreenshotDir="), Directory);
                if (Directory.IsEmpty()) Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NpcAudit/Screenshots"));
                Directory = FPaths::ConvertRelativePathToFull(Directory);
                IFileManager::Get().MakeDirectory(*Directory, true);
                Player = World->GetFirstPlayerController()->GetPawn();
                if (World->GetGameViewport() && World->GetGameViewport()->GetGameViewport())
                    World->GetGameViewport()->GetGameViewport()->SetFixedViewportSize(1280, 720);
                UNpcAuditLibrary::WriteProperty(World->GetAuthGameMode(), TEXT("TimeSpeed"), TEXT("0"));
                UNpcAuditLibrary::WriteProperty(World->GetAuthGameMode(), TEXT("CurrentTime"), TEXT("600"));
                UClass* Class = LoadClass<APawn>(nullptr, TEXT("/Game/ThirdPerson/Blueprints/BP_NPC_Standard.BP_NPC_Standard_C"));
                for (TActorIterator<APawn> It(World, Class); It; ++It)
                {
                    if (UNpcAuditLibrary::ReadProperty(*It, TEXT("NPC_Row_Name")).Contains(TEXT("NPC_01")))
                    {
                        Npc = *It;
                        break;
                    }
                }
                if (!Player.IsValid() || !Npc.IsValid())
                {
                    Test->AddError(TEXT("Screenshot fixture requires actual player and Charles"));
                    return true;
                }
                if (AAIController* Ai = Cast<AAIController>(Npc->GetController()))
                {
                    Ai->StopMovement();
                    if (Ai->GetBrainComponent()) Ai->GetBrainComponent()->PauseLogic(TEXT("Screenshot fixture"));
                }
                const FVector At = Npc->GetActorLocation() + FVector(-260, 0, 0);
                Player->SetActorLocationAndRotation(At, FRotator::ZeroRotator, false, nullptr, ETeleportType::TeleportPhysics);
                World->GetFirstPlayerController()->SetControlRotation(FRotator(-8, 0, 0));
                // Use the implemented V first-person action, with no camera or gameplay asset edits.
                World->GetFirstPlayerController()->InputKey(FInputKeyParams(EKeys::V, IE_Pressed, 1.0, false));
                CameraInputFrame = GFrameCounter;
                bReleaseCameraInput = true;
                Stage = 1; LastAction = Now;
                return false;
            }
            if (Now - LastAction < (Stage == 1 ? 8.0 : 2.0)) return false;
            if (Stage == 1)
            {
                USpringArmComponent* Arm = Player->FindComponentByClass<USpringArmComponent>();
                Test->TestTrue(TEXT("Original V action selected first-person camera"),
                    Arm && FMath::IsNearlyZero(Arm->TargetArmLength) && FMath::IsNearlyEqual(Arm->SocketOffset.Z, 80.0));
                Request(TEXT("gameplay.png"));
                Stage = 2; LastAction = Now;
            }
            else if (Stage == 2 && Written())
            {
                UNpcAuditLibrary::Invoke(Player.Get(), TEXT("SendInteractionSignal"), TEXT("{\"KeyName\":\"Interact\"}"));
                Stage = 3; LastAction = Now;
            }
            else if (Stage == 3)
            {
                Request(TEXT("dialogue.png"));
                Stage = 4; LastAction = Now;
            }
            else if (Stage == 4 && Written())
            {
                UNpcAuditLibrary::Invoke(Npc.Get(), TEXT("Interact"), TEXT("{\"InteractionKey\":\"Trade\"}"));
                Stage = 5; LastAction = Now;
            }
            else if (Stage == 5)
            {
                Request(TEXT("trading.png"));
                Stage = 6; LastAction = Now;
            }
            else if (Stage == 6 && Written())
            {
                Test->AddInfo(TEXT("Actual rendered gameplay, dialogue and trading screenshots: ") + Directory);
                return true;
            }
            return false;
        }
    private:
        void Request(const TCHAR* Name)
        {
            Pending = FPaths::Combine(Directory, Name);
            if (IFileManager::Get().FileExists(*Pending))
            {
                Test->AddError(TEXT("Refusing to overwrite an existing screenshot: ") + Pending);
                Pending.Empty();
                return;
            }
            FScreenshotRequest::RequestScreenshot(Pending, true, false);
        }
        bool Written()
        {
            if (Pending.IsEmpty()) return false;
            return IFileManager::Get().FileSize(*Pending) > 1024;
        }
        FAutomationTestBase* Test;
        TWeakObjectPtr<APawn> Player, Npc;
        FString Directory, Pending;
        int32 Stage = 0;
        double Started = -1, LastAction = 0;
        uint64 CameraInputFrame = 0;
        bool bReleaseCameraInput = false;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNpcVisualCapture, "NPC.Visual.Capture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNpcVisualCapture::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("Editor available"), GEditor) || GEditor->PlayWorld) return false;
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/ThirdPerson/Maps/ThirdPersonMap")));
    ADD_LATENT_AUTOMATION_COMMAND(NpcVisualAudit::FStartWindow());
    ADD_LATENT_AUTOMATION_COMMAND(NpcVisualAudit::FCapture(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}
#endif
