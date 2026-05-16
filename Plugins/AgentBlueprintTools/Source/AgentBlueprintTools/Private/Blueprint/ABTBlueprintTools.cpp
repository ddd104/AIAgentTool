#include "Blueprint/ABTBlueprintTools.h"
#include "Utils/ABTJson.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintActionDatabase.h"
#include "Components/ActorComponent.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopedSlowTask.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"
#include "Editor.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace
{
    FEdGraphPinType MakePinTypeFromString(const FString& TypeName)
    {
        FEdGraphPinType PinType;
        if (TypeName.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
        }
        else if (TypeName.Equals(TEXT("float"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("double"), ESearchCase::IgnoreCase))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
            PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
        }
        else if (TypeName.Equals(TEXT("int"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("integer"), ESearchCase::IgnoreCase))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
        }
        else if (TypeName.Equals(TEXT("string"), ESearchCase::IgnoreCase))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_String;
        }
        else if (TypeName.Equals(TEXT("name"), ESearchCase::IgnoreCase))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
        }
        else if (TypeName.Equals(TEXT("text"), ESearchCase::IgnoreCase))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
        }
        else
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
        }
        return PinType;
    }

    FString PinDirectionToString(EEdGraphPinDirection Dir)
    {
        switch (Dir)
        {
            case EGPD_Input: return TEXT("input");
            case EGPD_Output: return TEXT("output");
            default: return TEXT("unknown");
        }
    }

    FString PinTypeToString(const FEdGraphPinType& Type)
    {
        FString Result = Type.PinCategory.ToString();
        if (!Type.PinSubCategory.IsNone())
        {
            Result += TEXT(".") + Type.PinSubCategory.ToString();
        }
        if (Type.PinSubCategoryObject.IsValid())
        {
            Result += TEXT(":") + Type.PinSubCategoryObject->GetName();
        }
        return Result;
    }

    UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName)
    {
        if (!Node) return nullptr;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
            {
                return Pin;
            }
        }
        return nullptr;
    }

    bool ParseNodePinRef(const FString& Ref, FString& OutNodeId, FString& OutPinName)
    {
        FString Left, Right;
        if (!Ref.Split(TEXT("."), &Left, &Right)) return false;
        OutNodeId = Left;
        OutPinName = Right;
        return !OutNodeId.IsEmpty() && !OutPinName.IsEmpty();
    }

    UEdGraphNode* FindExistingNodeByGuidOrName(UEdGraph* Graph, const FString& Id)
    {
        if (!Graph) return nullptr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;
            if (Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens).Equals(Id, ESearchCase::IgnoreCase)) return Node;
            if (Node->GetName().Equals(Id, ESearchCase::IgnoreCase)) return Node;
            if (Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Equals(Id, ESearchCase::IgnoreCase)) return Node;
        }
        return nullptr;
    }

    UFunction* FindFunctionByClassAndName(const FString& ClassName, const FString& FunctionName)
    {
        UClass* OwnerClass = nullptr;
        if (!ClassName.IsEmpty())
        {
            OwnerClass = LoadObject<UClass>(nullptr, *ClassName);
            if (!OwnerClass)
            {
                for (TObjectIterator<UClass> It; It; ++It)
                {
                    UClass* Candidate = *It;
                    if (Candidate && (Candidate->GetName().Equals(ClassName, ESearchCase::IgnoreCase) ||
                        Candidate->GetPathName().Equals(ClassName, ESearchCase::IgnoreCase)))
                    {
                        OwnerClass = Candidate;
                        break;
                    }
                }
            }
        }

        if (OwnerClass)
        {
            return OwnerClass->FindFunctionByName(*FunctionName);
        }

        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            if (!Class) continue;
            if (UFunction* Function = Class->FindFunctionByName(*FunctionName))
            {
                return Function;
            }
        }
        return nullptr;
    }

    UClass* ResolveClassByNameOrPath(const FString& ClassNameOrPath, UClass* RequiredParent)
    {
        if (ClassNameOrPath.IsEmpty())
        {
            return nullptr;
        }

        UClass* Class = LoadObject<UClass>(nullptr, *ClassNameOrPath);
        if (!Class && !ClassNameOrPath.Contains(TEXT(".")) && !ClassNameOrPath.Contains(TEXT("/")))
        {
            Class = LoadObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *ClassNameOrPath));
        }
        if (!Class)
        {
            for (TObjectIterator<UClass> It; It; ++It)
            {
                UClass* Candidate = *It;
                if (Candidate && (Candidate->GetName().Equals(ClassNameOrPath, ESearchCase::IgnoreCase) ||
                    Candidate->GetPathName().Equals(ClassNameOrPath, ESearchCase::IgnoreCase)))
                {
                    Class = Candidate;
                    break;
                }
            }
        }

        return Class && (!RequiredParent || Class->IsChildOf(RequiredParent)) ? Class : nullptr;
    }

    USCS_Node* FindSCSNodeByName(UBlueprint* Blueprint, const FString& ComponentName)
    {
        if (!Blueprint || !Blueprint->SimpleConstructionScript || ComponentName.IsEmpty())
        {
            return nullptr;
        }
        return Blueprint->SimpleConstructionScript->FindSCSNode(*ComponentName);
    }

    UEdGraphPin* FindFirstPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const FName& Category)
    {
        if (!Node) return nullptr;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == Direction && Pin->PinType.PinCategory == Category)
            {
                return Pin;
            }
        }
        return nullptr;
    }

    UEdGraphPin* FindVectorInputPin(UEdGraphNode* Node)
    {
        if (!Node) return nullptr;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input &&
                Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct &&
                Pin->PinType.PinSubCategoryObject == TBaseStructure<FVector>::Get())
            {
                return Pin;
            }
        }
        return nullptr;
    }

    FString VectorDefaultString(const FVector& Value)
    {
        return FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"), Value.X, Value.Y, Value.Z);
    }

    FVector ReadVectorFromOp(const TSharedPtr<FJsonObject>& Op, const FString& Field, const FVector& DefaultValue)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Op.IsValid() || !Op->TryGetArrayField(Field, Values) || !Values || Values->Num() < 3)
        {
            return DefaultValue;
        }
        return FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
    }
}

