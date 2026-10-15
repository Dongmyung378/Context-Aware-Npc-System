#include "NpcTickOptimizationLibrary.h"
#include "NpcAuditLibrary.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace NpcTickReviewedOptimization
{
    struct FNpcReviewedEdge
    {
        UEdGraphPin* Output = nullptr;
        UEdGraphPin* Input = nullptr;
    };

    static FString Encode(const TSharedRef<FJsonObject>& Object)
    {
        FString Text;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
        FJsonSerializer::Serialize(Object, Writer);
        return Text;
    }

    static TSharedRef<FJsonObject> Response(bool bOk)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("schema"), TEXT("npc-tick-lookup-candidate/v1"));
        Result->SetStringField(TEXT("operation"), TEXT("optimize_tick_lookup"));
        Result->SetBoolField(TEXT("ok"), bOk);
        Result->SetBoolField(TEXT("asset_saved"), false);
        Result->SetBoolField(TEXT("changed"), false);
        return Result;
    }

    static FString Failure(const TCHAR* Code)
    {
        TSharedRef<FJsonObject> Result = Response(false);
        Result->SetStringField(TEXT("error"), Code);
        return Encode(Result);
    }

    static UEdGraphNode* FindAnchor(UBlueprint* Blueprint, const TCHAR* Id, const TCHAR* ClassName)
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

    // Check the full adjacency set of every observed pin, including reciprocal
    // links and intentionally unconnected ports. No graph mutation occurs here.
    static bool Matches(const TArray<FNpcReviewedEdge>& Edges, const TSet<UEdGraphPin*>& Observed)
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
        for (const FNpcReviewedEdge& Edge : Edges)
        {
            if (!Edge.Output || !Edge.Input || Edge.Output->Direction != EGPD_Output
                || Edge.Input->Direction != EGPD_Input)
            {
                return false;
            }
            Expected.FindOrAdd(Edge.Output).AddUnique(Edge.Input);
            Expected.FindOrAdd(Edge.Input).AddUnique(Edge.Output);
        }
        for (const TPair<UEdGraphPin*, TArray<UEdGraphPin*>>& Pair : Expected)
        {
            if (Pair.Key->LinkedTo.Num() != Pair.Value.Num())
            {
                return false;
            }
            for (UEdGraphPin* Neighbor : Pair.Value)
            {
                if (!Pair.Key->LinkedTo.Contains(Neighbor) || !Neighbor->LinkedTo.Contains(Pair.Key))
                {
                    return false;
                }
            }
        }
        return true;
    }

    static TSharedRef<FJsonObject> Endpoint(UEdGraphPin* Port)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("node_guid"),
            Port->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
        Result->SetStringField(TEXT("pin_name"), Port->PinName.ToString());
        Result->SetStringField(TEXT("pin_guid"), Port->PinId.ToString(EGuidFormats::DigitsWithHyphens));
        return Result;
    }

    static TArray<TSharedPtr<FJsonValue>> EdgeLog(const TArray<FNpcReviewedEdge>& Edges)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        for (const FNpcReviewedEdge& Edge : Edges)
        {
            TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetObjectField(TEXT("output"), Endpoint(Edge.Output));
            Entry->SetObjectField(TEXT("input"), Endpoint(Edge.Input));
            Result.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return Result;
    }
}

