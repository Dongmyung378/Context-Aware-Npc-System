#include "NpcAuditLibrary.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Internationalization/Regex.h"
#include "JsonObjectConverter.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace NpcAudit
{
    static bool IsSensitive(const FString& Name)
    {
        const FString Lower = Name.ToLower();
        return Lower.Contains(TEXT("password")) || Lower.Contains(TEXT("secret"))
            || Lower.Contains(TEXT("token")) || Lower.Contains(TEXT("credential"))
            || Lower.Contains(TEXT("authorization")) || Lower.Contains(TEXT("api_key"))
            || Lower.Contains(TEXT("apikey"));
    }

    static TSharedRef<FJsonObject> Response(const FString& Operation, bool bOk)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("schema"), TEXT("npc-audit/v1"));
        Result->SetStringField(TEXT("operation"), Operation);
        Result->SetBoolField(TEXT("ok"), bOk);
        return Result;
    }

    static FString Encode(const TSharedRef<FJsonObject>& Object)
    {
        FString Result;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
        FJsonSerializer::Serialize(Object, Writer);
        return Result;
    }

    static FString ErrorResponse(const FString& Operation, const FString& Code, const FString& Field = FString())
    {
        TSharedRef<FJsonObject> Result = Response(Operation, false);
        Result->SetStringField(TEXT("error"), Code);
        if (!Field.IsEmpty())
        {
            Result->SetStringField(TEXT("field"), Field);
        }
        return Encode(Result);
    }

    static FString Flags(uint64 Value)
    {
        return FString::Printf(TEXT("0x%016llx"), static_cast<unsigned long long>(Value));
    }

    static TSharedPtr<FJsonValue> SafeExport(FProperty* Property, const void* Address)
    {
        if (IsSensitive(Property->GetName()))
        {
            return MakeShared<FJsonValueString>(TEXT("[redacted]"));
        }
        // Do not recursively expand instanced components, actors, or other objects.
        if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
        {
            const UObject* Value = ObjectProperty->GetObjectPropertyValue(Address);
            if (Value)
            {
                return MakeShared<FJsonValueString>(Value->GetPathName());
            }
            return MakeShared<FJsonValueNull>();
        }
        // JSON doubles cannot losslessly represent all reflected 64-bit integers.
        if (const FInt64Property* Integer = CastField<FInt64Property>(Property))
        {
            return MakeShared<FJsonValueString>(LexToString(Integer->GetPropertyValue(Address)));
        }
        if (const FUInt64Property* Integer = CastField<FUInt64Property>(Property))
        {
            return MakeShared<FJsonValueString>(LexToString(Integer->GetPropertyValue(Address)));
        }
        return nullptr;
    }

    static TSharedPtr<FJsonValue> PropertyValue(FProperty* Property, const void* Address)
    {
        FJsonObjectConverter::CustomExportCallback Callback;
        Callback.BindStatic(&SafeExport);
        TSharedPtr<FJsonValue> Result = FJsonObjectConverter::UPropertyToJsonValue(
            Property, Address, 0, 0, &Callback, nullptr,
            EJsonObjectConversionFlags::SkipStandardizeCase);
        return Result.IsValid() ? Result : MakeShared<FJsonValueNull>();
    }

    static TSharedRef<FJsonObject> DescribeProperty(FProperty* Property, const void* Container)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("name"), Property->GetName());
        Result->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
        Result->SetStringField(TEXT("field_class"), Property->GetClass()->GetName());
        Result->SetStringField(TEXT("owner"), Property->GetOwnerStruct()->GetPathName());
        Result->SetStringField(TEXT("flags"), Flags(Property->GetPropertyFlags()));
        Result->SetNumberField(TEXT("array_dim"), Property->ArrayDim);
        Result->SetBoolField(TEXT("redacted"), IsSensitive(Property->GetName()));
        Result->SetStringField(TEXT("display_name"), Property->GetDisplayNameText().ToString());
        Result->SetStringField(TEXT("category"), Property->GetMetaData(TEXT("Category")));
        if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            Result->SetStringField(TEXT("struct"), StructProperty->Struct->GetPathName());
        }
        if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
        {
            Result->SetStringField(TEXT("enum"), EnumProperty->GetEnum()->GetPathName());
        }
        if (Container)
        {
            Result->SetField(TEXT("value"), PropertyValue(
                Property, Property->ContainerPtrToValuePtr<void>(Container)));
        }
        return Result;
    }

    static TSharedRef<FJsonObject> DescribeFunction(UFunction* Function)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("name"), Function->GetName());
        Result->SetStringField(TEXT("owner"), Function->GetOuter()->GetPathName());
        Result->SetStringField(TEXT("flags"), Flags(Function->FunctionFlags));
        Result->SetNumberField(TEXT("parameter_bytes"), Function->ParmsSize);
        Result->SetBoolField(TEXT("latent"), Function->HasMetaData(TEXT("Latent")));
        Result->SetStringField(TEXT("world_context"), Function->GetMetaData(TEXT("WorldContext")));
        TArray<TSharedPtr<FJsonValue>> Parameters;
        for (TFieldIterator<FProperty> It(Function, EFieldIteratorFlags::ExcludeSuper); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property->HasAnyPropertyFlags(CPF_Parm))
            {
                continue;
            }
            TSharedRef<FJsonObject> Parameter = DescribeProperty(Property, nullptr);
            const bool bReturn = Property->HasAnyPropertyFlags(CPF_ReturnParm);
            const bool bOut = Property->HasAnyPropertyFlags(CPF_OutParm);
            const bool bReference = Property->HasAnyPropertyFlags(CPF_ReferenceParm);
            Parameter->SetBoolField(TEXT("input"), !bReturn && (!bOut || bReference));
            Parameter->SetBoolField(TEXT("output"), bReturn || bOut);
            Parameter->SetBoolField(TEXT("return"), bReturn);
            const FString DefaultKey = TEXT("CPP_Default_") + Property->GetName();
            if (Function->HasMetaData(*DefaultKey) && !IsSensitive(Property->GetName()))
            {
                Parameter->SetStringField(TEXT("cpp_default"), Function->GetMetaData(*DefaultKey));
            }
            Parameters.Add(MakeShared<FJsonValueObject>(Parameter));
        }
        Result->SetArrayField(TEXT("parameters"), Parameters);
        return Result;
    }

    static TSharedRef<FJsonObject> DescribeClass(UClass* Class, bool bIncludeInherited)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("path"), Class->GetPathName());
        Result->SetBoolField(TEXT("blueprint_generated"), Class->ClassGeneratedBy != nullptr);
        TArray<TSharedPtr<FJsonValue>> Hierarchy;
        for (UClass* Owner = Class; Owner; Owner = bIncludeInherited ? Owner->GetSuperClass() : nullptr)
        {
            TSharedRef<FJsonObject> Declared = MakeShared<FJsonObject>();
            Declared->SetStringField(TEXT("class"), Owner->GetPathName());
            TArray<TSharedPtr<FJsonValue>> Properties;
            for (TFieldIterator<FProperty> It(Owner, EFieldIteratorFlags::ExcludeSuper); It; ++It)
            {
                Properties.Add(MakeShared<FJsonValueObject>(DescribeProperty(*It, nullptr)));
            }
            TArray<TSharedPtr<FJsonValue>> Functions;
            for (TFieldIterator<UFunction> It(Owner, EFieldIteratorFlags::ExcludeSuper); It; ++It)
            {
                Functions.Add(MakeShared<FJsonValueObject>(DescribeFunction(*It)));
            }
            Declared->SetArrayField(TEXT("properties"), Properties);
            Declared->SetArrayField(TEXT("functions"), Functions);
            Hierarchy.Add(MakeShared<FJsonValueObject>(Declared));
        }
        Result->SetArrayField(TEXT("hierarchy"), Hierarchy);
        return Result;
    }

    static bool ParseValue(const FString& Json, TSharedPtr<FJsonValue>& Value)
    {
        if (Json.Len() > 1024 * 1024)
        {
            return false;
        }
        // UE 5.4's root-value deserializer only accepts objects and arrays.
        // A one-element envelope also handles numeric, boolean and null fixtures.
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(
            TEXT("[") + Json + TEXT("]"));
        TArray<TSharedPtr<FJsonValue>> Envelope;
        if (!FJsonSerializer::Deserialize(Reader, Envelope) || Envelope.Num() != 1)
        {
            return false;
        }
        Value = Envelope[0];
        return Value.IsValid();
    }

    static bool ImportValue(FProperty* Property, void* Address, const TSharedPtr<FJsonValue>& Value)
    {
        if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
        {
            if (Value->Type == EJson::Null)
            {
                ObjectProperty->SetObjectPropertyValue(Address, nullptr);
                return true;
            }
            FString Path;
            if (!Value->TryGetString(Path))
            {
                return false;
            }
            UObject* Reference = StaticFindObject(UObject::StaticClass(), nullptr, *Path);
            if (!Reference)
            {
                Reference = LoadObject<UObject>(nullptr, *Path);
            }
            if (!Reference || !Reference->IsA(ObjectProperty->PropertyClass))
            {
                return false;
            }
            if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
            {
                const UClass* ReferencedClass = Cast<UClass>(Reference);
                if (!ReferencedClass || !ReferencedClass->IsChildOf(ClassProperty->MetaClass))
                {
                    return false;
                }
            }
            ObjectProperty->SetObjectPropertyValue(Address, Reference);
            return true;
        }
        FText Reason;
        return FJsonObjectConverter::JsonValueToUProperty(Value, Property, Address, 0, 0, true, &Reason);
    }
}

