#include "AgentProjectGraphModule.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/StringConv.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Variable.h"
#include "MaterialShaderType.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "FAgentProjectGraphModule"

namespace
{
    constexpr const TCHAR* BlueprintCacheDirName = TEXT(".ai/cache/blueprint_ir");
    constexpr const TCHAR* AssetRegistryCacheDirName = TEXT(".ai/cache/asset_registry");
    constexpr const TCHAR* MaterialCacheDirName = TEXT(".ai/cache/material_ir");

    TSharedPtr<FJsonObject> MakeOk()
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetBoolField(TEXT("ok"), true);
        return Json;
    }

    TSharedPtr<FJsonObject> MakeWarningResult(const FString& Warning)
    {
        TSharedPtr<FJsonObject> Json = MakeOk();
        TArray<TSharedPtr<FJsonValue>> Warnings;
        Warnings.Add(MakeShared<FJsonValueString>(Warning));
        Json->SetBoolField(TEXT("success"), false);
        Json->SetArrayField(TEXT("warnings"), Warnings);
        return Json;
    }

    FString JsonToString(const TSharedPtr<FJsonObject>& Object)
    {
        FString Output;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
        FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
        return Output;
    }

    bool JsonFromString(const FString& Text, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
    {
        if (Text.TrimStartAndEnd().IsEmpty())
        {
            OutObject = MakeShared<FJsonObject>();
            return true;
        }

        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
        if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
        {
            OutError = TEXT("Request body is not valid JSON.");
            return false;
        }
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> JsonValues;
        for (const FString& Value : Values)
        {
            JsonValues.Add(MakeShared<FJsonValueString>(Value));
        }
        return JsonValues;
    }

    FString GuidToString(const FGuid& Guid)
    {
        return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphens) : FString();
    }

    FString PinDirectionToString(EEdGraphPinDirection Direction)
    {
        switch (Direction)
        {
        case EGPD_Input:
            return TEXT("Input");
        case EGPD_Output:
            return TEXT("Output");
        default:
            return TEXT("Unknown");
        }
    }

    FString PinContainerTypeToString(EPinContainerType ContainerType)
    {
        switch (ContainerType)
        {
        case EPinContainerType::None:
            return TEXT("None");
        case EPinContainerType::Array:
            return TEXT("Array");
        case EPinContainerType::Set:
            return TEXT("Set");
        case EPinContainerType::Map:
            return TEXT("Map");
        default:
            return TEXT("Unknown");
        }
    }

    bool IsExecPin(const UEdGraphPin* Pin)
    {
        return Pin && Pin->PinType.PinCategory == TEXT("exec");
    }

    TSharedPtr<FJsonObject> PinTypeJson(const FEdGraphPinType& PinType)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("category"), PinType.PinCategory.ToString());
        Json->SetStringField(TEXT("subCategory"), PinType.PinSubCategory.ToString());
        Json->SetStringField(TEXT("containerType"), PinContainerTypeToString(PinType.ContainerType));
        Json->SetBoolField(TEXT("isReference"), PinType.bIsReference != 0);
        Json->SetBoolField(TEXT("isConst"), PinType.bIsConst != 0);
        Json->SetBoolField(TEXT("isWeakPointer"), PinType.bIsWeakPointer != 0);
        Json->SetBoolField(TEXT("isUObjectWrapper"), PinType.bIsUObjectWrapper != 0);
        if (PinType.PinSubCategoryObject.IsValid())
        {
            Json->SetStringField(TEXT("subCategoryObject"), PinType.PinSubCategoryObject->GetPathName());
        }
        if (!PinType.PinValueType.TerminalCategory.IsNone())
        {
            TSharedPtr<FJsonObject> ValueType = MakeShared<FJsonObject>();
            ValueType->SetStringField(TEXT("category"), PinType.PinValueType.TerminalCategory.ToString());
            ValueType->SetStringField(TEXT("subCategory"), PinType.PinValueType.TerminalSubCategory.ToString());
            if (PinType.PinValueType.TerminalSubCategoryObject.IsValid())
            {
                ValueType->SetStringField(TEXT("subCategoryObject"), PinType.PinValueType.TerminalSubCategoryObject->GetPathName());
            }
            Json->SetObjectField(TEXT("valueType"), ValueType);
        }
        return Json;
    }

    TSharedPtr<FJsonObject> MetadataArrayJson(const TArray<FBPVariableMetaDataEntry>& MetadataArray)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        for (const FBPVariableMetaDataEntry& Entry : MetadataArray)
        {
            Json->SetStringField(Entry.DataKey.ToString(), Entry.DataValue);
        }
        return Json;
    }

    TArray<TSharedPtr<FJsonValue>> PinArrayJson(const TArray<UEdGraphPin*>& Pins);

    TSharedPtr<FJsonObject> PinConnectionJson(const UEdGraphPin* Pin)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        if (!Pin)
        {
            return Json;
        }

        const UEdGraphNode* OwningNode = Pin->GetOwningNodeUnchecked();
        Json->SetStringField(TEXT("pinId"), GuidToString(Pin->PinId));
        Json->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
        Json->SetStringField(TEXT("direction"), PinDirectionToString(Pin->Direction));
        if (OwningNode)
        {
            Json->SetStringField(TEXT("nodeGuid"), GuidToString(OwningNode->NodeGuid));
            Json->SetStringField(TEXT("nodeTitle"), OwningNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
            Json->SetStringField(TEXT("nodeClass"), OwningNode->GetClass()->GetPathName());
        }
        return Json;
    }

    TSharedPtr<FJsonObject> PinJson(const UEdGraphPin* Pin)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        if (!Pin)
        {
            return Json;
        }

        Json->SetStringField(TEXT("pinId"), GuidToString(Pin->PinId));
        Json->SetStringField(TEXT("persistentGuid"), GuidToString(Pin->PersistentGuid));
        Json->SetStringField(TEXT("name"), Pin->PinName.ToString());
#if WITH_EDITORONLY_DATA
        Json->SetStringField(TEXT("friendlyName"), Pin->PinFriendlyName.ToString());
