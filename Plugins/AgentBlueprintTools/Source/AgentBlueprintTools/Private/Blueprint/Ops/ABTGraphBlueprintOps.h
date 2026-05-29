#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UEdGraphNode;

namespace ABT::Blueprint::Ops
{
    bool EnsureVariable(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError);
    bool EnsureFunction(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, TArray<FString>& OutMessages, FString& OutError);
    bool AddNode(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, TArray<FString>& OutMessages, FString& OutError);
    bool ConnectPins(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, TArray<FString>& OutMessages, FString& OutError);
    bool SetPinDefault(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, TArray<FString>& OutMessages, FString& OutError);
    bool RemoveNode(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, TArray<FString>& OutMessages, FString& OutError);
    bool SetBlueprintProperty(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError);
}