FString UNpcAuditLibrary::DumpObject(UObject* Object, bool bIncludeInherited)
{
    using namespace NpcAudit;
    if (!IsInGameThread() || !IsValid(Object))
    {
        return ErrorResponse(TEXT("dump_object"), TEXT("valid_object_on_game_thread_required"));
    }
    TSharedRef<FJsonObject> Result = Response(TEXT("dump_object"), true);
    Result->SetStringField(TEXT("object"), Object->GetPathName());
    Result->SetObjectField(TEXT("class"), DescribeClass(Object->GetClass(), bIncludeInherited));
    Result->SetBoolField(TEXT("class_default_object"), Object->HasAnyFlags(RF_ClassDefaultObject));
    TArray<TSharedPtr<FJsonValue>> State;
    for (TFieldIterator<FProperty> It(Object->GetClass(), bIncludeInherited
        ? EFieldIteratorFlags::IncludeSuper : EFieldIteratorFlags::ExcludeSuper); It; ++It)
    {
        State.Add(MakeShared<FJsonValueObject>(DescribeProperty(*It, Object)));
    }
    Result->SetArrayField(TEXT("state"), State);
    return Encode(Result);
}

FString UNpcAuditLibrary::DumpClass(UClass* Class, bool bIncludeInherited)
{
    using namespace NpcAudit;
    if (!IsInGameThread() || !IsValid(Class))
    {
        return ErrorResponse(TEXT("dump_class"), TEXT("valid_class_on_game_thread_required"));
    }
    TSharedRef<FJsonObject> Result = Response(TEXT("dump_class"), true);
    Result->SetObjectField(TEXT("class"), DescribeClass(Class, bIncludeInherited));
    return Encode(Result);
}