UBlueprint* FABTBlueprintTools::LoadBlueprint(const FString& AssetPath, FString& OutError)
{
    UObject* Object = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Object)
    {
        OutError = FString::Printf(TEXT("Could not load asset: %s"), *AssetPath);
        return nullptr;
    }

    UBlueprint* Blueprint = Cast<UBlueprint>(Object);
    if (!Blueprint)
    {
        OutError = FString::Printf(TEXT("Asset is not a Blueprint: %s"), *AssetPath);
        return nullptr;
    }

    return Blueprint;
}

UEdGraph* FABTBlueprintTools::FindGraph(UBlueprint* Blueprint, const FString& GraphName)
{
    if (!Blueprint) return nullptr;
    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
        {
            return Graph;
        }
    }
    return nullptr;
}

UEdGraph* FABTBlueprintTools::EnsureFunctionGraph(UBlueprint* Blueprint, const FString& GraphName)
{
    if (UEdGraph* Existing = FindGraph(Blueprint, GraphName))
    {
        return Existing;
    }

    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint,
        *GraphName,
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());

    FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, NewGraph, false, nullptr);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    return NewGraph;
}

TSharedPtr<FJsonObject> FABTBlueprintTools::ExportPin(UEdGraphPin* Pin)
{
    TSharedPtr<FJsonObject> Json = ABTJson::Object();
    if (!Pin) return Json;

    Json->SetStringField(TEXT("id"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
    Json->SetStringField(TEXT("name"), Pin->PinName.ToString());
    Json->SetStringField(TEXT("direction"), PinDirectionToString(Pin->Direction));
    Json->SetStringField(TEXT("type"), PinTypeToString(Pin->PinType));
    Json->SetStringField(TEXT("default"), Pin->DefaultValue);

    TArray<TSharedPtr<FJsonValue>> Links;
    for (UEdGraphPin* Linked : Pin->LinkedTo)
    {
        if (Linked && Linked->GetOwningNode())
        {
            TSharedPtr<FJsonObject> Link = ABTJson::Object();
            Link->SetStringField(TEXT("node"), Linked->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
            Link->SetStringField(TEXT("pin"), Linked->PinName.ToString());
            Links.Add(ABTJson::ObjectValue(Link));
        }
    }
    Json->SetArrayField(TEXT("links"), Links);
    return Json;
}

TSharedPtr<FJsonObject> FABTBlueprintTools::ExportNode(UEdGraphNode* Node)
{
    TSharedPtr<FJsonObject> Json = ABTJson::Object();
    if (!Node) return Json;

    Json->SetStringField(TEXT("id"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    Json->SetStringField(TEXT("object_name"), Node->GetName());
    Json->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
    Json->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
    Json->SetNumberField(TEXT("x"), Node->NodePosX);
    Json->SetNumberField(TEXT("y"), Node->NodePosY);

    if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
    {
        Json->SetStringField(TEXT("kind"), TEXT("CallFunction"));
        Json->SetStringField(TEXT("function"), Call->FunctionReference.GetMemberName().ToString());
    }
    else if (Cast<UK2Node_IfThenElse>(Node))
    {
        Json->SetStringField(TEXT("kind"), TEXT("Branch"));
    }
    else if (const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node))
    {
        Json->SetStringField(TEXT("kind"), TEXT("GetVariable"));
        Json->SetStringField(TEXT("variable"), Get->GetVarNameString());
    }
    else if (const UK2Node_VariableSet* Set = Cast<UK2Node_VariableSet>(Node))
    {
        Json->SetStringField(TEXT("kind"), TEXT("SetVariable"));
        Json->SetStringField(TEXT("variable"), Set->GetVarNameString());
    }
    else if (const UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Node))
    {
        Json->SetStringField(TEXT("kind"), TEXT("CustomEvent"));
        Json->SetStringField(TEXT("event"), Custom->CustomFunctionName.ToString());
    }
    else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
    {
        Json->SetStringField(TEXT("kind"), TEXT("Event"));
        Json->SetStringField(TEXT("event"), Event->EventReference.GetMemberName().ToString());
    }
    else if (Cast<UK2Node_FunctionEntry>(Node))
    {
        Json->SetStringField(TEXT("kind"), TEXT("FunctionEntry"));
    }
    else
    {
        Json->SetStringField(TEXT("kind"), TEXT("Node"));
    }

    TArray<TSharedPtr<FJsonValue>> Pins;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        Pins.Add(ABTJson::ObjectValue(ExportPin(Pin)));
    }
    Json->SetArrayField(TEXT("pins"), Pins);
    return Json;
}

TSharedPtr<FJsonObject> FABTBlueprintTools::ExportGraph(UEdGraph* Graph)
{
    TSharedPtr<FJsonObject> Json = ABTJson::Object();
    if (!Graph) return Json;

    Json->SetStringField(TEXT("name"), Graph->GetName());
    Json->SetStringField(TEXT("schema"), Graph->Schema ? Graph->Schema->GetClass()->GetPathName() : TEXT(""));

    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        Nodes.Add(ABTJson::ObjectValue(ExportNode(Node)));
    }
    Json->SetArrayField(TEXT("nodes"), Nodes);
    return Json;
}

bool FABTBlueprintTools::ExportBlueprint(const FString& AssetPath, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    UBlueprint* Blueprint = LoadBlueprint(AssetPath, OutError);
    if (!Blueprint) return false;

    OutJson = ABTJson::Ok();
    OutJson->SetStringField(TEXT("asset_path"), AssetPath);
    OutJson->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    OutJson->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
    OutJson->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : TEXT(""));

    TArray<TSharedPtr<FJsonValue>> Variables;
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        TSharedPtr<FJsonObject> V = ABTJson::Object();
        V->SetStringField(TEXT("name"), Var.VarName.ToString());
        V->SetStringField(TEXT("type"), PinTypeToString(Var.VarType));
        V->SetStringField(TEXT("default"), Var.DefaultValue);
        Variables.Add(ABTJson::ObjectValue(V));
    }
    OutJson->SetArrayField(TEXT("variables"), Variables);

    TArray<TSharedPtr<FJsonValue>> Components;
    if (Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (!Node) continue;
            TSharedPtr<FJsonObject> C = ABTJson::Object();
            C->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
            C->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetPathName() : TEXT(""));
            if (UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Node->ComponentTemplate))
            {
                C->SetStringField(TEXT("static_mesh"), MeshComponent->GetStaticMesh() ? MeshComponent->GetStaticMesh()->GetPathName() : TEXT(""));
            }
            if (UMeshComponent* MeshComponent = Cast<UMeshComponent>(Node->ComponentTemplate))
            {
                TArray<TSharedPtr<FJsonValue>> Materials;
                for (int32 Index = 0; Index < MeshComponent->GetNumMaterials(); ++Index)
                {
                    UMaterialInterface* Material = MeshComponent->GetMaterial(Index);
                    Materials.Add(ABTJson::StringValue(Material ? Material->GetPathName() : FString()));
                }
                C->SetArrayField(TEXT("materials"), Materials);
            }
            Components.Add(ABTJson::ObjectValue(C));
        }
    }
    OutJson->SetArrayField(TEXT("components"), Components);

    TArray<UEdGraph*> GraphsRaw;
    Blueprint->GetAllGraphs(GraphsRaw);
    TArray<TSharedPtr<FJsonValue>> Graphs;
    for (UEdGraph* Graph : GraphsRaw)
    {
        Graphs.Add(ABTJson::ObjectValue(ExportGraph(Graph)));
    }
    OutJson->SetArrayField(TEXT("graphs"), Graphs);

    TSharedPtr<FJsonObject> Summary = ABTJson::Object();
    TArray<TSharedPtr<FJsonValue>> EntryPoints;
    TArray<TSharedPtr<FJsonValue>> Reads;
    TArray<TSharedPtr<FJsonValue>> Writes;
    TArray<TSharedPtr<FJsonValue>> Calls;
    for (UEdGraph* Graph : GraphsRaw)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node)) EntryPoints.Add(ABTJson::StringValue(Event->EventReference.GetMemberName().ToString()));
            if (UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Node)) EntryPoints.Add(ABTJson::StringValue(Custom->CustomFunctionName.ToString()));
            if (UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node)) Reads.Add(ABTJson::StringValue(Get->GetVarNameString()));
            if (UK2Node_VariableSet* Set = Cast<UK2Node_VariableSet>(Node)) Writes.Add(ABTJson::StringValue(Set->GetVarNameString()));
            if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node)) Calls.Add(ABTJson::StringValue(Call->FunctionReference.GetMemberName().ToString()));
        }
    }
    Summary->SetArrayField(TEXT("entry_points"), EntryPoints);
    Summary->SetArrayField(TEXT("variable_reads"), Reads);
    Summary->SetArrayField(TEXT("variable_writes"), Writes);
    Summary->SetArrayField(TEXT("external_calls"), Calls);
    OutJson->SetObjectField(TEXT("semantic_summary"), Summary);
    return true;
}