#endif
        Json->SetStringField(TEXT("direction"), PinDirectionToString(Pin->Direction));
        Json->SetObjectField(TEXT("type"), PinTypeJson(Pin->PinType));
        Json->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
        Json->SetStringField(TEXT("autogeneratedDefaultValue"), Pin->AutogeneratedDefaultValue);
        Json->SetStringField(TEXT("defaultTextValue"), Pin->DefaultTextValue.ToString());
        Json->SetBoolField(TEXT("defaultValueIsReadOnly"), Pin->bDefaultValueIsReadOnly != 0);
        Json->SetBoolField(TEXT("defaultValueIsIgnored"), Pin->bDefaultValueIsIgnored != 0);
        if (Pin->DefaultObject)
        {
            Json->SetStringField(TEXT("defaultObject"), Pin->DefaultObject->GetPathName());
        }

        TArray<TSharedPtr<FJsonValue>> Connections;
        for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            if (LinkedPin)
            {
                Connections.Add(MakeShared<FJsonValueObject>(PinConnectionJson(LinkedPin)));
            }
        }
        Json->SetArrayField(TEXT("connections"), Connections);

        TArray<TSharedPtr<FJsonValue>> SubPins;
        for (const UEdGraphPin* SubPin : Pin->SubPins)
        {
            if (SubPin)
            {
                SubPins.Add(MakeShared<FJsonValueObject>(PinJson(SubPin)));
            }
        }
        Json->SetArrayField(TEXT("subPins"), SubPins);
        return Json;
    }

    TArray<TSharedPtr<FJsonValue>> PinArrayJson(const TArray<UEdGraphPin*>& Pins)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const UEdGraphPin* Pin : Pins)
        {
            if (Pin)
            {
                Values.Add(MakeShared<FJsonValueObject>(PinJson(Pin)));
            }
        }
        return Values;
    }

    TSharedPtr<FJsonObject> VariableDescriptionJson(const FBPVariableDescription& Variable)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("name"), Variable.VarName.ToString());
        Json->SetStringField(TEXT("guid"), GuidToString(Variable.VarGuid));
        Json->SetStringField(TEXT("friendlyName"), Variable.FriendlyName);
        Json->SetStringField(TEXT("category"), Variable.Category.ToString());
        Json->SetStringField(TEXT("defaultValue"), Variable.DefaultValue);
        Json->SetStringField(TEXT("pinCategory"), Variable.VarType.PinCategory.ToString());
        Json->SetStringField(TEXT("pinSubCategory"), Variable.VarType.PinSubCategory.ToString());
        if (Variable.VarType.PinSubCategoryObject.IsValid())
        {
            Json->SetStringField(TEXT("pinSubCategoryObject"), Variable.VarType.PinSubCategoryObject->GetPathName());
        }
        Json->SetObjectField(TEXT("type"), PinTypeJson(Variable.VarType));
        Json->SetObjectField(TEXT("metadata"), MetadataArrayJson(Variable.MetaDataArray));
        Json->SetNumberField(TEXT("propertyFlags"), static_cast<double>(Variable.PropertyFlags));
        Json->SetStringField(TEXT("repNotifyFunc"), Variable.RepNotifyFunc.ToString());
        Json->SetStringField(TEXT("replicationCondition"), FString::FromInt(static_cast<int32>(Variable.ReplicationCondition.GetValue())));

        const bool bInstanceEditable = (Variable.PropertyFlags & CPF_Edit) != 0;
        const bool bBlueprintVisible = (Variable.PropertyFlags & CPF_BlueprintVisible) != 0;
        const bool bBlueprintReadOnly = (Variable.PropertyFlags & CPF_BlueprintReadOnly) != 0;
        const bool bExposeOnSpawn = Variable.HasMetaData(TEXT("ExposeOnSpawn"));
        Json->SetBoolField(TEXT("instanceEditable"), bInstanceEditable);
        Json->SetBoolField(TEXT("blueprintVisible"), bBlueprintVisible);
        Json->SetBoolField(TEXT("blueprintReadOnly"), bBlueprintReadOnly);
        Json->SetBoolField(TEXT("exposeOnSpawn"), bExposeOnSpawn);
        Json->SetBoolField(TEXT("isExposed"), bInstanceEditable || bExposeOnSpawn || bBlueprintVisible);
        return Json;
    }

    TSharedPtr<FJsonObject> ExportEditableObjectDefaults(UObject* Object, int32 MaxProperties = 80)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        if (!Object)
        {
            return Json;
        }

        int32 ExportedCount = 0;
        for (TFieldIterator<FProperty> PropertyIt(Object->GetClass()); PropertyIt && ExportedCount < MaxProperties; ++PropertyIt)
        {
            FProperty* Property = *PropertyIt;
            if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
            {
                continue;
            }
            if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient))
            {
                continue;
            }

            FString Value;
            const void* PropertyValue = Property->ContainerPtrToValuePtr<void>(Object);
            Property->ExportTextItem_Direct(Value, PropertyValue, nullptr, Object, PPF_None);
            if (Value.Len() > 512)
            {
                Value = Value.Left(512) + TEXT("...");
            }
            Json->SetStringField(Property->GetName(), Value);
            ++ExportedCount;
        }
        return Json;
    }

    void AddWarning(TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& Warning)
    {
        Warnings.Add(MakeShared<FJsonValueString>(Warning));
        UE_LOG(LogTemp, Warning, TEXT("AgentProjectGraph: %s"), *Warning);
    }

    FString CacheRoot(const TCHAR* RelativeDir)
    {
        return FPaths::Combine(FPaths::ProjectDir(), RelativeDir);
    }

    bool EnsureDirectory(const FString& Directory, TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        if (IFileManager::Get().MakeDirectory(*Directory, true))
        {
            return true;
        }

        AddWarning(Warnings, FString::Printf(TEXT("Failed to create output directory: %s"), *Directory));
        return false;
    }

    FString MakeObjectPath(const FString& AssetPath)
    {
        FString Normalized = AssetPath.TrimStartAndEnd();
        if (Normalized.IsEmpty())
        {
            return Normalized;
        }

        const int32 LastSlash = Normalized.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        const int32 LastDot = Normalized.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        if (LastDot > LastSlash)
        {
            return Normalized;
        }

        FString Leaf = Normalized.Mid(LastSlash + 1);
        return FString::Printf(TEXT("%s.%s"), *Normalized, *Leaf);
    }

    FString SanitizedFileName(const FString& AssetPath)
    {
        FString Name = AssetPath;
        Name.ReplaceInline(TEXT("/"), TEXT("_"));
        Name.ReplaceInline(TEXT("\\"), TEXT("_"));
        Name.ReplaceInline(TEXT(":"), TEXT("_"));
        Name.ReplaceInline(TEXT("."), TEXT("_"));
        Name.TrimStartAndEndInline();
        if (Name.StartsWith(TEXT("_")))
        {
            Name.RightChopInline(1);
        }
        return Name.IsEmpty() ? TEXT("asset") : Name;
    }

    bool SaveJsonFile(const FString& Directory, const FString& FileName, const TSharedPtr<FJsonObject>& Json, FString& OutPath, TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        if (!EnsureDirectory(Directory, Warnings))
        {
            return false;
        }

        OutPath = FPaths::Combine(Directory, FileName);
        if (!FFileHelper::SaveStringToFile(JsonToString(Json), *OutPath))
        {
            AddWarning(Warnings, FString::Printf(TEXT("Failed to write JSON: %s"), *OutPath));
            return false;
        }
        return true;
    }

    FString ObjectPathOrEmpty(const UObject* Object)
    {
        return Object ? Object->GetPathName() : FString();
    }

    FString ClassPathOrEmpty(const UClass* Class)
    {
        return Class ? Class->GetPathName() : FString();
    }

    FString EnumValueToString(const UEnum* Enum, int64 Value)
    {
        return Enum ? Enum->GetNameStringByValue(Value) : FString::FromInt(static_cast<int32>(Value));
    }

    IAssetRegistry& AssetRegistry()
    {
        return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    }

    void PackageReferences(FName PackageName, TArray<FString>& OutDependencies, TArray<FString>& OutReferencers)
    {
        IAssetRegistry& Registry = AssetRegistry();

        TArray<FName> Dependencies;
        Registry.GetDependencies(PackageName, Dependencies, UE::AssetRegistry::EDependencyCategory::Package);
        Dependencies.Sort(FNameLexicalLess());
        for (const FName& Dependency : Dependencies)
        {
            OutDependencies.Add(Dependency.ToString());
        }

        TArray<FName> Referencers;
        Registry.GetReferencers(PackageName, Referencers, UE::AssetRegistry::EDependencyCategory::Package);
        Referencers.Sort(FNameLexicalLess());
        for (const FName& Referencer : Referencers)
        {
            OutReferencers.Add(Referencer.ToString());
        }
    }

    TSharedPtr<FJsonObject> AssetSummaryJson(const FAssetData& AssetData)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("assetPath"), AssetData.GetObjectPathString());
        Json->SetStringField(TEXT("assetName"), AssetData.AssetName.ToString());
        Json->SetStringField(TEXT("assetClass"), AssetData.AssetClassPath.ToString());
        Json->SetStringField(TEXT("packageName"), AssetData.PackageName.ToString());
        Json->SetStringField(TEXT("packagePath"), AssetData.PackagePath.ToString());

        TSharedPtr<FJsonObject> Tags = MakeShared<FJsonObject>();
        for (const auto TagValue : AssetData.TagsAndValues)
        {
            const FString Value = TagValue.Value.AsString();
            Tags->SetStringField(TagValue.Key.ToString(), Value.Len() > 512 ? Value.Left(512) + TEXT("...") : Value);
        }
        Json->SetObjectField(TEXT("tags"), Tags);

        FString ParentMaterial;
        if (AssetData.GetTagValue(TEXT("Parent"), ParentMaterial) || AssetData.GetTagValue(TEXT("ParentMaterial"), ParentMaterial))
        {
            Json->SetStringField(TEXT("parentMaterial"), ParentMaterial);
        }

        TArray<FString> Dependencies;
        TArray<FString> Referencers;
        PackageReferences(AssetData.PackageName, Dependencies, Referencers);
        Json->SetArrayField(TEXT("dependencies"), StringArrayToJson(Dependencies));
        Json->SetArrayField(TEXT("referencers"), StringArrayToJson(Referencers));
        return Json;
    }

    TSharedPtr<FJsonObject> NodeReferenceJson(const UEdGraphNode* Node)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        if (!Node)
        {
            return Json;
        }

        if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
        {
            const UFunction* Function = CallNode->GetTargetFunction();
            Json->SetStringField(TEXT("kind"), TEXT("function"));
            Json->SetStringField(TEXT("name"), Function ? Function->GetName() : CallNode->FunctionReference.GetMemberName().ToString());
            Json->SetStringField(TEXT("path"), Function ? Function->GetPathName() : CallNode->FunctionReference.GetMemberName().ToString());
            if (const UClass* ParentClass = CallNode->FunctionReference.GetMemberParentClass())
            {
                Json->SetStringField(TEXT("parentClass"), ParentClass->GetPathName());
            }
            return Json;
        }

        if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
        {
            Json->SetStringField(TEXT("kind"), TEXT("variable"));
            Json->SetStringField(TEXT("name"), VariableNode->GetVarNameString());
            if (const UClass* SourceClass = VariableNode->GetVariableSourceClass())
            {
                Json->SetStringField(TEXT("sourceClass"), SourceClass->GetPathName());
            }
            return Json;
        }

        if (const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
        {
            Json->SetStringField(TEXT("kind"), TEXT("macro"));
            if (const UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
            {
                Json->SetStringField(TEXT("name"), MacroGraph->GetName());
                Json->SetStringField(TEXT("path"), MacroGraph->GetPathName());
            }
            if (const UBlueprint* SourceBlueprint = MacroNode->GetSourceBlueprint())
            {
                Json->SetStringField(TEXT("sourceBlueprint"), SourceBlueprint->GetPathName());
            }
            return Json;
        }

        Json->SetStringField(TEXT("kind"), TEXT("none"));
        return Json;
    }

    TSharedPtr<FJsonObject> NodeJson(const UEdGraphNode* Node)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        if (!Node)
        {
            return Json;
        }

        Json->SetStringField(TEXT("nodeGuid"), GuidToString(Node->NodeGuid));
        Json->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetPathName());
        Json->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
        Json->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        Json->SetStringField(TEXT("fullTitle"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
        Json->SetStringField(TEXT("tooltip"), Node->GetTooltipText().ToString());
        Json->SetNumberField(TEXT("x"), Node->NodePosX);
        Json->SetNumberField(TEXT("y"), Node->NodePosY);
        TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
        Position->SetNumberField(TEXT("x"), Node->NodePosX);
        Position->SetNumberField(TEXT("y"), Node->NodePosY);
        Json->SetObjectField(TEXT("position"), Position);

        const UK2Node* K2Node = Cast<UK2Node>(Node);
        Json->SetBoolField(TEXT("isPure"), K2Node ? K2Node->IsNodePure() : false);
        Json->SetBoolField(TEXT("isK2Node"), K2Node != nullptr);
        Json->SetObjectField(TEXT("reference"), NodeReferenceJson(Node));
        Json->SetObjectField(TEXT("functionReference"), NodeReferenceJson(Node));
        Json->SetArrayField(TEXT("pins"), PinArrayJson(Node->Pins));
        return Json;
    }

    TSharedPtr<FJsonObject> GraphConnectionJson(const UEdGraphPin* FromPin, const UEdGraphPin* ToPin)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetObjectField(TEXT("from"), PinConnectionJson(FromPin));
        Json->SetObjectField(TEXT("to"), PinConnectionJson(ToPin));
        Json->SetStringField(TEXT("type"), IsExecPin(FromPin) || IsExecPin(ToPin) ? TEXT("execution") : TEXT("data"));
        return Json;
    }

    void AddGraphConnections(const UEdGraph* Graph, TArray<TSharedPtr<FJsonValue>>& ExecutionLines, TArray<TSharedPtr<FJsonValue>>& DataLines)
    {
        if (!Graph)
        {
            return;
        }

        TSet<FString> SeenConnections;
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }

            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output)
                {
                    continue;
                }

                for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    if (!LinkedPin)
                    {
                        continue;
                    }
                    const FString Key = FString::Printf(TEXT("%s:%s>%s:%s"),
                        *GuidToString(Pin->PinId),
                        *Pin->PinName.ToString(),
                        *GuidToString(LinkedPin->PinId),
                        *LinkedPin->PinName.ToString());
                    if (SeenConnections.Contains(Key))
                    {
                        continue;
                    }
                    SeenConnections.Add(Key);

                    TSharedPtr<FJsonValueObject> ConnectionValue = MakeShared<FJsonValueObject>(GraphConnectionJson(Pin, LinkedPin));
                    if (IsExecPin(Pin) || IsExecPin(LinkedPin))
                    {
                        ExecutionLines.Add(ConnectionValue);
                    }
                    else
                    {
                        DataLines.Add(ConnectionValue);
                    }
                }
            }
        }
    }

    TArray<TSharedPtr<FJsonValue>> GraphPinsByDirection(const UEdGraph* Graph, bool bInputs)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        if (!Graph)
        {
            return Values;
        }

        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (const UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
            {
                if (bInputs)
                {
                    for (const UEdGraphPin* Pin : EntryNode->Pins)
                    {
                        if (Pin && Pin->Direction == EGPD_Output && !IsExecPin(Pin))
                        {
                            Values.Add(MakeShared<FJsonValueObject>(PinJson(Pin)));
                        }
                    }
                }
            }
            else if (const UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
            {
                if (!bInputs)
                {
                    for (const UEdGraphPin* Pin : ResultNode->Pins)
                    {
                        if (Pin && Pin->Direction == EGPD_Input && !IsExecPin(Pin))
                        {
                            Values.Add(MakeShared<FJsonValueObject>(PinJson(Pin)));
                        }
                    }
                }
            }
        }
        return Values;
    }

    TArray<TSharedPtr<FJsonValue>> LocalVariableArray(const UEdGraph* Graph)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        if (!Graph)
        {
            return Values;
        }

        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (const UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
            {
                for (const FBPVariableDescription& LocalVariable : EntryNode->LocalVariables)
                {
                    Values.Add(MakeShared<FJsonValueObject>(VariableDescriptionJson(LocalVariable)));
                }
            }
        }
        return Values;
    }

    TArray<TSharedPtr<FJsonValue>> GraphSummaryArray(const TArray<TObjectPtr<UEdGraph>>& Graphs, const TCHAR* GraphKind)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }

            TSharedPtr<FJsonObject> GraphJson = MakeShared<FJsonObject>();
            GraphJson->SetStringField(TEXT("name"), Graph->GetName());
            GraphJson->SetStringField(TEXT("graphKind"), GraphKind);
            GraphJson->SetStringField(TEXT("path"), Graph->GetPathName());
            GraphJson->SetNumberField(TEXT("nodeCount"), Graph->Nodes.Num());

            TArray<TSharedPtr<FJsonValue>> Nodes;
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node)
                {
                    continue;
                }
                Nodes.Add(MakeShared<FJsonValueObject>(NodeJson(Node)));
            }
            GraphJson->SetArrayField(TEXT("nodes"), Nodes);
            GraphJson->SetArrayField(TEXT("inputs"), GraphPinsByDirection(Graph, true));
            GraphJson->SetArrayField(TEXT("outputs"), GraphPinsByDirection(Graph, false));
            GraphJson->SetArrayField(TEXT("localVariables"), LocalVariableArray(Graph));

            TArray<TSharedPtr<FJsonValue>> ExecutionLines;
            TArray<TSharedPtr<FJsonValue>> DataLines;
            AddGraphConnections(Graph, ExecutionLines, DataLines);
            GraphJson->SetArrayField(TEXT("executionLines"), ExecutionLines);
            GraphJson->SetArrayField(TEXT("dataLines"), DataLines);
            Values.Add(MakeShared<FJsonValueObject>(GraphJson));
        }
        return Values;
    }

    TArray<TSharedPtr<FJsonValue>> CalledFunctionArray(const UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        TSet<FString> Seen;
        TArray<UEdGraph*> Graphs;
        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            Graphs.Add(Graph);
        }
        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            Graphs.Add(Graph);
        }

        for (const UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }

            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
                if (!CallNode)
                {
                    continue;
                }

                const UFunction* Function = CallNode->GetTargetFunction();
                const FString FunctionPath = Function ? Function->GetPathName() : CallNode->FunctionReference.GetMemberName().ToString();
                const FString Key = FString::Printf(TEXT("%s|%s"), *Graph->GetName(), *FunctionPath);
                if (Seen.Contains(Key))
                {
                    continue;
                }
                Seen.Add(Key);

                TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
                Json->SetStringField(TEXT("graph"), Graph->GetName());
                Json->SetStringField(TEXT("function"), FunctionPath);
                Json->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
                Values.Add(MakeShared<FJsonValueObject>(Json));
            }
        }
        return Values;
    }

    TArray<TSharedPtr<FJsonValue>> VariableArray(const UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
        {
            Values.Add(MakeShared<FJsonValueObject>(VariableDescriptionJson(Variable)));
        }
        return Values;
    }

    TArray<TSharedPtr<FJsonValue>> InterfaceArray(const UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
        {
            if (Interface.Interface)
            {
                Values.Add(MakeShared<FJsonValueString>(Interface.Interface->GetPathName()));
            }
        }
        return Values;
    }

    TArray<TSharedPtr<FJsonValue>> ComponentArray(const UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        const USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
        if (!SCS)
        {
            return Values;
        }

        for (const USCS_Node* Node : SCS->GetAllNodes())
        {
            if (!Node)
            {
                continue;
            }

            TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
            Json->SetStringField(TEXT("componentClass"), ClassPathOrEmpty(Node->ComponentClass));
            Json->SetStringField(TEXT("variableGuid"), GuidToString(Node->VariableGuid));
            Json->SetStringField(TEXT("attachToName"), Node->AttachToName.ToString());
            Json->SetStringField(TEXT("attachParent"), Node->ParentComponentOrVariableName.ToString());
            Json->SetStringField(TEXT("parentComponentOwnerClassName"), Node->ParentComponentOwnerClassName.ToString());
            Json->SetBoolField(TEXT("isParentComponentNative"), Node->bIsParentComponentNative);
            Json->SetObjectField(TEXT("metadata"), MetadataArrayJson(Node->MetaDataArray));

            TArray<FString> ChildNames;
            for (const USCS_Node* ChildNode : Node->GetChildNodes())
            {
                if (ChildNode)
                {
                    ChildNames.Add(ChildNode->GetVariableName().ToString());
                }
            }
            Json->SetArrayField(TEXT("children"), StringArrayToJson(ChildNames));

            if (UActorComponent* ComponentTemplate = Node->ComponentTemplate)
            {
                Json->SetStringField(TEXT("templateName"), ComponentTemplate->GetName());
                Json->SetStringField(TEXT("templatePath"), ComponentTemplate->GetPathName());
                Json->SetObjectField(TEXT("defaultProperties"), ExportEditableObjectDefaults(ComponentTemplate));

                if (const USceneComponent* SceneComponent = Cast<USceneComponent>(ComponentTemplate))
                {
                    Json->SetStringField(TEXT("mobility"), StaticEnum<EComponentMobility::Type>()->GetNameStringByValue(static_cast<int64>(SceneComponent->Mobility.GetValue())));
                }
            }
            Values.Add(MakeShared<FJsonValueObject>(Json));
        }
        return Values;
    }

    TSharedPtr<FJsonObject> BlueprintIrJson(UBlueprint* Blueprint, const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("assetPath"), AssetPath);
        Json->SetStringField(TEXT("blueprintName"), Blueprint->GetName());
        Json->SetStringField(TEXT("parentClass"), ClassPathOrEmpty(Blueprint->ParentClass));
        Json->SetStringField(TEXT("generatedClass"), ClassPathOrEmpty(Blueprint->GeneratedClass));
        Json->SetArrayField(TEXT("implementedInterfaces"), InterfaceArray(Blueprint));
        Json->SetArrayField(TEXT("variables"), VariableArray(Blueprint));
        Json->SetArrayField(TEXT("functions"), GraphSummaryArray(Blueprint->FunctionGraphs, TEXT("Function")));
        Json->SetArrayField(TEXT("macros"), GraphSummaryArray(Blueprint->MacroGraphs, TEXT("Macro")));
        Json->SetArrayField(TEXT("eventGraphs"), GraphSummaryArray(Blueprint->UbergraphPages, TEXT("EventGraph")));
        Json->SetArrayField(TEXT("components"), ComponentArray(Blueprint));
        Json->SetArrayField(TEXT("calledFunctions"), CalledFunctionArray(Blueprint));

        TArray<FString> Dependencies;
        TArray<FString> Referencers;
        PackageReferences(Blueprint->GetOutermost()->GetFName(), Dependencies, Referencers);
        Json->SetArrayField(TEXT("referencedAssets"), StringArrayToJson(Dependencies));
        return Json;
    }

    TSharedPtr<FJsonObject> MaterialIrJson(UMaterial* Material, const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("assetPath"), AssetPath);
        Json->SetStringField(TEXT("materialDomain"), EnumValueToString(StaticEnum<EMaterialDomain>(), static_cast<int64>(Material->MaterialDomain)));
        Json->SetStringField(TEXT("blendMode"), EnumValueToString(StaticEnum<EBlendMode>(), static_cast<int64>(Material->BlendMode)));
        Json->SetStringField(TEXT("shadingModel"), GetShadingModelFieldString(Material->GetShadingModels()));

        TArray<TSharedPtr<FJsonValue>> Expressions;
        for (const TObjectPtr<UMaterialExpression>& ExpressionPtr : Material->GetExpressions())
        {
            UMaterialExpression* Expression = ExpressionPtr.Get();
            if (!Expression)
            {
                continue;
            }

            TArray<FString> Captions;
            Expression->GetCaption(Captions);

            TSharedPtr<FJsonObject> ExpressionJson = MakeShared<FJsonObject>();
            ExpressionJson->SetStringField(TEXT("name"), Expression->GetName());
            ExpressionJson->SetStringField(TEXT("class"), Expression->GetClass()->GetPathName());
            ExpressionJson->SetNumberField(TEXT("editorX"), Expression->MaterialExpressionEditorX);
            ExpressionJson->SetNumberField(TEXT("editorY"), Expression->MaterialExpressionEditorY);
            ExpressionJson->SetArrayField(TEXT("caption"), StringArrayToJson(Captions));
            ExpressionJson->SetNumberField(TEXT("inputCount"), Expression->CountInputs());
            Expressions.Add(MakeShared<FJsonValueObject>(ExpressionJson));
        }
        Json->SetArrayField(TEXT("expressions"), Expressions);
        return Json;
    }

    UObject* LoadAssetObject(const FString& AssetPath, TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        const FString ObjectPath = MakeObjectPath(AssetPath);
        if (ObjectPath.IsEmpty())
        {
            AddWarning(Warnings, TEXT("Asset path is empty."));
            return nullptr;
        }

        UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
        if (!Object)
        {
            AddWarning(Warnings, FString::Printf(TEXT("Failed to load asset for read-only export: %s"), *AssetPath));
        }
        return Object;
    }

    bool GetAssetsByClass(UClass* Class, TArray<FAssetData>& OutAssets)
    {
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
        Filter.ClassPaths.Add(Class->GetClassPathName());
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;
        return AssetRegistry().GetAssets(Filter, OutAssets);
    }
}