FString UNpcAuditLibrary::ReadProperty(UObject* Object, const FString& PropertyName)
{
    using namespace NpcAudit;
    if (!IsInGameThread() || !IsValid(Object))
    {
        return ErrorResponse(TEXT("read_property"), TEXT("valid_object_on_game_thread_required"));
    }
    FProperty* Property = Object->GetClass()->FindPropertyByName(FName(*PropertyName));
    if (!Property)
    {
        return ErrorResponse(TEXT("read_property"), TEXT("property_not_found"), PropertyName);
    }
    TSharedRef<FJsonObject> Result = Response(TEXT("read_property"), true);
    Result->SetStringField(TEXT("object"), Object->GetPathName());
    Result->SetObjectField(TEXT("property"), DescribeProperty(Property, Object));
    return Encode(Result);
}

FString UNpcAuditLibrary::WriteProperty(UObject* Object, const FString& PropertyName, const FString& ValueJson)
{
    using namespace NpcAudit;
    if (!IsInGameThread() || !IsValid(Object))
    {
        return ErrorResponse(TEXT("write_property"), TEXT("valid_object_on_game_thread_required"));
    }
    FProperty* Property = Object->GetClass()->FindPropertyByName(FName(*PropertyName));
    if (!Property || IsSensitive(PropertyName))
    {
        return ErrorResponse(TEXT("write_property"), TEXT("property_unavailable"), PropertyName);
    }
    TSharedPtr<FJsonValue> Value;
    if (!ParseValue(ValueJson, Value))
    {
        return ErrorResponse(TEXT("write_property"), TEXT("invalid_or_oversized_json"));
    }
    FDefaultConstructedPropertyElement Temporary(Property);
    void* Destination = Property->ContainerPtrToValuePtr<void>(Object);
    Property->CopyCompleteValue(Temporary.GetObjAddress(), Destination);
    if (!ImportValue(Property, Temporary.GetObjAddress(), Value))
    {
        return ErrorResponse(TEXT("write_property"), TEXT("property_conversion_failed"), PropertyName);
    }
    Property->CopyCompleteValue(Destination, Temporary.GetObjAddress());
    TSharedRef<FJsonObject> Result = Response(TEXT("write_property"), true);
    Result->SetObjectField(TEXT("property"), DescribeProperty(Property, Object));
    Result->SetBoolField(TEXT("asset_saved"), false);
    return Encode(Result);
}