bool FABTBlueprintTools::AnalyzeBlueprintGraph(const FString& AssetPath, const FString& GraphName, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    UBlueprint* Blueprint = LoadBlueprint(AssetPath, OutError);
    if (!Blueprint) return false;

    UEdGraph* Graph = GraphName.IsEmpty() ? (Blueprint->UbergraphPages.Num() ? Blueprint->UbergraphPages[0].Get() : nullptr) : FindGraph(Blueprint, GraphName);
    if (!Graph)
    {
        OutError = TEXT("Graph not found");
        return false;
    }

    OutJson = ABTJson::Ok();
    OutJson->SetStringField(TEXT("asset_path"), AssetPath);
    OutJson->SetStringField(TEXT("graph"), Graph->GetName());
    OutJson->SetObjectField(TEXT("graph_ir"), ExportGraph(Graph));
    return true;
}

bool FABTBlueprintTools::ListCallableFunctions(const FString& Query, int32 Limit, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    OutJson = ABTJson::Ok();
    TArray<TSharedPtr<FJsonValue>> Functions;

    const FString LowerQuery = Query.ToLower();
    int32 Count = 0;
    for (TObjectIterator<UClass> ClassIt; ClassIt && Count < Limit; ++ClassIt)
    {
        UClass* Class = *ClassIt;
        if (!Class || Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists)) continue;

        for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt && Count < Limit; ++FuncIt)
        {
            UFunction* Function = *FuncIt;
            if (!Function || !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure)) continue;

            const FString FullName = Class->GetName() + TEXT("::") + Function->GetName();
            if (!LowerQuery.IsEmpty() && !FullName.ToLower().Contains(LowerQuery)) continue;

            TSharedPtr<FJsonObject> Item = ABTJson::Object();
            Item->SetStringField(TEXT("class"), Class->GetPathName());
            Item->SetStringField(TEXT("class_name"), Class->GetName());
            Item->SetStringField(TEXT("function"), Function->GetName());
            Item->SetStringField(TEXT("display"), FullName);
            Functions.Add(ABTJson::ObjectValue(Item));
            Count++;
        }
    }

    OutJson->SetArrayField(TEXT("functions"), Functions);
    return true;
}

