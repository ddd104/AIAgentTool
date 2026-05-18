#include "Blueprint/Utils/ABTBlueprintGraphUtils.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/FieldIterator.h"

namespace ABT::Blueprint
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

    UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName)
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

    UEdGraph* EnsureFunctionGraph(UBlueprint* Blueprint, const FString& GraphName)
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