FString UNpcAuditLibrary::Invoke(UObject* Object, const FString& FunctionName, const FString& ArgumentsJson)
{
    using namespace NpcAudit;
    if (!IsInGameThread() || !IsValid(Object))
    {
        return ErrorResponse(TEXT("invoke"), TEXT("valid_object_on_game_thread_required"));
    }
    UFunction* Function = Object->FindFunction(FName(*FunctionName));
    if (!Function)
    {
        return ErrorResponse(TEXT("invoke"), TEXT("function_not_found"), FunctionName);
    }
    if (Function->HasMetaData(TEXT("Latent")))
    {
        return ErrorResponse(TEXT("invoke"), TEXT("latent_function_requires_ticked_harness"), FunctionName);
    }
    TSharedPtr<FJsonValue> Parsed;
    if (!ParseValue(ArgumentsJson, Parsed) || Parsed->Type != EJson::Object)
    {
        return ErrorResponse(TEXT("invoke"), TEXT("json_object_required"));
    }
    const TSharedPtr<FJsonObject> Arguments = Parsed->AsObject();
    TMap<FString, FProperty*> Inputs;
    for (TFieldIterator<FProperty> It(Function, EFieldIteratorFlags::ExcludeSuper); It; ++It)
    {
        FProperty* Property = *It;
        if (Property->HasAnyPropertyFlags(CPF_Parm)
            && !Property->HasAnyPropertyFlags(CPF_ReturnParm)
            && (!Property->HasAnyPropertyFlags(CPF_OutParm) || Property->HasAnyPropertyFlags(CPF_ReferenceParm)))
        {
            Inputs.Add(Property->GetName(), Property);
        }
    }
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Argument : Arguments->Values)
    {
        if (!Inputs.Contains(Argument.Key) || IsSensitive(Argument.Key))
        {
            return ErrorResponse(TEXT("invoke"), TEXT("unknown_or_unavailable_argument"), Argument.Key);
        }
    }
    FStructOnScope Parameters(Function);
    for (const TPair<FString, FProperty*>& Input : Inputs)
    {
        FProperty* Property = Input.Value;
        void* Address = Property->ContainerPtrToValuePtr<void>(Parameters.GetStructMemory());
        const TSharedPtr<FJsonValue>* Supplied = Arguments->Values.Find(Input.Key);
        if (Supplied)
        {
            if (!ImportValue(Property, Address, *Supplied))
            {
                return ErrorResponse(TEXT("invoke"), TEXT("argument_conversion_failed"), Input.Key);
            }
        }
        else
        {
            const FString DefaultKey = TEXT("CPP_Default_") + Input.Key;
            if (!Function->HasMetaData(*DefaultKey)
                || !Property->ImportText_Direct(*Function->GetMetaData(*DefaultKey), Address, Object, PPF_None))
            {
                return ErrorResponse(TEXT("invoke"), TEXT("missing_required_argument"), Input.Key);
            }
        }
    }
    Object->ProcessEvent(Function, Parameters.GetStructMemory());
    TSharedRef<FJsonObject> Result = Response(TEXT("invoke"), true);
    Result->SetStringField(TEXT("function"), FunctionName);
    Result->SetStringField(TEXT("execution"), TEXT("synchronous_dispatch_completed"));
    Result->SetBoolField(TEXT("latent_completion_observed"), false);
    TSharedRef<FJsonObject> Output = MakeShared<FJsonObject>();
    for (TFieldIterator<FProperty> It(Function, EFieldIteratorFlags::ExcludeSuper); It; ++It)
    {
        FProperty* Property = *It;
        if (Property->HasAnyPropertyFlags(CPF_Parm))
        {
            Output->SetField(Property->GetName(), PropertyValue(Property,
                Property->ContainerPtrToValuePtr<void>(Parameters.GetStructMemory())));
        }
    }
    Result->SetObjectField(TEXT("parameters"), Output);
    return Encode(Result);
}

