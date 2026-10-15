#include "NpcVendorPatchLibrary.h"

#include "NpcAuditLibrary.h"
#include "Components/SkeletalMeshComponent.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_GetArrayItem.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"

namespace NpcReviewedVendorPatch
{
    static const TCHAR* ReplacementGuid = TEXT("BD14481E-351A-4F06-9A15-500800000001");

    struct FNpcVendorEdge
    {
        UEdGraphPin* Output = nullptr;
        UEdGraphPin* Input = nullptr;
    };

    static TSharedRef<FJsonObject> Response(bool bOk)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("schema"), TEXT("npc-vendor-construction-fix/v1"));
        Result->SetStringField(TEXT("operation"), TEXT("correct_butterfly"));
        Result->SetBoolField(TEXT("ok"), bOk);
        Result->SetBoolField(TEXT("asset_saved"), false);
        Result->SetBoolField(TEXT("changed"), false);
        return Result;
    }

    static FString Encode(const TSharedRef<FJsonObject>& Result)
    {
        FString Text;
        FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Text));
        return Text;
    }

    static FString Failure(const TCHAR* Code)
    {
        TSharedRef<FJsonObject> Result = Response(false);
        Result->SetStringField(TEXT("error"), Code);
        return Encode(Result);
    }

    static UEdGraphNode* FindNode(UBlueprint* Blueprint, const TCHAR* Id, const TCHAR* ClassName)
    {
        FGuid Guid;
        if (!FGuid::Parse(Id, Guid))
        {
            return nullptr;
        }
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        UEdGraphNode* Found = nullptr;
        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && Node->NodeGuid == Guid)
                {
                    if (Found || Node->GetClass()->GetName() != ClassName)
                    {
                        return nullptr;
                    }
                    Found = Node;
                }
            }
        }
        return Found;
    }

    static UEdGraphPin* Pin(UEdGraphNode* Node, const TCHAR* Name)
    {
        return Node ? Node->FindPin(FName(Name)) : nullptr;
    }

    // The original asset predates current pin serialization. Loading/reconstructing
    // its nodes creates fresh PinIds; identity is the stable node GUID plus the
    // exact reviewed pin declaration and reciprocal adjacency, never a session ID.
    static UEdGraphPin* ReviewedPin(UEdGraphNode* Node, const TCHAR* Name,
        EEdGraphPinDirection Direction, const TCHAR* Category,
        const TCHAR* Subcategory = TEXT("None"), const TCHAR* ObjectPath = nullptr,
        EPinContainerType Container = EPinContainerType::None)
    {
        UEdGraphPin* Found = Pin(Node, Name);
        if (!Found || Found->Direction != Direction || Found->PinType.PinCategory != FName(Category)
            || Found->PinType.PinSubCategory != FName(Subcategory) || Found->PinType.ContainerType != Container
            || Found->PinType.bIsReference || Found->PinType.bIsConst || Found->bOrphanedPin || Found->ParentPin)
        {
            return nullptr;
        }
        int32 MatchingNames = 0;
        for (UEdGraphPin* Candidate : Node->Pins)
        {
            MatchingNames += Candidate && Candidate->PinName == FName(Name) ? 1 : 0;
        }
        const UObject* TypeObject = Found->PinType.PinSubCategoryObject.Get();
        if (MatchingNames != 1 || (ObjectPath ? (!TypeObject || TypeObject->GetPathName() != ObjectPath) : TypeObject != nullptr))
        {
            return nullptr;
        }
        return Found;
    }

    static bool Calls(UEdGraphNode* Node, const TCHAR* FunctionName, const TCHAR* OwnerPath)
    {
        const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
        const UFunction* Function = Call ? Call->GetTargetFunction() : nullptr;
        return Function && Function->GetOwnerClass() && Function->GetFName() == FName(FunctionName)
            && Function->GetOwnerClass()->GetPathName() == OwnerPath;
    }

    // Compare complete reciprocal adjacency for every observed endpoint, including empty ports.
    static bool Matches(const TArray<FNpcVendorEdge>& Edges, const TSet<UEdGraphPin*>& Observed)
    {
        TMap<UEdGraphPin*, TArray<UEdGraphPin*>> Expected;
        for (UEdGraphPin* Port : Observed)
        {
            if (!Port)
            {
                return false;
            }
            Expected.Add(Port);
        }
        for (const FNpcVendorEdge& Edge : Edges)
        {
            if (!Edge.Output || !Edge.Input || Edge.Output->Direction != EGPD_Output
                || Edge.Input->Direction != EGPD_Input)
            {
                return false;
            }
            Expected.FindOrAdd(Edge.Output).AddUnique(Edge.Input);
            Expected.FindOrAdd(Edge.Input).AddUnique(Edge.Output);
        }
        for (const TPair<UEdGraphPin*, TArray<UEdGraphPin*>>& Entry : Expected)
        {
            if (Entry.Key->LinkedTo.Num() != Entry.Value.Num())
            {
                return false;
            }
            for (UEdGraphPin* Other : Entry.Value)
            {
                if (!Entry.Key->LinkedTo.Contains(Other) || !Other->LinkedTo.Contains(Entry.Key))
                {
                    return false;
                }
            }
        }
        return true;
    }

    // Fingerprint all other actual nodes/defaults/pins/edges in BOTH graphs. Edges to the
    // replaced nodes are the only excluded links. No independent animation simulation.
    static FString UntouchedGraphDigest(UBlueprint* Blueprint, const TSet<UEdGraphNode*>& Excluded)
    {
        TArray<FString> Records;
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node || Excluded.Contains(Node))
                {
                    continue;
                }
                const FString NodeKey = Graph->GraphGuid.ToString() + TEXT(":") + Node->NodeGuid.ToString();
                Records.Add(NodeKey + TEXT(":class:") + Node->GetClass()->GetPathName());
                if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
                {
                    const UFunction* Function = Call->GetTargetFunction();
                    Records.Add(NodeKey + TEXT(":function:") + (Function ? Function->GetPathName() : FString()));
                }
                for (UEdGraphPin* Port : Node->Pins)
                {
                    if (!Port)
                    {
                        continue;
                    }
                    const FString PortKey = NodeKey + TEXT(":") + Port->PinId.ToString();
                    Records.Add(PortKey + TEXT(":name:") + Port->PinName.ToString());
                    Records.Add(PortKey + TEXT(":default:") + Port->DefaultValue);
                    Records.Add(PortKey + TEXT(":default_text:") + Port->DefaultTextValue.ToString());
                    Records.Add(PortKey + TEXT(":default_object:")
                        + (Port->DefaultObject ? Port->DefaultObject->GetPathName() : FString()));
                    Records.Add(PortKey + TEXT(":type:") + Port->PinType.PinCategory.ToString()
                        + TEXT(":") + Port->PinType.PinSubCategory.ToString());
                    for (UEdGraphPin* Other : Port->LinkedTo)
                    {
                        if (Other && !Excluded.Contains(Other->GetOwningNode()))
                        {
                            Records.Add(PortKey + TEXT(":link:") + Other->GetOwningNode()->NodeGuid.ToString()
                                + TEXT(":") + Other->PinId.ToString());
                        }
                    }
                }
            }
        }
        Records.Sort();
        const FString Joined = FString::Join(Records, TEXT("\n"));
        FTCHARToUTF8 Bytes(*Joined);
        uint8 Digest[FSHA1::DigestSize];
        FSHA1::HashBuffer(Bytes.Get(), Bytes.Length(), Digest);
        return BytesToHex(Digest, FSHA1::DigestSize).ToLower();
    }

    static TSharedRef<FJsonObject> CompileResult(UBlueprint* Blueprint, bool bChanged,
        const FString& BeforeDigest, const FString& AfterDigest)
    {
        TSharedPtr<FJsonObject> Compilation;
        const FString CompilationText = UNpcAuditLibrary::CompileBlueprint(Blueprint);
        bool bCompileOk = false;
        double Errors = -1;
        double Warnings = -1;
        const bool bParsed = FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(CompilationText), Compilation)
            && Compilation.IsValid();
        const bool bClean = bParsed && Compilation->TryGetBoolField(TEXT("ok"), bCompileOk) && bCompileOk
            && Compilation->TryGetNumberField(TEXT("errors"), Errors) && Errors == 0
            && Compilation->TryGetNumberField(TEXT("warnings"), Warnings) && Warnings == 0
            && Blueprint->Status == BS_UpToDate;
        const bool bUnchanged = BeforeDigest == AfterDigest;
        TSharedRef<FJsonObject> Result = Response(bClean && bUnchanged);
        Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
        Result->SetBoolField(TEXT("changed"), bChanged);
        Result->SetBoolField(TEXT("compiled_this_call"), true);
        Result->SetStringField(TEXT("status"), bClean && bUnchanged
            ? (bChanged ? TEXT("construction_fix_compiled_in_memory") : TEXT("already_corrected"))
            : TEXT("validation_failed_do_not_save"));
        Result->SetStringField(TEXT("replacement_function"), TEXT("USkeletalMeshComponent::OverrideAnimationData"));
        Result->SetBoolField(TEXT("unrelated_graph_snapshot_preserved"), bUnchanged);
        Result->SetStringField(TEXT("untouched_graph_sha1_before"), BeforeDigest);
        Result->SetStringField(TEXT("untouched_graph_sha1_after"), AfterDigest);
        Result->SetBoolField(TEXT("animation_array_selection_preserved"), true);
        Result->SetBoolField(TEXT("rate_expression_preserved"), true);
        Result->SetBoolField(TEXT("event_graph_untouched"), true);
        Result->SetStringField(TEXT("playback_initial_state"), TEXT("single-node animation, looping=true, playing=true, position=0, existing rate expression"));
        Result->SetStringField(TEXT("preservation_scope"), TEXT("reviewed graph inputs and topology; actual preview and runtime animation must also be exercised in Unreal"));
        if (bParsed)
        {
            Result->SetObjectField(TEXT("compilation"), Compilation);
        }
        return Result;
    }
}