void FAgentProjectGraphModule::StartupModule()
{
#if WITH_EDITOR
    if (IsRunningCommandlet())
    {
        return;
    }

    RegisterConsoleCommands();
    StartHttpServer();
#endif
}

void FAgentProjectGraphModule::ShutdownModule()
{
#if WITH_EDITOR
    StopHttpServer();
    UnregisterConsoleCommands();
#endif
}

void FAgentProjectGraphModule::RegisterConsoleCommands()
{
    IConsoleManager& ConsoleManager = IConsoleManager::Get();
    ConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("AgentProjectGraph.ExportAll"),
        TEXT("Export read-only Blueprint, asset registry, and material summaries to .ai/cache."),
        FConsoleCommandWithArgsDelegate::CreateRaw(this, &FAgentProjectGraphModule::HandleExportAllCommand),
        ECVF_Default));

    ConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("AgentProjectGraph.ExportBlueprint"),
        TEXT("Export one Blueprint summary. Usage: AgentProjectGraph.ExportBlueprint /Game/Path/BP_Name"),
        FConsoleCommandWithArgsDelegate::CreateRaw(this, &FAgentProjectGraphModule::HandleExportBlueprintCommand),
        ECVF_Default));

    ConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("AgentProjectGraph.ExportAssets"),
        TEXT("Export AssetRegistry summary to .ai/cache/asset_registry/assets.json."),
        FConsoleCommandWithArgsDelegate::CreateRaw(this, &FAgentProjectGraphModule::HandleExportAssetsCommand),
        ECVF_Default));

    ConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("AgentProjectGraph.ExportMaterials"),
        TEXT("Export material summaries to .ai/cache/material_ir."),
        FConsoleCommandWithArgsDelegate::CreateRaw(this, &FAgentProjectGraphModule::HandleExportMaterialsCommand),
        ECVF_Default));
}