FString UNpcAuditLibrary::CompileBlueprint(UObject* BlueprintAsset)
{
    using namespace NpcAudit;
    UBlueprint* Blueprint = Cast<UBlueprint>(BlueprintAsset);
    if (!IsInGameThread() || !IsValid(Blueprint))
    {
        return ErrorResponse(TEXT("compile_blueprint"), TEXT("valid_blueprint_on_game_thread_required"));
    }
    FCompilerResultsLog Results;
    Results.bSilentMode = true;
    FKismetEditorUtilities::CompileBlueprint(Blueprint,
        EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &Results);
    TSharedRef<FJsonObject> Result = Response(TEXT("compile_blueprint"),
        Results.NumErrors == 0 && Blueprint->Status != BS_Error);
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetNumberField(TEXT("status"), static_cast<int32>(Blueprint->Status));
    Result->SetNumberField(TEXT("errors"), Results.NumErrors);
    Result->SetNumberField(TEXT("warnings"), Results.NumWarnings);
    Result->SetBoolField(TEXT("asset_saved"), false);
    TArray<TSharedPtr<FJsonValue>> Messages;
    for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetNumberField(TEXT("severity"), static_cast<int32>(Message->GetSeverity()));
        Item->SetStringField(TEXT("message"), Message->ToText().ToString());
        Messages.Add(MakeShared<FJsonValueObject>(Item));
    }
    Result->SetArrayField(TEXT("messages"), Messages);
    return Encode(Result);
}