FString UNpcTickOptimizationLibrary::OptimizeTickLookup(UObject* BlueprintAsset)
{
    using namespace NpcTickReviewedOptimization;
    UBlueprint* Blueprint = Cast<UBlueprint>(BlueprintAsset);
    if (!IsInGameThread() || !IsValid(Blueprint)
        || Blueprint->GetPathName() != TEXT("/Game/ThirdPerson/Blueprints/BP_NPC_Standard.BP_NPC_Standard"))
    {
        return Failure(TEXT("reviewed_blueprint_on_game_thread_required"));
    }

    UEdGraphNode* Tick = FindAnchor(Blueprint, TEXT("DC53B1CE-4E93-EF92-6074-02958AF40D33"), TEXT("K2Node_Event"));
    UEdGraphNode* Sequence = FindAnchor(Blueprint, TEXT("4E338E87-4961-2171-ABAE-1BA87FA36E15"), TEXT("K2Node_ExecutionSequence"));
    UEdGraphNode* SharedCast = FindAnchor(Blueprint, TEXT("A15444DF-49CF-CBEB-3AB0-BDB507A1C8B1"), TEXT("K2Node_DynamicCast"));
    UEdGraphNode* CastOne = FindAnchor(Blueprint, TEXT("D5AAF500-4096-5B7A-E41B-CEAC051FC5F3"), TEXT("K2Node_DynamicCast"));
    UEdGraphNode* CastTwo = FindAnchor(Blueprint, TEXT("EA7327E0-4D10-73E9-B751-F884D65872BD"), TEXT("K2Node_DynamicCast"));
    UEdGraphNode* BranchZero = FindAnchor(Blueprint, TEXT("9DB60F60-464F-F40E-45E5-4CAE6E1BA158"), TEXT("K2Node_IfThenElse"));
    UEdGraphNode* BranchOne = FindAnchor(Blueprint, TEXT("2964D7D6-4AC0-351E-D059-769F580070B1"), TEXT("K2Node_IfThenElse"));
    UEdGraphNode* BranchTwo = FindAnchor(Blueprint, TEXT("8EA2F7F6-4542-71CE-70D9-819F1C21AE1D"), TEXT("K2Node_IfThenElse"));
    UEdGraphNode* SharedGet = FindAnchor(Blueprint, TEXT("F6CB2BE8-41C6-4B07-2718-E69FD343F7D7"), TEXT("K2Node_CallFunction"));
    UEdGraphNode* GetOne = FindAnchor(Blueprint, TEXT("915F2EE9-470D-7CB0-C5F7-06BCBA33442B"), TEXT("K2Node_CallFunction"));
    UEdGraphNode* GetTwo = FindAnchor(Blueprint, TEXT("B65F47D5-4380-0729-4C3D-9ABBCDF7E66A"), TEXT("K2Node_CallFunction"));
    UEdGraphNode* TimeZero = FindAnchor(Blueprint, TEXT("8784A915-45F1-1AA7-4FF3-A6A3D0EE6057"), TEXT("K2Node_VariableGet"));
    UEdGraphNode* TimeOne = FindAnchor(Blueprint, TEXT("5AF0D799-4D79-B5C4-CDC9-2DAA5E0BA140"), TEXT("K2Node_VariableGet"));
    UEdGraphNode* TimeTwo = FindAnchor(Blueprint, TEXT("63519E1F-4ED2-9B28-0C79-EF8A6D5F00C9"), TEXT("K2Node_VariableGet"));
    const TArray<UEdGraphNode*> Anchors = {Tick, Sequence, SharedCast, CastOne, CastTwo,
        BranchZero, BranchOne, BranchTwo, SharedGet, GetOne, GetTwo, TimeZero, TimeOne, TimeTwo};
    UEdGraph* Graph = Tick ? Tick->GetGraph() : nullptr;
    for (UEdGraphNode* Anchor : Anchors)
    {
        if (!Anchor || Anchor->GetGraph() != Graph || !Graph || Graph->GetName() != TEXT("EventGraph"))
        {
            return Failure(TEXT("unique_reviewed_node_anchors_required"));
        }
    }

    const TArray<FNpcReviewedEdge> Unchanged = {
        {Pin(SharedGet, TEXT("ReturnValue")), Pin(SharedCast, TEXT("Object"))},
        {Pin(GetOne, TEXT("ReturnValue")), Pin(CastOne, TEXT("Object"))},
        {Pin(GetTwo, TEXT("ReturnValue")), Pin(CastTwo, TEXT("Object"))},
        {Pin(SharedCast, TEXT("AsBP Game Mode")), Pin(TimeZero, TEXT("self"))}
    };
    const TArray<FNpcReviewedEdge> Removed = {
        {Pin(Tick, TEXT("then")), Pin(Sequence, TEXT("execute"))},
        {Pin(Sequence, TEXT("then_0")), Pin(SharedCast, TEXT("execute"))},
        {Pin(SharedCast, TEXT("then")), Pin(BranchZero, TEXT("execute"))},
        {Pin(Sequence, TEXT("then_1")), Pin(CastOne, TEXT("execute"))},
        {Pin(CastOne, TEXT("then")), Pin(BranchOne, TEXT("execute"))},
        {Pin(Sequence, TEXT("then_2")), Pin(CastTwo, TEXT("execute"))},
        {Pin(CastTwo, TEXT("then")), Pin(BranchTwo, TEXT("execute"))},
        {Pin(CastOne, TEXT("AsBP Game Mode")), Pin(TimeOne, TEXT("self"))},
        {Pin(CastTwo, TEXT("AsBP Game Mode")), Pin(TimeTwo, TEXT("self"))}
    };
    const TArray<FNpcReviewedEdge> Added = {
        {Pin(Tick, TEXT("then")), Pin(SharedCast, TEXT("execute"))},
        {Pin(SharedCast, TEXT("then")), Pin(Sequence, TEXT("execute"))},
        {Pin(Sequence, TEXT("then_0")), Pin(BranchZero, TEXT("execute"))},
        {Pin(Sequence, TEXT("then_1")), Pin(BranchOne, TEXT("execute"))},
        {Pin(Sequence, TEXT("then_2")), Pin(BranchTwo, TEXT("execute"))},
        {Pin(SharedCast, TEXT("AsBP Game Mode")), Pin(TimeOne, TEXT("self"))},
        {Pin(SharedCast, TEXT("AsBP Game Mode")), Pin(TimeTwo, TEXT("self"))}
    };
    TSet<UEdGraphPin*> Observed;
    for (const TArray<FNpcReviewedEdge>* Edges : {&Unchanged, &Removed, &Added})
    {
        for (const FNpcReviewedEdge& Edge : *Edges)
        {
            Observed.Add(Edge.Output);
            Observed.Add(Edge.Input);
        }
    }
    for (UEdGraphNode* Cast : {SharedCast, CastOne, CastTwo})
    {
        Observed.Add(Pin(Cast, TEXT("CastFailed")));
    }
    TArray<FNpcReviewedEdge> OriginalTopology = Unchanged;
    OriginalTopology.Append(Removed);
    TArray<FNpcReviewedEdge> OptimizedTopology = Unchanged;
    OptimizedTopology.Append(Added);
    if (Matches(OptimizedTopology, Observed))
    {
        TSharedRef<FJsonObject> Result = Response(true);
        Result->SetStringField(TEXT("status"), TEXT("already_optimized_reviewed_topology"));
        Result->SetBoolField(TEXT("compiled_this_call"), false);
        return Encode(Result);
    }
    if (!Matches(OriginalTopology, Observed))
    {
        return Failure(TEXT("connections_do_not_match_reviewed_initial_topology"));
    }

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    for (const FNpcReviewedEdge& Edge : Added)
    {
        const ECanCreateConnectionResponse ResponseCode = Schema->CanCreateConnection(Edge.Output, Edge.Input).Response;
        if (ResponseCode != CONNECT_RESPONSE_MAKE && ResponseCode != CONNECT_RESPONSE_BREAK_OTHERS_A
            && ResponseCode != CONNECT_RESPONSE_BREAK_OTHERS_B && ResponseCode != CONNECT_RESPONSE_BREAK_OTHERS_AB)
        {
            return Failure(TEXT("schema_preflight_rejected_direct_connection"));
        }
    }
    // Compilation can refresh graph pins, so capture the audit records while
    // every reviewed pin pointer is still valid, before mutation/compilation.
    const TArray<TSharedPtr<FJsonValue>> RemovedLog = EdgeLog(Removed);
    const TArray<TSharedPtr<FJsonValue>> AddedLog = EdgeLog(Added);

    // All GUID, class, graph, pin, reciprocal-link and schema checks above are
    // complete before the first mutation. No new node or gameplay variable.
    Blueprint->Modify();
    Graph->Modify();
    for (UEdGraphNode* Anchor : Anchors)
    {
        Anchor->Modify();
    }
    for (const FNpcReviewedEdge& Edge : Removed)
    {
        Edge.Output->BreakLinkTo(Edge.Input);
    }
    bool bConnected = true;
    for (const FNpcReviewedEdge& Edge : Added)
    {
        bConnected &= Schema->TryCreateConnection(Edge.Output, Edge.Input);
    }
    if (!bConnected || !Matches(OptimizedTopology, Observed))
    {
        for (const FNpcReviewedEdge& Edge : Added)
        {
            Edge.Output->BreakLinkTo(Edge.Input);
        }
        for (const FNpcReviewedEdge& Edge : Removed)
        {
            Schema->TryCreateConnection(Edge.Output, Edge.Input);
        }
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        TSharedRef<FJsonObject> Result = Response(false);
        Result->SetStringField(TEXT("error"), TEXT("connection_rejected_do_not_save"));
        Result->SetBoolField(TEXT("original_topology_restored"), Matches(OriginalTopology, Observed));
        return Encode(Result);
    }
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Compilation;
    const FString CompilationText = UNpcAuditLibrary::CompileBlueprint(Blueprint);
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CompilationText);
    bool bCompileOk = false;
    const bool bParsed = FJsonSerializer::Deserialize(Reader, Compilation) && Compilation.IsValid();
    bCompileOk = bParsed && Compilation->TryGetBoolField(TEXT("ok"), bCompileOk) && bCompileOk;
    TSharedRef<FJsonObject> Result = Response(bCompileOk);
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetStringField(TEXT("status"), bCompileOk ? TEXT("candidate_compiled_in_memory") : TEXT("compile_failed_do_not_save"));
    Result->SetBoolField(TEXT("changed"), true);
    Result->SetBoolField(TEXT("compiled_this_call"), true);
    Result->SetBoolField(TEXT("sequence_branch_order_preserved"), true);
    Result->SetBoolField(TEXT("per_frame_evaluation_preserved"), true);
    Result->SetBoolField(TEXT("interact_lookup_untouched"), true);
    Result->SetBoolField(TEXT("new_gameplay_variables"), false);
    Result->SetNumberField(TEXT("initial_tick_lookup_and_cast_pairs"), 3);
    Result->SetNumberField(TEXT("candidate_tick_lookup_and_cast_pairs"), 1);
    Result->SetArrayField(TEXT("removed_edges"), RemovedLog);
    Result->SetArrayField(TEXT("added_edges"), AddedLog);
    Result->SetStringField(TEXT("performance_claim"), TEXT("None. Adopt only after same-scenario functional and performance measurements."));
    if (bParsed)
    {
        Result->SetObjectField(TEXT("compilation"), Compilation);
    }
    else
    {
        Result->SetStringField(TEXT("compilation_parse_error"), TEXT("actual_compile_result_was_not_valid_json"));
    }
    return Encode(Result);
}