void FAgentProjectGraphModule::UnregisterConsoleCommands()
{
    IConsoleManager& ConsoleManager = IConsoleManager::Get();
    for (IConsoleObject* Command : ConsoleCommands)
    {
        if (Command)
        {
            ConsoleManager.UnregisterConsoleObject(Command);
        }
    }
    ConsoleCommands.Reset();
}

void FAgentProjectGraphModule::StartHttpServer()
{
    int32 PortArg = 0;
    if (FParse::Value(FCommandLine::Get(), TEXT("APGPort="), PortArg) && PortArg > 0)
    {
        ListenPort = static_cast<uint32>(PortArg);
    }

    const FString EnvPort = FPlatformMisc::GetEnvironmentVariable(TEXT("APG_BRIDGE_PORT"));
    if (!EnvPort.IsEmpty())
    {
        const int32 Parsed = FCString::Atoi(*EnvPort);
        if (Parsed > 0)
        {
            ListenPort = static_cast<uint32>(Parsed);
        }
    }

    FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
    Router = HttpServerModule.GetHttpRouter(ListenPort);
    BindRoutes();
    HttpServerModule.StartAllListeners();

    UE_LOG(LogTemp, Display, TEXT("AgentProjectGraph bridge listening on http://127.0.0.1:%u"), ListenPort);
}

