#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

struct FABTBlueprintExportOptions
{
    bool bIncludeGraphs = true;
    bool bIncludePins = true;
};

class FABTBlueprintTools
{
public:
    static bool ExportBlueprint(const FString& AssetPath, TSharedPtr<FJsonObject>& OutJson, FString& OutError, const FABTBlueprintExportOptions& Options = FABTBlueprintExportOptions());
    static bool AnalyzeBlueprintGraph(const FString& AssetPath, const FString& GraphName, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
    static bool ListCallableFunctions(const FString& Query, int32 Limit, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
    static bool DryRunPatch(const TSharedPtr<FJsonObject>& Patch, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
    static bool ApplyPatch(const TSharedPtr<FJsonObject>& Patch, bool bSaveOnSuccess, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
    static bool CompileBlueprintAsset(const FString& AssetPath, TSharedPtr<FJsonObject>& OutJson, FString& OutError);

private:
    static UBlueprint* LoadBlueprint(const FString& AssetPath, FString& OutError);
    static UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName);
    static UEdGraph* EnsureFunctionGraph(UBlueprint* Blueprint, const FString& GraphName);

    static TSharedPtr<FJsonObject> ExportGraph(UEdGraph* Graph, bool bIncludePins);
    static TSharedPtr<FJsonObject> ExportNode(UEdGraphNode* Node, bool bIncludePins);
    static TSharedPtr<FJsonObject> ExportPin(UEdGraphPin* Pin);

    static bool ValidatePatch(const TSharedPtr<FJsonObject>& Patch, TArray<FString>& OutMessages);
    static bool ApplyOperation(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, TArray<FString>& OutMessages, FString& OutError);
    static bool CompileBlueprint(UBlueprint* Blueprint, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
};