FString UNpcAuditLibrary::DumpBlueprintGraphs(UObject* BlueprintAsset)
{
    using namespace NpcAudit;
    UBlueprint* Blueprint = Cast<UBlueprint>(BlueprintAsset);
    if (!IsInGameThread() || !IsValid(Blueprint))
    {
        return ErrorResponse(TEXT("dump_blueprint_graphs"), TEXT("valid_blueprint_on_game_thread_required"));
    }

    int32 RedactedTextFields = 0;
    auto Sanitize = [&RedactedTextFields](const FString& Input) -> FString
    {
        FString Result = Input;
        // Asset paths (/Game, /Engine, /Script) remain useful. Local OS paths do not.
        const TCHAR* Patterns[] = {
            TEXT("[A-Za-z]:[\\\\/][^\\r\\n\"<>|]*"),
            TEXT("/(?:Users|home|mnt|var|tmp)/[^\\r\\n\"<>|]*")
        };
        for (const TCHAR* PatternText : Patterns)
        {
            FRegexMatcher Matcher(FRegexPattern(PatternText), Result);
            FString Replaced;
            int32 Start = 0;
            bool bMatched = false;
            while (Matcher.FindNext())
            {
                Replaced += Result.Mid(Start, Matcher.GetMatchBeginning() - Start);
                Replaced += TEXT("[private-path]");
                Start = Matcher.GetMatchEnding();
                bMatched = true;
            }
            if (bMatched)
            {
                Replaced += Result.Mid(Start);
                Result = MoveTemp(Replaced);
            }
        }
        if (Result != Input)
        {
            ++RedactedTextFields;
        }
        return Result;
    };

    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);
    TSet<const UEdGraph*> Visited;
    TArray<TSharedPtr<FJsonValue>> Graphs;
    int32 NodeCount = 0;
    int32 PinCount = 0;
    int32 DirectedLinkCount = 0;
    int32 InvalidLinkCount = 0;
    int32 InvalidGraphCount = 0;
    int32 InvalidNodeCount = 0;
    for (UEdGraph* Graph : AllGraphs)
    {
        if (!IsValid(Graph))
        {
            ++InvalidGraphCount;
            continue;
        }
        if (Visited.Contains(Graph))
        {
            continue;
        }
        Visited.Add(Graph);
        TSharedRef<FJsonObject> GraphJson = MakeShared<FJsonObject>();
        GraphJson->SetStringField(TEXT("name"), Sanitize(Graph->GetName()));
        GraphJson->SetStringField(TEXT("path"), Sanitize(Graph->GetPathName()));
        GraphJson->SetStringField(TEXT("guid"), Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphens));
        TArray<TSharedPtr<FJsonValue>> Nodes;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!IsValid(Node))
            {
                ++InvalidNodeCount;
                continue;
            }
            ++NodeCount;
            TSharedRef<FJsonObject> NodeJson = MakeShared<FJsonObject>();
            NodeJson->SetStringField(TEXT("name"), Node->GetName());
            NodeJson->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
            NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
            NodeJson->SetStringField(TEXT("title"), Sanitize(Node->GetNodeTitle(ENodeTitleType::ListView).ToString()));
            NodeJson->SetStringField(TEXT("comment"), Sanitize(Node->NodeComment));
            NodeJson->SetNumberField(TEXT("x"), Node->NodePosX);
            NodeJson->SetNumberField(TEXT("y"), Node->NodePosY);
            if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
            {
                TSharedRef<FJsonObject> Function = MakeShared<FJsonObject>();
                Function->SetStringField(TEXT("name"), Call->FunctionReference.GetMemberName().ToString());
                Function->SetStringField(TEXT("guid"), Call->FunctionReference.GetMemberGuid().ToString(EGuidFormats::DigitsWithHyphens));
                Function->SetBoolField(TEXT("self_context"), Call->FunctionReference.IsSelfContext());
                const UClass* Owner = Call->FunctionReference.GetMemberParentClass(Blueprint->GeneratedClass);
                Function->SetStringField(TEXT("owner"), Owner ? Owner->GetPathName() : FString());
                NodeJson->SetObjectField(TEXT("function_reference"), Function);
            }
            TArray<TSharedPtr<FJsonValue>> Pins;
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin)
                {
                    continue;
                }
                ++PinCount;
                TSharedRef<FJsonObject> PinJson = MakeShared<FJsonObject>();
                PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
                PinJson->SetStringField(TEXT("guid"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
                PinJson->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
                TSharedRef<FJsonObject> Type = MakeShared<FJsonObject>();
                Type->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
                Type->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
                const UObject* SubcategoryObject = Pin->PinType.PinSubCategoryObject.Get();
                Type->SetStringField(TEXT("subcategory_object"), SubcategoryObject ? SubcategoryObject->GetPathName() : FString());
                Type->SetNumberField(TEXT("container_type"), static_cast<int32>(Pin->PinType.ContainerType));
                Type->SetBoolField(TEXT("reference"), Pin->PinType.bIsReference);
                Type->SetBoolField(TEXT("const"), Pin->PinType.bIsConst);
                PinJson->SetObjectField(TEXT("type"), Type);
                const bool bRedacted = IsSensitive(Pin->PinName.ToString());
                PinJson->SetBoolField(TEXT("default_redacted"), bRedacted);
                PinJson->SetStringField(TEXT("default_value"), bRedacted ? TEXT("[redacted]") : Sanitize(Pin->DefaultValue));
                PinJson->SetStringField(TEXT("autogenerated_default"), bRedacted ? TEXT("[redacted]") : Sanitize(Pin->AutogeneratedDefaultValue));
                PinJson->SetStringField(TEXT("default_text"), bRedacted ? TEXT("[redacted]") : Sanitize(Pin->DefaultTextValue.ToString()));
                PinJson->SetStringField(TEXT("default_object"), bRedacted ? TEXT("[redacted]")
                    : (Pin->DefaultObject ? Sanitize(Pin->DefaultObject->GetPathName()) : FString()));
                PinJson->SetBoolField(TEXT("hidden"), Pin->bHidden);
                PinJson->SetBoolField(TEXT("orphaned"), Pin->bOrphanedPin);
                PinJson->SetStringField(TEXT("parent_pin_guid"), Pin->ParentPin
                    ? Pin->ParentPin->PinId.ToString(EGuidFormats::DigitsWithHyphens) : FString());
                TArray<TSharedPtr<FJsonValue>> Links;
                for (const UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    const UEdGraphNode* LinkedNode = Linked ? Linked->GetOwningNodeUnchecked() : nullptr;
                    if (!Linked || !IsValid(LinkedNode))
                    {
                        ++InvalidLinkCount;
                        continue;
                    }
                    ++DirectedLinkCount;
                    TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
                    Link->SetStringField(TEXT("node_guid"), LinkedNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
                    Link->SetStringField(TEXT("pin_guid"), Linked->PinId.ToString(EGuidFormats::DigitsWithHyphens));
                    Link->SetStringField(TEXT("pin_name"), Linked->PinName.ToString());
                    Links.Add(MakeShared<FJsonValueObject>(Link));
                }
                PinJson->SetArrayField(TEXT("links"), Links);
                Pins.Add(MakeShared<FJsonValueObject>(PinJson));
            }
            NodeJson->SetArrayField(TEXT("pins"), Pins);
            Nodes.Add(MakeShared<FJsonValueObject>(NodeJson));
        }
        GraphJson->SetArrayField(TEXT("nodes"), Nodes);
        Graphs.Add(MakeShared<FJsonValueObject>(GraphJson));
    }
    TSharedRef<FJsonObject> Result = Response(TEXT("dump_blueprint_graphs"), true);
    Result->SetStringField(TEXT("blueprint"), Sanitize(Blueprint->GetPathName()));
    Result->SetBoolField(TEXT("complete"), InvalidGraphCount == 0 && InvalidNodeCount == 0 && InvalidLinkCount == 0);
    Result->SetBoolField(TEXT("truncated"), false);
    Result->SetBoolField(TEXT("asset_saved"), false);
    Result->SetNumberField(TEXT("graph_count"), Graphs.Num());
    Result->SetNumberField(TEXT("node_count"), NodeCount);
    Result->SetNumberField(TEXT("pin_count"), PinCount);
    Result->SetNumberField(TEXT("directed_link_count"), DirectedLinkCount);
    Result->SetNumberField(TEXT("invalid_link_count"), InvalidLinkCount);
    Result->SetNumberField(TEXT("invalid_graph_count"), InvalidGraphCount);
    Result->SetNumberField(TEXT("invalid_node_count"), InvalidNodeCount);
    Result->SetNumberField(TEXT("redacted_text_fields"), RedactedTextFields);
    Result->SetArrayField(TEXT("graphs"), Graphs);
    return Encode(Result);
}
