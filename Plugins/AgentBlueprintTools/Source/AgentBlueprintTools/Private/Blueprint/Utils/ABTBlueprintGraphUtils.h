#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UFunction;
class USCS_Node;

namespace ABT::Blueprint
{
    FEdGraphPinType MakePinTypeFromString(const FString& TypeName);
    FString PinDirectionToString(EEdGraphPinDirection Dir);
    FString PinTypeToString(const FEdGraphPinType& Type);

    UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName);
    bool ParseNodePinRef(const FString& Ref, FString& OutNodeId, FString& OutPinName);
    UEdGraphNode* FindExistingNodeByGuidOrName(UEdGraph* Graph, const FString& Id);
    UEdGraphPin* FindFirstPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const FName& Category);
    UEdGraphPin* FindVectorInputPin(UEdGraphNode* Node);

    UFunction* FindFunctionByClassAndName(const FString& ClassName, const FString& FunctionName);
    UClass* ResolveClassByNameOrPath(const FString& ClassNameOrPath, UClass* RequiredParent);
    USCS_Node* FindSCSNodeByName(UBlueprint* Blueprint, const FString& ComponentName);

    UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName);
    UEdGraph* EnsureFunctionGraph(UBlueprint* Blueprint, const FString& GraphName);

    FString VectorDefaultString(const FVector& Value);
    FVector ReadVectorFromOp(const TSharedPtr<FJsonObject>& Op, const FString& Field, const FVector& DefaultValue);
}