void FAgentProjectGraphModule::StopHttpServer()
{
    if (!Router.IsValid())
    {
        return;
    }

    for (const FHttpRouteHandle& Handle : RouteHandles)
    {
        Router->UnbindRoute(Handle);
    }
    RouteHandles.Reset();
    Router.Reset();
}

void FAgentProjectGraphModule::BindRoutes()
{
    check(Router.IsValid());

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/agent-project-graph/health")), EHttpServerRequestVerbs::VERB_GET,
        FHttpRequestHandler::CreateRaw(this, &FAgentProjectGraphModule::HandleHealth)));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/agent-project-graph/export-all")), EHttpServerRequestVerbs::VERB_POST,
        FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/agent-project-graph/export-all"), Request, OnComplete, [this](const TSharedPtr<FJsonObject>&)
            {
                return ExportAll();
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/agent-project-graph/export-assets")), EHttpServerRequestVerbs::VERB_POST,
        FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/agent-project-graph/export-assets"), Request, OnComplete, [this](const TSharedPtr<FJsonObject>&)
            {
                return ExportAssets();
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/agent-project-graph/export-blueprint")), EHttpServerRequestVerbs::VERB_POST,
        FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/agent-project-graph/export-blueprint"), Request, OnComplete, [this](const TSharedPtr<FJsonObject>& Body)
            {
                FString AssetPath;
                Body->TryGetStringField(TEXT("assetPath"), AssetPath);
                return ExportBlueprint(AssetPath);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/agent-project-graph/export-materials")), EHttpServerRequestVerbs::VERB_POST,
        FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/agent-project-graph/export-materials"), Request, OnComplete, [this](const TSharedPtr<FJsonObject>&)
            {
                return ExportAllMaterials();
            });
        })));
}