FString UNpcVendorPatchLibrary::CorrectButterfly(UObject* BlueprintAsset)
{
    using namespace NpcReviewedVendorPatch;
    UBlueprint* Blueprint = Cast<UBlueprint>(BlueprintAsset);
    if (!IsInGameThread() || !IsValid(Blueprint) || !GEditor || GEditor->PlayWorld
        || Blueprint->GetPathName() != TEXT("/Game/AdvancedVillagePack/Blueprints/BP_Butterfly.BP_Butterfly"))
    {
        return Failure(TEXT("reviewed_butterfly_asset_on_editor_game_thread_with_pie_stopped_required"));
    }
    UEdGraphNode* Sequence = FindNode(Blueprint, TEXT("2EB8FB71-4119-F658-4225-D092B9D5CE6B"), TEXT("K2Node_ExecutionSequence"));
    UEdGraphNode* Mesh = FindNode(Blueprint, TEXT("CF8EB73F-4C13-FEE7-001C-DEAA4FA3164E"), TEXT("K2Node_VariableGet"));
    UEdGraphNode* AnimationArray = FindNode(Blueprint, TEXT("44BF59BC-4DE0-851A-9754-9D8C1148A9D0"), TEXT("K2Node_VariableGet"));
    UEdGraphNode* AnimationIndex = FindNode(Blueprint, TEXT("ECC02893-436D-C029-19E7-A19AD6B8289E"), TEXT("K2Node_VariableGet"));
    UEdGraphNode* Lerp = FindNode(Blueprint, TEXT("7A64623F-4D73-97F9-7563-0291DAB1682C"), TEXT("K2Node_CallFunction"));
    UEdGraphNode* Play = FindNode(Blueprint, TEXT("EFAD2380-4EC2-D62A-7F90-FFA491F74CA4"), TEXT("K2Node_CallFunction"));
    UEdGraphNode* Rate = FindNode(Blueprint, TEXT("FEFC1BB9-47CC-A12E-B7AF-21BCED46B6A8"), TEXT("K2Node_CallFunction"));
    UEdGraphNode* Existing = FindNode(Blueprint, ReplacementGuid, TEXT("K2Node_CallFunction"));
    // UE 5.4 replaces legacy Array_Get calls with newly spawned GetArrayItem
    // nodes while loading this original asset. Their GUIDs and pin IDs are NOT
    // stable across loads (K2Node_CallArrayFunction::ConvertDeprecatedNode and
    // UBlueprintNodeSpawner::SpawnEdGraphNode). Identify that migrated node from
    // the stable original sink, then require both exact original getter inputs.
    UEdGraphPin* AnimationSink = Play ? Pin(Play, TEXT("NewAnimToPlay")) : Pin(Existing, TEXT("InAnimToPlay"));
    UK2Node_GetArrayItem* Selection = AnimationSink && AnimationSink->LinkedTo.Num() == 1 && AnimationSink->LinkedTo[0]
        ? Cast<UK2Node_GetArrayItem>(AnimationSink->LinkedTo[0]->GetOwningNode()) : nullptr;
    UEdGraph* Graph = Sequence ? Sequence->GetGraph() : nullptr;
    FGuid ExpectedGraph;
    FGuid::Parse(TEXT("70602F3E-473F-8F1B-3924-A0927DEABBE3"), ExpectedGraph);
    if (!Graph || Graph->GraphGuid != ExpectedGraph || Graph->GetName() != TEXT("UserConstructionScript"))
    {
        return Failure(TEXT("reviewed_construction_graph_required"));
    }
    const TArray<UEdGraphNode*> StableAnchors = {Sequence, Mesh, AnimationArray, AnimationIndex, Lerp};
    for (UEdGraphNode* Node : StableAnchors)
    {
        if (!Node || Node->GetGraph() != Graph)
        {
            return Failure(TEXT("unique_reviewed_node_anchors_required"));
        }
    }
    UEdGraphPin* Start = ReviewedPin(Sequence, TEXT("then_1"), EGPD_Output, TEXT("exec"));
    UEdGraphPin* MeshOutput = ReviewedPin(Mesh, TEXT("Butterfly_Anim_Var01"), EGPD_Output,
        TEXT("object"), TEXT("None"), TEXT("/Script/Engine.SkeletalMeshComponent"));
    UEdGraphPin* AnimationArrayOutput = ReviewedPin(AnimationArray, TEXT("ButterflyPathAnimations"), EGPD_Output,
        TEXT("object"), TEXT("None"), TEXT("/Script/Engine.AnimSequence"), EPinContainerType::Array);
    UEdGraphPin* AnimationIndexOutput = ReviewedPin(AnimationIndex, TEXT("PathAnimationVariation"), EGPD_Output, TEXT("int"));
    UEdGraphPin* AnimationOutput = ReviewedPin(Selection, TEXT("Output"), EGPD_Output,
        TEXT("object"), TEXT("None"), TEXT("/Script/Engine.AnimSequence"));
    UEdGraphPin* RateOutput = ReviewedPin(Lerp, TEXT("ReturnValue"), EGPD_Output, TEXT("real"), TEXT("double"));
    if (!Start || !MeshOutput || !AnimationArrayOutput || !AnimationIndexOutput || !RateOutput)
    {
        return Failure(TEXT("reviewed_source_pin_declarations_required"));
    }
    const FBoolProperty* ReturnByReference = Selection
        ? FindFProperty<FBoolProperty>(Selection->GetClass(), TEXT("bReturnByRefDesired")) : nullptr;
    if (!Selection || Selection->GetClass() != UK2Node_GetArrayItem::StaticClass()
        || Selection->GetGraph() != Graph || Selection->Pins.Num() != 3
        || !ReturnByReference || ReturnByReference->GetPropertyValue_InContainer(Selection)
        || !AnimationOutput || AnimationOutput->PinName != FName(TEXT("Output")) || AnimationOutput->PinType.bIsReference)
    {
        return Failure(TEXT("reviewed_migrated_array_selection_required"));
    }
    auto SelectorMatches = [Selection, AnimationArrayOutput, AnimationIndexOutput, AnimationOutput](UEdGraphPin* Sink)
    {
        UEdGraphPin* ArrayInput = ReviewedPin(Selection, TEXT("Array"), EGPD_Input,
            TEXT("object"), TEXT("None"), TEXT("/Script/Engine.AnimSequence"), EPinContainerType::Array);
        UEdGraphPin* IndexInput = ReviewedPin(Selection, TEXT("Dimension 1"), EGPD_Input, TEXT("int"));
        const TArray<FNpcVendorEdge> Edges = {{AnimationArrayOutput, ArrayInput},
            {AnimationIndexOutput, IndexInput}, {AnimationOutput, Sink}};
        const TSet<UEdGraphPin*> Ports = {AnimationArrayOutput, AnimationIndexOutput,
            ArrayInput, IndexInput, AnimationOutput, Sink};
        return Matches(Edges, Ports);
    };
    if (!SelectorMatches(AnimationSink))
    {
        return Failure(TEXT("migrated_selection_does_not_have_exact_original_getter_edges"));
    }
    auto AddedEdges = [Start, MeshOutput, AnimationOutput, RateOutput](UEdGraphNode* NewNode)
    {
        return TArray<FNpcVendorEdge>{
            {Start, Pin(NewNode, TEXT("execute"))},
            {MeshOutput, Pin(NewNode, TEXT("self"))},
            {AnimationOutput, Pin(NewNode, TEXT("InAnimToPlay"))},
            {RateOutput, Pin(NewNode, TEXT("PlayRate"))}
        };
    };
    auto AddedPorts = [Start, MeshOutput, AnimationOutput, RateOutput](UEdGraphNode* NewNode)
    {
        TSet<UEdGraphPin*> Observed = {Start, MeshOutput, AnimationOutput, RateOutput};
        for (UEdGraphPin* Port : NewNode->Pins)
        {
            Observed.Add(Port);
        }
        return Observed;
    };
    auto NewDefaultsMatch = [](UEdGraphNode* Node)
    {
        UEdGraphPin* Looping = Pin(Node, TEXT("bIsLooping"));
        UEdGraphPin* Playing = Pin(Node, TEXT("bIsPlaying"));
        UEdGraphPin* Position = Pin(Node, TEXT("Position"));
        return Node && Node->Pins.Num() == 8 && Looping && Playing && Position
            && Looping->DefaultValue == TEXT("true") && Playing->DefaultValue == TEXT("true")
            && FCString::Atod(*Position->DefaultValue) == 0.0;
    };
    if (Existing)
    {
        if (Play || Rate || Existing->GetGraph() != Graph
            || !Calls(Existing, TEXT("OverrideAnimationData"), TEXT("/Script/Engine.SkeletalMeshComponent"))
            || !NewDefaultsMatch(Existing) || !Matches(AddedEdges(Existing), AddedPorts(Existing))
            || !SelectorMatches(Pin(Existing, TEXT("InAnimToPlay"))))
        {
            return Failure(TEXT("existing_replacement_does_not_match_reviewed_patch"));
        }
        const FString Digest = UntouchedGraphDigest(Blueprint, {Existing});
        return Encode(CompileResult(Blueprint, false, Digest, Digest));
    }
    if (!Play || !Rate || Play->GetGraph() != Graph || Rate->GetGraph() != Graph
        || Play->Pins.Num() != 5 || Rate->Pins.Num() != 4
        || !Calls(Play, TEXT("PlayAnimation"), TEXT("/Script/Engine.SkeletalMeshComponent"))
        || !Calls(Rate, TEXT("SetPlayRate"), TEXT("/Script/Engine.SkeletalMeshComponent")))
    {
        return Failure(TEXT("original_animation_calls_required"));
    }
    UEdGraphPin* PlayExec = ReviewedPin(Play, TEXT("execute"), EGPD_Input, TEXT("exec"));
    UEdGraphPin* PlayThen = ReviewedPin(Play, TEXT("then"), EGPD_Output, TEXT("exec"));
    UEdGraphPin* PlaySelf = ReviewedPin(Play, TEXT("self"), EGPD_Input,
        TEXT("object"), TEXT("None"), TEXT("/Script/Engine.SkeletalMeshComponent"));
    UEdGraphPin* PlayAnimation = ReviewedPin(Play, TEXT("NewAnimToPlay"), EGPD_Input,
        TEXT("object"), TEXT("None"), TEXT("/Script/Engine.AnimationAsset"));
    UEdGraphPin* PlayLoop = ReviewedPin(Play, TEXT("bLooping"), EGPD_Input, TEXT("bool"));
    UEdGraphPin* RateExec = ReviewedPin(Rate, TEXT("execute"), EGPD_Input, TEXT("exec"));
    UEdGraphPin* RateThen = ReviewedPin(Rate, TEXT("then"), EGPD_Output, TEXT("exec"));
    UEdGraphPin* RateSelf = ReviewedPin(Rate, TEXT("self"), EGPD_Input,
        TEXT("object"), TEXT("None"), TEXT("/Script/Engine.SkeletalMeshComponent"));
    UEdGraphPin* RateInput = ReviewedPin(Rate, TEXT("Rate"), EGPD_Input, TEXT("real"), TEXT("float"));
    const TArray<FNpcVendorEdge> OriginalEdges = {
        {Start, PlayExec}, {PlayThen, RateExec}, {MeshOutput, PlaySelf},
        {MeshOutput, RateSelf}, {AnimationOutput, PlayAnimation}, {RateOutput, RateInput}
    };
    const TSet<UEdGraphPin*> OriginalPorts = {Start, MeshOutput, AnimationOutput, RateOutput,
        PlayExec, PlayThen, PlaySelf, PlayAnimation, PlayLoop, RateExec, RateThen, RateSelf, RateInput};
    if (!PlayLoop || PlayLoop->DefaultValue != TEXT("true") || !Matches(OriginalEdges, OriginalPorts))
    {
        return Failure(TEXT("original_animation_pin_defaults_or_complete_edges_differ"));
    }
    UFunction* Override = USkeletalMeshComponent::StaticClass()->FindFunctionByName(TEXT("OverrideAnimationData"));
    if (!Override || Override->HasMetaData(TEXT("UnsafeDuringActorConstruction")))
    {
        return Failure(TEXT("construction_safe_engine_override_function_required"));
    }
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    UK2Node_CallFunction* Replacement = NewObject<UK2Node_CallFunction>(Graph, NAME_None, RF_Transactional);
    Replacement->SetFromFunction(Override);
    FGuid::Parse(ReplacementGuid, Replacement->NodeGuid);
    Replacement->NodePosX = Play->NodePosX;
    Replacement->NodePosY = Play->NodePosY;
    Replacement->NodeComment = TEXT("Construction-safe animation data; original animation selection and play-rate expression retained.");
    Replacement->AllocateDefaultPins();
    UEdGraphPin* Looping = Pin(Replacement, TEXT("bIsLooping"));
    UEdGraphPin* Playing = Pin(Replacement, TEXT("bIsPlaying"));
    UEdGraphPin* Position = Pin(Replacement, TEXT("Position"));
    if (!Looping || !Playing || !Position)
    {
        return Failure(TEXT("engine_override_pin_layout_differs"));
    }
    Schema->TrySetDefaultValue(*Looping, TEXT("true"), false);
    Schema->TrySetDefaultValue(*Playing, TEXT("true"), false);
    Schema->TrySetDefaultValue(*Position, TEXT("0.0"), false);
    const TArray<FNpcVendorEdge> NewEdges = AddedEdges(Replacement);
    if (!NewDefaultsMatch(Replacement))
    {
        return Failure(TEXT("engine_override_defaults_rejected"));
    }
    for (const FNpcVendorEdge& Edge : NewEdges)
    {
        if (!Edge.Output || !Edge.Input)
        {
            return Failure(TEXT("replacement_connection_preflight_rejected"));
        }
        const ECanCreateConnectionResponse Connection = Schema->CanCreateConnection(Edge.Output, Edge.Input).Response;
        // Reject implicit conversion/promotion nodes: only this one reviewed call
        // may be inserted, and the original Lerp expression must remain unchanged.
        if (Connection != CONNECT_RESPONSE_MAKE && Connection != CONNECT_RESPONSE_BREAK_OTHERS_A
            && Connection != CONNECT_RESPONSE_BREAK_OTHERS_B && Connection != CONNECT_RESPONSE_BREAK_OTHERS_AB)
        {
            return Failure(TEXT("replacement_connection_preflight_rejected"));
        }
    }
    const FString BeforeDigest = UntouchedGraphDigest(Blueprint, {Play, Rate});
    Blueprint->Modify();
    Graph->Modify();
    Play->Modify();
    Rate->Modify();
    Graph->AddNode(Replacement, false, false);
    for (const FNpcVendorEdge& Edge : OriginalEdges)
    {
        Edge.Output->BreakLinkTo(Edge.Input);
    }
    bool bConnected = true;
    for (const FNpcVendorEdge& Edge : NewEdges)
    {
        bConnected &= Schema->TryCreateConnection(Edge.Output, Edge.Input);
    }
    if (!bConnected || !Matches(NewEdges, AddedPorts(Replacement))
        || !SelectorMatches(Pin(Replacement, TEXT("InAnimToPlay"))))
    {
        Replacement->BreakAllNodeLinks();
        Graph->RemoveNode(Replacement);
        for (const FNpcVendorEdge& Edge : OriginalEdges)
        {
            Schema->TryCreateConnection(Edge.Output, Edge.Input);
        }
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        TSharedRef<FJsonObject> Result = Response(false);
        Result->SetStringField(TEXT("error"), TEXT("replacement_connection_failed_do_not_save"));
        Result->SetBoolField(TEXT("original_topology_restored"), Matches(OriginalEdges, OriginalPorts));
        return Encode(Result);
    }
    // Removing these nodes is necessary: merely disconnecting construction-unsafe
    // function calls can still leave compiler warnings. The original ZIP/graph dump
    // remains the source archive; this API does not overwrite it or save the asset.
    FBlueprintEditorUtils::RemoveNode(Blueprint, Play, true);
    FBlueprintEditorUtils::RemoveNode(Blueprint, Rate, true);
    const FString AfterDigest = UntouchedGraphDigest(Blueprint, {Replacement});
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    return Encode(CompileResult(Blueprint, true, BeforeDigest, AfterDigest));
}