bool FABTBlueprintTools::ValidatePatch(const TSharedPtr<FJsonObject>& Patch, TArray<FString>& OutMessages)
{
    if (!Patch.IsValid())
    {
        OutMessages.Add(TEXT("Patch object is invalid."));
        return false;
    }

    if (ABTJson::GetString(Patch, TEXT("target")).IsEmpty())
    {
        OutMessages.Add(TEXT("Patch.target is required."));
    }

    const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
    if (!Patch->TryGetArrayField(TEXT("operations"), Ops) || !Ops)
    {
        OutMessages.Add(TEXT("Patch.operations array is required."));
    }

    return OutMessages.Num() == 0;
}

bool FABTBlueprintTools::DryRunPatch(const TSharedPtr<FJsonObject>& Patch, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    TArray<FString> Messages;
    const bool bValid = ValidatePatch(Patch, Messages);

    OutJson = ABTJson::Ok();
    OutJson->SetBoolField(TEXT("valid"), bValid);
    TArray<TSharedPtr<FJsonValue>> JsonMessages;
    for (const FString& Message : Messages)
    {
        JsonMessages.Add(ABTJson::StringValue(Message));
    }
    OutJson->SetArrayField(TEXT("messages"), JsonMessages);
    return true;
}

bool FABTBlueprintTools::ApplyOperation(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, TArray<FString>& OutMessages, FString& OutError)
{
    const FString OpName = ABTJson::GetString(Op, TEXT("op"));

    if (OpName == TEXT("ensure_variable"))
    {
        const FString Name = ABTJson::GetString(Op, TEXT("name"));
        const FString Type = ABTJson::GetString(Op, TEXT("type"));
        if (Name.IsEmpty()) { OutError = TEXT("ensure_variable.name missing"); return false; }

        bool bExists = false;
        for (const FBPVariableDescription& Var : Blueprint->NewVariables)
        {
            if (Var.VarName.ToString().Equals(Name, ESearchCase::IgnoreCase)) { bExists = true; break; }
        }
        if (!bExists)
        {
            FBlueprintEditorUtils::AddMemberVariable(Blueprint, *Name, MakePinTypeFromString(Type));
            OutMessages.Add(FString::Printf(TEXT("Added variable %s"), *Name));
        }
        return true;
    }

    if (OpName == TEXT("ensure_function"))
    {
        const FString Name = ABTJson::GetString(Op, TEXT("name"));
        if (Name.IsEmpty()) { OutError = TEXT("ensure_function.name missing"); return false; }
        UEdGraph* Graph = EnsureFunctionGraph(Blueprint, Name);
        if (!Graph) { OutError = TEXT("Failed to create function graph"); return false; }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Cast<UK2Node_FunctionEntry>(Node))
            {
                NodeMap.Add(TEXT("Entry"), Node);
                break;
            }
        }
        OutMessages.Add(FString::Printf(TEXT("Ensured function %s"), *Name));
        return true;
    }

    if (OpName == TEXT("ensure_move_function"))
    {
        const FString FunctionName = ABTJson::GetString(Op, TEXT("name"), TEXT("MoveByDelta"));
        UEdGraph* Graph = EnsureFunctionGraph(Blueprint, FunctionName);
        if (!Graph) { OutError = TEXT("Failed to create move function graph"); return false; }

        UK2Node_FunctionEntry* Entry = nullptr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionEntry* Candidate = Cast<UK2Node_FunctionEntry>(Node))
            {
                Entry = Candidate;
                break;
            }
        }
        if (!Entry) { OutError = TEXT("Move function entry not found"); return false; }

        bool bAlreadyHasMoveCall = false;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
            {
                if (Call->FunctionReference.GetMemberName() == TEXT("K2_AddActorWorldOffset"))
                {
                    bAlreadyHasMoveCall = true;
                    break;
                }
            }
        }
        if (bAlreadyHasMoveCall)
        {
            OutMessages.Add(FString::Printf(TEXT("Move function already exists: %s"), *FunctionName));
            return true;
        }

        UFunction* Function = AActor::StaticClass()->FindFunctionByName(TEXT("K2_AddActorWorldOffset"));
        if (!Function) { OutError = TEXT("AActor::K2_AddActorWorldOffset not found"); return false; }

        UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
        Call->SetFromFunction(Function);
        Graph->AddNode(Call, true, false);
        Call->CreateNewGuid();
        Call->NodePosX = 260;
        Call->NodePosY = 0;
        Call->AllocateDefaultPins();

        UEdGraphPin* EntryThen = FindFirstPin(Entry, EGPD_Output, UEdGraphSchema_K2::PC_Exec);
        UEdGraphPin* CallExec = FindFirstPin(Call, EGPD_Input, UEdGraphSchema_K2::PC_Exec);
        if (!EntryThen || !CallExec)
        {
            OutError = TEXT("Could not find exec pins for move function");
            return false;
        }

        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        if (!Schema->TryCreateConnection(EntryThen, CallExec))
        {
            OutError = TEXT("Could not connect move function exec pins");
            return false;
        }

        if (UEdGraphPin* DeltaPin = FindVectorInputPin(Call))
        {
            DeltaPin->DefaultValue = VectorDefaultString(ReadVectorFromOp(Op, TEXT("delta"), FVector(100.0, 0.0, 0.0)));
        }

        OutMessages.Add(FString::Printf(TEXT("Ensured move function %s"), *FunctionName));
        return true;
    }

    if (OpName == TEXT("ensure_component"))
    {
        if (!Blueprint->SimpleConstructionScript)
        {
            OutError = TEXT("Blueprint has no SimpleConstructionScript");
            return false;
        }

        const FString Name = ABTJson::GetString(Op, TEXT("name"));
        const FString ClassName = ABTJson::GetString(Op, TEXT("class"), TEXT("StaticMeshComponent"));
        if (Name.IsEmpty()) { OutError = TEXT("ensure_component.name missing"); return false; }

        if (FindSCSNodeByName(Blueprint, Name))
        {
            OutMessages.Add(FString::Printf(TEXT("Component already exists: %s"), *Name));
            return true;
        }

        UClass* ComponentClass = ResolveClassByNameOrPath(ClassName, UActorComponent::StaticClass());
        if (!ComponentClass)
        {
            OutError = FString::Printf(TEXT("Component class not found: %s"), *ClassName);
            return false;
        }

        USCS_Node* Node = Blueprint->SimpleConstructionScript->CreateNode(ComponentClass, *Name);
        if (!Node)
        {
            OutError = TEXT("Create component node failed");
            return false;
        }

        const FString AttachTo = ABTJson::GetString(Op, TEXT("attachTo"));
        if (!AttachTo.IsEmpty())
        {
            if (USCS_Node* Parent = FindSCSNodeByName(Blueprint, AttachTo))
            {
                Parent->AddChildNode(Node);
            }
            else
            {
                OutError = FString::Printf(TEXT("Parent component not found: %s"), *AttachTo);
                return false;
            }
        }
        else
        {
            Blueprint->SimpleConstructionScript->AddNode(Node);
        }

        if (USceneComponent* SceneTemplate = Cast<USceneComponent>(Node->ComponentTemplate))
        {
            const double Scale = ABTJson::GetNumber(Op, TEXT("uniformScale"), 1.0);
            SceneTemplate->SetRelativeScale3D(FVector(Scale));
        }

        OutMessages.Add(FString::Printf(TEXT("Ensured component %s (%s)"), *Name, *ComponentClass->GetName()));
        return true;
    }

    if (OpName == TEXT("set_static_mesh"))
    {
        const FString ComponentName = ABTJson::GetString(Op, TEXT("component"));
        const FString MeshPath = ABTJson::GetString(Op, TEXT("mesh"));
        USCS_Node* Node = FindSCSNodeByName(Blueprint, ComponentName);
        UStaticMeshComponent* MeshComponent = Node ? Cast<UStaticMeshComponent>(Node->ComponentTemplate) : nullptr;
        if (!MeshComponent)
        {
            OutError = FString::Printf(TEXT("StaticMeshComponent not found: %s"), *ComponentName);
            return false;
        }
        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
        if (!Mesh)
        {
            OutError = FString::Printf(TEXT("StaticMesh not found: %s"), *MeshPath);
            return false;
        }
        MeshComponent->Modify();
        MeshComponent->SetStaticMesh(Mesh);
        OutMessages.Add(FString::Printf(TEXT("Set static mesh on %s"), *ComponentName));
        return true;
    }

    if (OpName == TEXT("set_component_material"))
    {
        const FString ComponentName = ABTJson::GetString(Op, TEXT("component"));
        const FString MaterialPath = ABTJson::GetString(Op, TEXT("material"));
        const int32 Index = ABTJson::GetInt(Op, TEXT("index"), 0);
        USCS_Node* Node = FindSCSNodeByName(Blueprint, ComponentName);
        UMeshComponent* MeshComponent = Node ? Cast<UMeshComponent>(Node->ComponentTemplate) : nullptr;
        if (!MeshComponent)
        {
            OutError = FString::Printf(TEXT("Mesh component not found: %s"), *ComponentName);
            return false;
        }
        UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
        if (!Material)
        {
            OutError = FString::Printf(TEXT("Material not found: %s"), *MaterialPath);
            return false;
        }
        MeshComponent->Modify();
        MeshComponent->SetMaterial(Index, Material);
        OutMessages.Add(FString::Printf(TEXT("Set material on %s[%d]"), *ComponentName, Index));
        return true;
    }

    if (OpName == TEXT("add_node"))
    {
        const FString GraphName = ABTJson::GetString(Op, TEXT("graph"), TEXT("EventGraph"));
        const FString Id = ABTJson::GetString(Op, TEXT("id"));
        const FString Type = ABTJson::GetString(Op, TEXT("type"));
        UEdGraph* Graph = FindGraph(Blueprint, GraphName);
        if (!Graph) { OutError = FString::Printf(TEXT("Graph not found: %s"), *GraphName); return false; }

        UEdGraphNode* NewNode = nullptr;
        if (Type == TEXT("Branch"))
        {
            UK2Node_IfThenElse* Node = NewObject<UK2Node_IfThenElse>(Graph);
            Graph->AddNode(Node, true, false);
            Node->CreateNewGuid();
            Node->AllocateDefaultPins();
            NewNode = Node;
        }
        else if (Type == TEXT("GetVariable"))
        {
            UK2Node_VariableGet* Node = NewObject<UK2Node_VariableGet>(Graph);
            Node->VariableReference.SetSelfMember(*ABTJson::GetString(Op, TEXT("variable")));
            Graph->AddNode(Node, true, false);
            Node->CreateNewGuid();
            Node->AllocateDefaultPins();
            NewNode = Node;
        }
        else if (Type == TEXT("SetVariable"))
        {
            UK2Node_VariableSet* Node = NewObject<UK2Node_VariableSet>(Graph);
            Node->VariableReference.SetSelfMember(*ABTJson::GetString(Op, TEXT("variable")));
            Graph->AddNode(Node, true, false);
            Node->CreateNewGuid();
            Node->AllocateDefaultPins();
            NewNode = Node;
        }
        else if (Type == TEXT("CustomEvent"))
        {
            UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(Graph);
            Node->CustomFunctionName = *ABTJson::GetString(Op, TEXT("event"), Id);
            Graph->AddNode(Node, true, false);
            Node->CreateNewGuid();
            Node->AllocateDefaultPins();
            NewNode = Node;
        }
        else if (Type == TEXT("CallFunction"))
        {
            FString FunctionName = ABTJson::GetString(Op, TEXT("function"));
            FString ClassName = ABTJson::GetString(Op, TEXT("class"));
            const TSharedPtr<FJsonObject>* FunctionObject = nullptr;
            if (Op->TryGetObjectField(TEXT("function_ref"), FunctionObject) && FunctionObject && FunctionObject->IsValid())
            {
                FunctionName = ABTJson::GetString(*FunctionObject, TEXT("name"), FunctionName);
                ClassName = ABTJson::GetString(*FunctionObject, TEXT("class"), ClassName);
            }
            UFunction* Function = FindFunctionByClassAndName(ClassName, FunctionName);
            if (!Function) { OutError = FString::Printf(TEXT("Function not found: %s"), *FunctionName); return false; }
            UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
            Node->SetFromFunction(Function);
            Graph->AddNode(Node, true, false);
            Node->CreateNewGuid();
            Node->AllocateDefaultPins();
            NewNode = Node;
        }
        else
        {
            OutError = FString::Printf(TEXT("Unsupported add_node.type: %s"), *Type);
            return false;
        }

        if (NewNode)
        {
            NewNode->NodePosX = ABTJson::GetInt(Op, TEXT("x"), 0);
            NewNode->NodePosY = ABTJson::GetInt(Op, TEXT("y"), 0);
            if (!Id.IsEmpty()) NodeMap.Add(Id, NewNode);
            OutMessages.Add(FString::Printf(TEXT("Added node %s:%s"), *Id, *Type));
        }
        return true;
    }

    if (OpName == TEXT("connect_exec") || OpName == TEXT("connect_data"))
    {
        FString FromNodeId, FromPinName, ToNodeId, ToPinName;
        if (!ParseNodePinRef(ABTJson::GetString(Op, TEXT("from")), FromNodeId, FromPinName) ||
            !ParseNodePinRef(ABTJson::GetString(Op, TEXT("to")), ToNodeId, ToPinName))
        {
            OutError = TEXT("connect op requires from/to in node.pin form");
            return false;
        }

        UEdGraphNode* FromNode = NodeMap.FindRef(FromNodeId);
        UEdGraphNode* ToNode = NodeMap.FindRef(ToNodeId);
        if (!FromNode || !ToNode)
        {
            OutError = TEXT("connect referenced unknown node id");
            return false;
        }

        UEdGraphPin* FromPin = FindPinByName(FromNode, FromPinName);
        UEdGraphPin* ToPin = FindPinByName(ToNode, ToPinName);
        if (!FromPin || !ToPin)
        {
            OutError = TEXT("connect referenced unknown pin name");
            return false;
        }

        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        if (!Schema->TryCreateConnection(FromPin, ToPin))
        {
            OutError = TEXT("TryCreateConnection failed");
            return false;
        }
        OutMessages.Add(FString::Printf(TEXT("Connected %s -> %s"), *ABTJson::GetString(Op, TEXT("from")), *ABTJson::GetString(Op, TEXT("to"))));
        return true;
    }

    if (OpName == TEXT("set_pin_default"))
    {
        FString NodeId, PinName;
        if (!ParseNodePinRef(ABTJson::GetString(Op, TEXT("target")), NodeId, PinName))
        {
            OutError = TEXT("set_pin_default.target must be node.pin");
            return false;
        }
        UEdGraphNode* Node = NodeMap.FindRef(NodeId);
        UEdGraphPin* Pin = FindPinByName(Node, PinName);
        if (!Pin) { OutError = TEXT("pin not found"); return false; }
        Pin->DefaultValue = ABTJson::GetString(Op, TEXT("value"));
        return true;
    }

    OutError = FString::Printf(TEXT("Unsupported op: %s"), *OpName);
    return false;
}