void FAgentProjectGraphModule::HandleExportAllCommand(const TArray<FString>& Args)
{
    UE_LOG(LogTemp, Display, TEXT("%s"), *JsonToString(ExportAll()));
}

void FAgentProjectGraphModule::HandleExportBlueprintCommand(const TArray<FString>& Args)
{
    if (Args.Num() < 1)
    {
        UE_LOG(LogTemp, Warning, TEXT("Usage: AgentProjectGraph.ExportBlueprint /Game/Path/BP_Name"));
        return;
    }
    UE_LOG(LogTemp, Display, TEXT("%s"), *JsonToString(ExportBlueprint(Args[0])));
}

void FAgentProjectGraphModule::HandleExportAssetsCommand(const TArray<FString>& Args)
{
    UE_LOG(LogTemp, Display, TEXT("%s"), *JsonToString(ExportAssets()));
}

void FAgentProjectGraphModule::HandleExportMaterialsCommand(const TArray<FString>& Args)
{
    UE_LOG(LogTemp, Display, TEXT("%s"), *JsonToString(ExportAllMaterials()));
}

bool FAgentProjectGraphModule::HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    TSharedPtr<FJsonObject> Json = MakeOk();
    Json->SetStringField(TEXT("name"), TEXT("AgentProjectGraph"));
    Json->SetStringField(TEXT("version"), TEXT("0.1.0"));
    Json->SetNumberField(TEXT("port"), ListenPort);
    SendJson(OnComplete, Json);
    return true;
}

