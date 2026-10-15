#include "NpcBlueprintPatchLibrary.h"
#include "NpcAuditLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace NpcReviewedPatch
{
    static UEdGraphNode* Node(UBlueprint* Blueprint, const TCHAR* Id)
    {
        FGuid Guid;
        FGuid::Parse(Id, Guid);
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
            for (UEdGraphNode* Current : Graph->Nodes)
                if (Current && Current->NodeGuid == Guid) return Current;
        return nullptr;
    }
    static UEdGraphPin* Pin(UEdGraphNode* Owner, const TCHAR* Name)
    {
        return Owner ? Owner->FindPin(FName(Name)) : nullptr;
    }
    static bool SingleEdge(UEdGraphPin* Out, UEdGraphPin* In)
    {
        return Out && In && Out->LinkedTo.Num() == 1 && Out->LinkedTo[0] == In;
    }
}

FString UNpcBlueprintPatchLibrary::CorrectNpcBlueprint(UObject* BlueprintAsset)
{
    using namespace NpcReviewedPatch;
    UBlueprint* Blueprint = Cast<UBlueprint>(BlueprintAsset);
    if (!Blueprint || Blueprint->GetPathName() != TEXT("/Game/ThirdPerson/Blueprints/BP_NPC_Standard.BP_NPC_Standard"))
        return TEXT("{\"ok\":false,\"error\":\"unexpected_blueprint\"}");
    UEdGraphNode* Quest = Node(Blueprint, TEXT("03A23920-4C29-D6EE-2BF3-33B9586AE665"));
    UEdGraphNode* Roll = Node(Blueprint, TEXT("5166DE7F-4BFA-ED5E-573F-F3879C1BD329"));
    UEdGraphNode* Player = Node(Blueprint, TEXT("3985C114-4B65-FBBC-4680-63A218085D37"));
    UEdGraphNode* Money = Node(Blueprint, TEXT("72C0987B-4585-79D3-EEF3-3884D9F01F2D"));
    UEdGraphNode* Greeting = Node(Blueprint, TEXT("C50A5832-4BCE-3C57-613A-FB8AE31105B1"));
    UEdGraphNode* Schedule = Node(Blueprint, TEXT("9DB60F60-464F-F40E-45E5-4CAE6E1BA158"));
    UEdGraphNode* Home = Node(Blueprint, TEXT("B83AADD6-49F0-407A-4574-B081B9623A81"));
    UEdGraphNode* Interacting = Node(Blueprint, TEXT("6267F6D9-4B47-0FBB-4A72-0F9E82C67E8A"));
    if (!Quest || !Roll || !Player || !Money || !Greeting || !Schedule || !Home || !Interacting)
        return TEXT("{\"ok\":false,\"error\":\"reviewed_anchor_missing\"}");
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    const bool bAlready = SingleEdge(Pin(Quest, TEXT("else")), Pin(Player, TEXT("execute")))
        && SingleEdge(Pin(Money, TEXT("else")), Pin(Roll, TEXT("execute")))
        && SingleEdge(Pin(Roll, TEXT("else")), Pin(Greeting, TEXT("execute")));
    if (bAlready && Node(Blueprint, TEXT("15C20261-0C15-4F2A-AB80-1D0180000001")))
        return TEXT("{\"ok\":true,\"changed\":false,\"asset_saved\":false,\"status\":\"already_corrected\"}");
    if (!SingleEdge(Pin(Quest, TEXT("else")), Pin(Roll, TEXT("execute")))
        || !SingleEdge(Pin(Roll, TEXT("else")), Pin(Player, TEXT("execute")))
        || !SingleEdge(Pin(Money, TEXT("else")), Pin(Greeting, TEXT("execute")))
        || !SingleEdge(Pin(Schedule, TEXT("else")), Pin(Home, TEXT("execute"))))
        return TEXT("{\"ok\":false,\"error\":\"initial_connections_do_not_match_reviewed_snapshot\"}");
    Blueprint->Modify();
    Quest->GetGraph()->Modify();
    Pin(Quest, TEXT("else"))->BreakLinkTo(Pin(Roll, TEXT("execute")));
    Pin(Roll, TEXT("else"))->BreakLinkTo(Pin(Player, TEXT("execute")));
    Pin(Money, TEXT("else"))->BreakLinkTo(Pin(Greeting, TEXT("execute")));
    bool bConnected = Schema->TryCreateConnection(Pin(Quest, TEXT("else")), Pin(Player, TEXT("execute")));
    bConnected &= Schema->TryCreateConnection(Pin(Money, TEXT("else")), Pin(Roll, TEXT("execute")));
    bConnected &= Schema->TryCreateConnection(Pin(Roll, TEXT("else")), Pin(Greeting, TEXT("execute")));
    UEdGraph* Graph = Schedule->GetGraph();
    UK2Node_IfThenElse* Guard = NewObject<UK2Node_IfThenElse>(Graph);
    FGuid::Parse(TEXT("15C20261-0C15-4F2A-AB80-1D0180000001"), Guard->NodeGuid);
    Guard->NodePosX = Home->NodePosX - 240;
    Guard->NodePosY = Home->NodePosY + 160;
    Guard->NodeComment = TEXT("Keep an active dialogue or trade open; return home after interaction closes.");
    Guard->bCommentBubbleVisible = true;
    Graph->AddNode(Guard, false, false);
    Guard->AllocateDefaultPins();
    Pin(Schedule, TEXT("else"))->BreakLinkTo(Pin(Home, TEXT("execute")));
    bConnected &= Schema->TryCreateConnection(Pin(Schedule, TEXT("else")), Guard->GetExecPin());
    bConnected &= Schema->TryCreateConnection(Pin(Interacting, TEXT("IsInteracting")), Guard->GetConditionPin());
    bConnected &= Schema->TryCreateConnection(Guard->GetElsePin(), Pin(Home, TEXT("execute")));
    if (!bConnected)
        return TEXT("{\"ok\":false,\"error\":\"connection_rejected_do_not_save\",\"asset_saved\":false}");
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    return UNpcAuditLibrary::CompileBlueprint(Blueprint);
}