bool FABTBlueprintTools::CompileBlueprint(UBlueprint* Blueprint, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    if (!Blueprint)
    {
        OutError = TEXT("CompileBlueprint called with null Blueprint");
        return false;
    }

    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    OutJson = ABTJson::Ok();
    const bool bCompileOk = Blueprint->Status != BS_Error;
    OutJson->SetBoolField(TEXT("compile_ok"), bCompileOk);
    OutJson->SetStringField(TEXT("status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(Blueprint->Status));
    return true;
}

bool FABTBlueprintTools::ApplyPatch(const TSharedPtr<FJsonObject>& Patch, bool bSaveOnSuccess, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    TArray<FString> ValidationMessages;
    if (!ValidatePatch(Patch, ValidationMessages))
    {
        OutJson = ABTJson::Ok();
        OutJson->SetBoolField(TEXT("valid"), false);
        return true;
    }

    UBlueprint* Blueprint = LoadBlueprint(ABTJson::GetString(Patch, TEXT("target")), OutError);
    if (!Blueprint) return false;

    const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
    Patch->TryGetArrayField(TEXT("operations"), Ops);

    const FScopedTransaction Transaction(NSLOCTEXT("AgentBlueprintTools", "ApplyBlueprintPatch", "Apply Blueprint Patch"));
    Blueprint->Modify();

    TArray<FString> Messages;
    TMap<FString, UEdGraphNode*> NodeMap;

    for (const TSharedPtr<FJsonValue>& Value : *Ops)
    {
        TSharedPtr<FJsonObject> Op = Value->AsObject();
        if (!Op.IsValid())
        {
            OutError = TEXT("operation is not an object");
            return false;
        }
        if (!ApplyOperation(Blueprint, Op, NodeMap, Messages, OutError))
        {
            return false;
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> CompileJson;
    if (!CompileBlueprint(Blueprint, CompileJson, OutError)) return false;

    OutJson = ABTJson::Ok();
    OutJson->SetObjectField(TEXT("compile"), CompileJson);
    OutJson->SetBoolField(TEXT("saved"), false);

    TArray<TSharedPtr<FJsonValue>> JsonMessages;
    for (const FString& Message : Messages) JsonMessages.Add(ABTJson::StringValue(Message));
    OutJson->SetArrayField(TEXT("messages"), JsonMessages);

    if (!CompileJson->GetBoolField(TEXT("compile_ok")))
    {
        OutJson->SetBoolField(TEXT("rolledBack"), true);
        return true;
    }

    if (bSaveOnSuccess)
    {
        UPackage* Package = Blueprint->GetOutermost();
        if (Package) Package->SetDirtyFlag(true);
        OutJson->SetBoolField(TEXT("saved"), true);
    }

    return true;
}

bool FABTBlueprintTools::CompileBlueprintAsset(const FString& AssetPath, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    UBlueprint* Blueprint = LoadBlueprint(AssetPath, OutError);
    if (!Blueprint) return false;
    return CompileBlueprint(Blueprint, OutJson, OutError);
}