bool FAgentProjectGraphModule::HandleJsonRoute(
    const FString& RouteName,
    const FHttpServerRequest& Request,
    const FHttpResultCallback& OnComplete,
    TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>& Body)> Handler)
{
    const FUTF8ToTCHAR BodyChars(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
    const FString BodyText(BodyChars.Length(), BodyChars.Get());
    TSharedPtr<FJsonObject> Body;
    FString Error;
    if (!JsonFromString(BodyText, Body, Error))
    {
        TSharedPtr<FJsonObject> Json = MakeWarningResult(FString::Printf(TEXT("%s: %s"), *RouteName, *Error));
        SendJson(OnComplete, Json, EHttpServerResponseCodes::BadRequest);
        return true;
    }

    TSharedPtr<FJsonObject> Json;
    Json = Handler(Body);

    if (!Json.IsValid())
    {
        Json = MakeWarningResult(FString::Printf(TEXT("%s did not return a JSON result."), *RouteName));
    }

    SendJson(OnComplete, Json);
    return true;
}

void FAgentProjectGraphModule::SendJson(const FHttpResultCallback& OnComplete, const TSharedPtr<FJsonObject>& Json, EHttpServerResponseCodes Code) const
{
    TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(JsonToString(Json), TEXT("application/json"));
    Response->Code = Code;
    OnComplete(MoveTemp(Response));
}

TSharedPtr<FJsonObject> FAgentProjectGraphModule::ExportAll()
{
    TSharedPtr<FJsonObject> Json = MakeOk();
    TArray<TSharedPtr<FJsonValue>> Warnings;

    TSharedPtr<FJsonObject> Assets = ExportAssets();
    TSharedPtr<FJsonObject> Blueprints = ExportAllBlueprints();
    TSharedPtr<FJsonObject> Materials = ExportAllMaterials();

    Json->SetObjectField(TEXT("assets"), Assets);
    Json->SetObjectField(TEXT("blueprints"), Blueprints);
    Json->SetObjectField(TEXT("materials"), Materials);

    auto AppendWarnings = [&Warnings](const TSharedPtr<FJsonObject>& Result)
    {
        const TArray<TSharedPtr<FJsonValue>>* SourceWarnings = nullptr;
        if (Result.IsValid() && Result->TryGetArrayField(TEXT("warnings"), SourceWarnings) && SourceWarnings)
        {
            Warnings.Append(*SourceWarnings);
        }
    };

    AppendWarnings(Assets);
    AppendWarnings(Blueprints);
    AppendWarnings(Materials);
    Json->SetArrayField(TEXT("warnings"), Warnings);
    return Json;
}

TSharedPtr<FJsonObject> FAgentProjectGraphModule::ExportAssets()
{
    TSharedPtr<FJsonObject> Json = MakeOk();
    TArray<TSharedPtr<FJsonValue>> Warnings;
    TArray<TSharedPtr<FJsonValue>> Assets;
    TMap<FString, int32> ClassCounts;

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(TEXT("/Game")));
    Filter.bRecursivePaths = true;

    TArray<FAssetData> AssetDataList;
    if (!AssetRegistry().GetAssets(Filter, AssetDataList))
    {
        AddWarning(Warnings, TEXT("AssetRegistry GetAssets returned false."));
    }

    for (const FAssetData& AssetData : AssetDataList)
    {
        Assets.Add(MakeShared<FJsonValueObject>(AssetSummaryJson(AssetData)));
        ClassCounts.FindOrAdd(AssetData.AssetClassPath.ToString()) += 1;
    }

    TSharedPtr<FJsonObject> ClassCountsJson = MakeShared<FJsonObject>();
    for (const TPair<FString, int32>& Pair : ClassCounts)
    {
        ClassCountsJson->SetNumberField(Pair.Key, Pair.Value);
    }

    Json->SetArrayField(TEXT("assets"), Assets);
    Json->SetNumberField(TEXT("assetCount"), Assets.Num());
    Json->SetObjectField(TEXT("classCounts"), ClassCountsJson);
    Json->SetArrayField(TEXT("warnings"), Warnings);

    FString OutputPath;
    SaveJsonFile(CacheRoot(AssetRegistryCacheDirName), TEXT("assets.json"), Json, OutputPath, Warnings);
    Json->SetStringField(TEXT("outputPath"), OutputPath);
    Json->SetArrayField(TEXT("warnings"), Warnings);
    return Json;
}

TSharedPtr<FJsonObject> FAgentProjectGraphModule::ExportAllBlueprints()
{
    TSharedPtr<FJsonObject> Json = MakeOk();
    TArray<TSharedPtr<FJsonValue>> Warnings;
    TArray<TSharedPtr<FJsonValue>> Outputs;

    TArray<FAssetData> BlueprintAssets;
    if (!GetAssetsByClass(UBlueprint::StaticClass(), BlueprintAssets))
    {
        AddWarning(Warnings, TEXT("AssetRegistry failed to query Blueprint assets."));
    }

    for (const FAssetData& AssetData : BlueprintAssets)
    {
        TSharedPtr<FJsonObject> Result = ExportBlueprint(AssetData.GetObjectPathString());
        Outputs.Add(MakeShared<FJsonValueObject>(Result));
        const TArray<TSharedPtr<FJsonValue>>* ResultWarnings = nullptr;
        if (Result->TryGetArrayField(TEXT("warnings"), ResultWarnings) && ResultWarnings)
        {
            Warnings.Append(*ResultWarnings);
        }
    }

    Json->SetNumberField(TEXT("exportedCount"), Outputs.Num());
    Json->SetArrayField(TEXT("outputs"), Outputs);
    Json->SetArrayField(TEXT("warnings"), Warnings);
    return Json;
}

TSharedPtr<FJsonObject> FAgentProjectGraphModule::ExportBlueprint(const FString& AssetPath)
{
    TArray<TSharedPtr<FJsonValue>> Warnings;
    UObject* Object = LoadAssetObject(AssetPath, Warnings);
    UBlueprint* Blueprint = Cast<UBlueprint>(Object);
    if (!Blueprint)
    {
        FString ClassName = Object ? Object->GetClass()->GetPathName() : TEXT("<null>");
        AddWarning(Warnings, FString::Printf(TEXT("Asset is not a UBlueprint: %s (%s)"), *AssetPath, *ClassName));
        TSharedPtr<FJsonObject> Json = MakeOk();
        Json->SetBoolField(TEXT("success"), false);
        Json->SetStringField(TEXT("assetPath"), AssetPath);
        Json->SetArrayField(TEXT("warnings"), Warnings);
        return Json;
    }

    TSharedPtr<FJsonObject> Ir = BlueprintIrJson(Blueprint, Blueprint->GetPathName());
    FString OutputPath;
    const bool bSaved = SaveJsonFile(
        CacheRoot(BlueprintCacheDirName),
        SanitizedFileName(Blueprint->GetPathName()) + TEXT(".json"),
        Ir,
        OutputPath,
        Warnings);

    TSharedPtr<FJsonObject> Json = MakeOk();
    Json->SetBoolField(TEXT("success"), bSaved);
    Json->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    Json->SetStringField(TEXT("outputPath"), OutputPath);
    Json->SetObjectField(TEXT("blueprint"), Ir);
    Json->SetArrayField(TEXT("warnings"), Warnings);
    return Json;
}

TSharedPtr<FJsonObject> FAgentProjectGraphModule::ExportAllMaterials()
{
    TSharedPtr<FJsonObject> Json = MakeOk();
    TArray<TSharedPtr<FJsonValue>> Warnings;
    TArray<TSharedPtr<FJsonValue>> Outputs;

    TArray<FAssetData> MaterialAssets;
    if (!GetAssetsByClass(UMaterial::StaticClass(), MaterialAssets))
    {
        AddWarning(Warnings, TEXT("AssetRegistry failed to query Material assets."));
    }

    for (const FAssetData& AssetData : MaterialAssets)
    {
        TSharedPtr<FJsonObject> Result = ExportMaterial(AssetData.GetObjectPathString());
        Outputs.Add(MakeShared<FJsonValueObject>(Result));
        const TArray<TSharedPtr<FJsonValue>>* ResultWarnings = nullptr;
        if (Result->TryGetArrayField(TEXT("warnings"), ResultWarnings) && ResultWarnings)
        {
            Warnings.Append(*ResultWarnings);
        }
    }

    Json->SetNumberField(TEXT("exportedCount"), Outputs.Num());
    Json->SetArrayField(TEXT("outputs"), Outputs);
    Json->SetArrayField(TEXT("warnings"), Warnings);
    return Json;
}

TSharedPtr<FJsonObject> FAgentProjectGraphModule::ExportMaterial(const FString& AssetPath)
{
    TArray<TSharedPtr<FJsonValue>> Warnings;
    UObject* Object = LoadAssetObject(AssetPath, Warnings);
    UMaterial* Material = Cast<UMaterial>(Object);
    if (!Material)
    {
        FString ClassName = Object ? Object->GetClass()->GetPathName() : TEXT("<null>");
        AddWarning(Warnings, FString::Printf(TEXT("Asset is not a UMaterial: %s (%s)"), *AssetPath, *ClassName));
        TSharedPtr<FJsonObject> Json = MakeOk();
        Json->SetBoolField(TEXT("success"), false);
        Json->SetStringField(TEXT("assetPath"), AssetPath);
        Json->SetArrayField(TEXT("warnings"), Warnings);
        return Json;
    }

    TSharedPtr<FJsonObject> Ir = MaterialIrJson(Material, Material->GetPathName());
    FString OutputPath;
    const bool bSaved = SaveJsonFile(
        CacheRoot(MaterialCacheDirName),
        SanitizedFileName(Material->GetPathName()) + TEXT(".json"),
        Ir,
        OutputPath,
        Warnings);

    TSharedPtr<FJsonObject> Json = MakeOk();
    Json->SetBoolField(TEXT("success"), bSaved);
    Json->SetStringField(TEXT("assetPath"), Material->GetPathName());
    Json->SetStringField(TEXT("outputPath"), OutputPath);
    Json->SetObjectField(TEXT("material"), Ir);
    Json->SetArrayField(TEXT("warnings"), Warnings);
    return Json;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAgentProjectGraphModule, AgentProjectGraph)
