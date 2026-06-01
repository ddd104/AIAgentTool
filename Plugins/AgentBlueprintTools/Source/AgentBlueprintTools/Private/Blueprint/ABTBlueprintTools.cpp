#include "Blueprint/ABTBlueprintTools.h"
#include "Blueprint/Ops/ABTActorBlueprintOps.h"
#include "Blueprint/Ops/ABTGraphBlueprintOps.h"
#include "Blueprint/Ops/ABTWidgetBlueprintOps.h"
#include "Blueprint/Utils/ABTBlueprintGraphUtils.h"
#include "Utils/ABTJson.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "BlueprintActionDatabase.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/ActorComponent.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/Widget.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Curves/RichCurve.h"
#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/TimelineTemplate.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopedSlowTask.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "ScopedTransaction.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Styling/SlateBrush.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Editor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "WidgetBlueprint.h"

namespace
{
    bool HasField(const TSharedPtr<FJsonObject>& Object, const FString& Field)
    {
        return Object.IsValid() && Object->HasField(Field);
    }

    void RequireField(const TSharedPtr<FJsonObject>& Op, const FString& OpName, const FString& Field, TArray<FString>& OutMessages)
    {
        if (!HasField(Op, Field) || ABTJson::GetString(Op, Field).IsEmpty())
        {
            OutMessages.Add(FString::Printf(TEXT("%s.%s is required."), *OpName, *Field));
        }
    }

    void ValidateBlueprintOperation(const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages)
    {
        if (!Op.IsValid())
        {
            OutMessages.Add(TEXT("Each operation must be an object."));
            return;
        }

        const FString OpName = ABTJson::GetString(Op, TEXT("op"));
        if (OpName.IsEmpty())
        {
            OutMessages.Add(TEXT("operation.op is required."));
            return;
        }

        if (OpName == TEXT("ensure_variable"))
        {
            RequireField(Op, OpName, TEXT("name"), OutMessages);
            RequireField(Op, OpName, TEXT("type"), OutMessages);
        }
        else if (OpName == TEXT("ensure_function") || OpName == TEXT("ensure_move_function"))
        {
            RequireField(Op, OpName, TEXT("name"), OutMessages);
        }
        else if (OpName == TEXT("ensure_component"))
        {
            RequireField(Op, OpName, TEXT("name"), OutMessages);
        }
        else if (OpName == TEXT("set_static_mesh"))
        {
            RequireField(Op, OpName, TEXT("component"), OutMessages);
            RequireField(Op, OpName, TEXT("mesh"), OutMessages);
        }
        else if (OpName == TEXT("set_component_material"))
        {
            RequireField(Op, OpName, TEXT("component"), OutMessages);
            RequireField(Op, OpName, TEXT("material"), OutMessages);
        }
        else if (OpName == TEXT("add_node"))
        {
            RequireField(Op, OpName, TEXT("id"), OutMessages);
            RequireField(Op, OpName, TEXT("type"), OutMessages);
        }
        else if (OpName == TEXT("connect_exec") || OpName == TEXT("connect_data"))
        {
            RequireField(Op, OpName, TEXT("from"), OutMessages);
            RequireField(Op, OpName, TEXT("to"), OutMessages);
        }
        else if (OpName == TEXT("set_pin_default"))
        {
            RequireField(Op, OpName, TEXT("target"), OutMessages);
        }
        else if (OpName == TEXT("remove_node"))
        {
            if (!HasField(Op, TEXT("node")) && !HasField(Op, TEXT("target")))
            {
                OutMessages.Add(TEXT("remove_node.node or remove_node.target is required."));
            }
        }
        else if (OpName == TEXT("layout_blueprint_graph"))
        {
            // graph defaults to EventGraph; nodes is optional so callers can run overlap cleanup only.
        }
        else if (OpName == TEXT("configure_timeline"))
        {
            if (!HasField(Op, TEXT("name")) && !HasField(Op, TEXT("timeline")))
            {
                OutMessages.Add(TEXT("configure_timeline.name or configure_timeline.timeline is required."));
            }
        }
        else if (OpName == TEXT("set_blueprint_property"))
        {
            RequireField(Op, OpName, TEXT("property"), OutMessages);
            if (!HasField(Op, TEXT("value")))
            {
                OutMessages.Add(TEXT("set_blueprint_property.value is required."));
            }
        }
        else if (OpName == TEXT("ensure_timer_loop"))
        {
            RequireField(Op, OpName, TEXT("function"), OutMessages);
        }
        else if (OpName == TEXT("ensure_looping_move"))
        {
            // function is optional here; the op defaults to MoveByDelta.
        }
        else if (OpName == TEXT("configure_interactable_actor"))
        {
            // Mesh, collision, prompt, key, timeline, and rotation fields all have reusable defaults.
        }
        else if (OpName == TEXT("configure_button_pages_widget"))
        {
            // pages is optional; the tool supplies useful defaults for quick UMG prototypes.
        }
        else if (OpName == TEXT("configure_figma_widget"))
        {
            if (!HasField(Op, TEXT("root")) && !HasField(Op, TEXT("children")))
            {
                OutMessages.Add(TEXT("configure_figma_widget.root or configure_figma_widget.children is required."));
            }
        }
        else if (OpName == TEXT("configure_message_plate_widget"))
        {
            RequireField(Op, OpName, TEXT("channel"), OutMessages);
            RequireField(Op, OpName, TEXT("payloadStruct"), OutMessages);
            RequireField(Op, OpName, TEXT("trueTexture"), OutMessages);
            RequireField(Op, OpName, TEXT("falseTexture"), OutMessages);
        }
        else
        {
            OutMessages.Add(FString::Printf(TEXT("Unsupported op: %s."), *OpName));
        }
    }

    TSharedPtr<FJsonValue> Vector2DValue(const FVector2D& Value)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        Array.Add(MakeShared<FJsonValueNumber>(Value.X));
        Array.Add(MakeShared<FJsonValueNumber>(Value.Y));
        return MakeShared<FJsonValueArray>(Array);
    }

    TSharedPtr<FJsonValue> ColorValue(const FLinearColor& Value)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        Array.Add(MakeShared<FJsonValueNumber>(Value.R));
        Array.Add(MakeShared<FJsonValueNumber>(Value.G));
        Array.Add(MakeShared<FJsonValueNumber>(Value.B));
        Array.Add(MakeShared<FJsonValueNumber>(Value.A));
        return MakeShared<FJsonValueArray>(Array);
    }

    TSharedPtr<FJsonValue> VectorValue(const FVector& Value)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        Array.Add(MakeShared<FJsonValueNumber>(Value.X));
        Array.Add(MakeShared<FJsonValueNumber>(Value.Y));
        Array.Add(MakeShared<FJsonValueNumber>(Value.Z));
        return MakeShared<FJsonValueArray>(Array);
    }

    TArray<TSharedPtr<FJsonValue>> ExportFloatCurveKeys(const FRichCurve& Curve)
    {
        TArray<TSharedPtr<FJsonValue>> Keys;
        for (const FRichCurveKey& Key : Curve.GetConstRefOfKeys())
        {
            TSharedPtr<FJsonObject> KeyJson = ABTJson::Object();
            KeyJson->SetNumberField(TEXT("time"), Key.Time);
            KeyJson->SetNumberField(TEXT("value"), Key.Value);
            Keys.Add(ABTJson::ObjectValue(KeyJson));
        }
        return Keys;
    }

    TArray<TSharedPtr<FJsonValue>> ExportVectorCurveKeys(const UCurveVector* Curve)
    {
        TArray<TSharedPtr<FJsonValue>> Keys;
        if (!Curve)
        {
            return Keys;
        }

        TSet<float> Times;
        for (int32 ComponentIndex = 0; ComponentIndex < 3; ++ComponentIndex)
        {
            for (const FRichCurveKey& Key : Curve->FloatCurves[ComponentIndex].GetConstRefOfKeys())
            {
                Times.Add(Key.Time);
            }
        }

        TArray<float> SortedTimes = Times.Array();
        SortedTimes.Sort();
        for (float Time : SortedTimes)
        {
            TSharedPtr<FJsonObject> KeyJson = ABTJson::Object();
            KeyJson->SetNumberField(TEXT("time"), Time);
            KeyJson->SetField(TEXT("value"), VectorValue(Curve->GetVectorValue(Time)));
            Keys.Add(ABTJson::ObjectValue(KeyJson));
        }
        return Keys;
    }

    TArray<TSharedPtr<FJsonValue>> ExportColorCurveKeys(const UCurveLinearColor* Curve)
    {
        TArray<TSharedPtr<FJsonValue>> Keys;
        if (!Curve)
        {
            return Keys;
        }

        TSet<float> Times;
        for (int32 ComponentIndex = 0; ComponentIndex < 4; ++ComponentIndex)
        {
            for (const FRichCurveKey& Key : Curve->FloatCurves[ComponentIndex].GetConstRefOfKeys())
            {
                Times.Add(Key.Time);
            }
        }

        TArray<float> SortedTimes = Times.Array();
        SortedTimes.Sort();
        for (float Time : SortedTimes)
        {
            TSharedPtr<FJsonObject> KeyJson = ABTJson::Object();
            KeyJson->SetNumberField(TEXT("time"), Time);
            KeyJson->SetField(TEXT("value"), ColorValue(Curve->GetLinearColorValue(Time)));
            Keys.Add(ABTJson::ObjectValue(KeyJson));
        }
        return Keys;
    }

    FString BrushResourcePath(const FSlateBrush& Brush)
    {
        const UObject* Resource = Brush.GetResourceObject();
        return Resource ? Resource->GetPathName() : FString();
    }

    TArray<TSharedPtr<FJsonValue>> ExportChildNames(const UPanelWidget* Panel)
    {
        TArray<TSharedPtr<FJsonValue>> Children;
        if (!Panel)
        {
            return Children;
        }

        for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
        {
            if (const UWidget* Child = Panel->GetChildAt(Index))
            {
                Children.Add(ABTJson::StringValue(Child->GetName()));
            }
        }
        return Children;
    }

    FString BlueprintNodeKind(const UEdGraphNode* Node)
    {
        if (Cast<UK2Node_CallFunction>(Node)) return TEXT("CallFunction");
        if (Cast<UK2Node_IfThenElse>(Node)) return TEXT("Branch");
        if (Cast<UK2Node_VariableGet>(Node)) return TEXT("GetVariable");
        if (Cast<UK2Node_VariableSet>(Node)) return TEXT("SetVariable");
        if (Cast<UK2Node_CustomEvent>(Node)) return TEXT("CustomEvent");
        if (Cast<UK2Node_Event>(Node)) return TEXT("Event");
        if (Cast<UK2Node_FunctionEntry>(Node)) return TEXT("FunctionEntry");
        return TEXT("Node");
    }

    TSharedPtr<FJsonObject> ExportNodeBrief(const UEdGraphNode* Node)
    {
        TSharedPtr<FJsonObject> Json = ABTJson::Object();
        if (!Node)
        {
            return Json;
        }

        Json->SetStringField(TEXT("id"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
        Json->SetStringField(TEXT("object_name"), Node->GetName());
        Json->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        Json->SetStringField(TEXT("kind"), BlueprintNodeKind(Node));
        return Json;
    }

    void AddUniqueString(TArray<TSharedPtr<FJsonValue>>& Values, TSet<FString>& Seen, const FString& Value)
    {
        if (Value.IsEmpty() || Seen.Contains(Value))
        {
            return;
        }
        Seen.Add(Value);
        Values.Add(ABTJson::StringValue(Value));
    }

    void AddUniqueObjectByKey(TArray<TSharedPtr<FJsonValue>>& Values, TSet<FString>& Seen, const FString& Key, const TSharedPtr<FJsonObject>& Object)
    {
        if (Key.IsEmpty() || Seen.Contains(Key) || !Object.IsValid())
        {
            return;
        }
        Seen.Add(Key);
        Values.Add(ABTJson::ObjectValue(Object));
    }

    bool IsExecPin(const UEdGraphPin* Pin)
    {
        return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
    }

    bool IsGraphEntryNode(const UEdGraphNode* Node)
    {
        return Cast<UK2Node_Event>(Node) || Cast<UK2Node_CustomEvent>(Node) || Cast<UK2Node_FunctionEntry>(Node);
    }

    TSharedPtr<FJsonObject> ExportExecEdge(const UEdGraphPin* FromPin, const UEdGraphPin* ToPin)
    {
        TSharedPtr<FJsonObject> Edge = ABTJson::Object();
        const UEdGraphNode* FromNode = FromPin ? FromPin->GetOwningNode() : nullptr;
        const UEdGraphNode* ToNode = ToPin ? ToPin->GetOwningNode() : nullptr;
        Edge->SetStringField(TEXT("from_node"), FromNode ? FromNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString());
        Edge->SetStringField(TEXT("from_pin"), FromPin ? FromPin->PinName.ToString() : FString());
        Edge->SetStringField(TEXT("to_node"), ToNode ? ToNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString());
        Edge->SetStringField(TEXT("to_pin"), ToPin ? ToPin->PinName.ToString() : FString());
        return Edge;
    }

    void AddExecPathResult(
        TArray<TSharedPtr<FJsonValue>>& OutPaths,
        const UEdGraphNode* EntryNode,
        const TArray<TSharedPtr<FJsonValue>>& PathNodes,
        const TArray<TSharedPtr<FJsonValue>>& PathEdges,
        bool bTruncated,
        bool bCycle)
    {
        TSharedPtr<FJsonObject> Path = ABTJson::Object();
        Path->SetStringField(TEXT("entry"), EntryNode ? EntryNode->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString());
        Path->SetArrayField(TEXT("nodes"), PathNodes);
        Path->SetArrayField(TEXT("edges"), PathEdges);
        Path->SetBoolField(TEXT("truncated"), bTruncated);
        Path->SetBoolField(TEXT("cycle"), bCycle);
        OutPaths.Add(ABTJson::ObjectValue(Path));
    }

    void WalkExecPaths(
        UEdGraphNode* EntryNode,
        UEdGraphNode* Node,
        TSet<UEdGraphNode*> Visited,
        TArray<TSharedPtr<FJsonValue>> PathNodes,
        TArray<TSharedPtr<FJsonValue>> PathEdges,
        TArray<TSharedPtr<FJsonValue>>& OutPaths,
        int32 Depth)
    {
        constexpr int32 MaxDepth = 64;
        constexpr int32 MaxPaths = 64;
        if (!Node || OutPaths.Num() >= MaxPaths)
        {
            return;
        }

        PathNodes.Add(ABTJson::ObjectValue(ExportNodeBrief(Node)));

        if (Visited.Contains(Node))
        {
            AddExecPathResult(OutPaths, EntryNode, PathNodes, PathEdges, false, true);
            return;
        }

        if (Depth >= MaxDepth)
        {
            AddExecPathResult(OutPaths, EntryNode, PathNodes, PathEdges, true, false);
            return;
        }

        Visited.Add(Node);

        bool bHasNext = false;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output || !IsExecPin(Pin))
            {
                continue;
            }

            for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                if (!LinkedPin || !IsExecPin(LinkedPin))
                {
                    continue;
                }

                UEdGraphNode* NextNode = LinkedPin->GetOwningNode();
                if (!NextNode)
                {
                    continue;
                }

                bHasNext = true;
                TArray<TSharedPtr<FJsonValue>> BranchEdges = PathEdges;
                BranchEdges.Add(ABTJson::ObjectValue(ExportExecEdge(Pin, LinkedPin)));
                WalkExecPaths(EntryNode, NextNode, Visited, PathNodes, BranchEdges, OutPaths, Depth + 1);
            }
        }

        if (!bHasNext)
        {
            AddExecPathResult(OutPaths, EntryNode, PathNodes, PathEdges, false, false);
        }
    }

    TSharedPtr<FJsonObject> BuildGraphAnalysis(UBlueprint* Blueprint, UEdGraph* Graph)
    {
        TSharedPtr<FJsonObject> Analysis = ABTJson::Object();
        if (!Graph)
        {
            return Analysis;
        }

        TSet<FString> ComponentNames;
        if (Blueprint && Blueprint->SimpleConstructionScript)
        {
            for (USCS_Node* SCSNode : Blueprint->SimpleConstructionScript->GetAllNodes())
            {
                if (SCSNode)
                {
                    ComponentNames.Add(SCSNode->GetVariableName().ToString());
                }
            }
        }

        TArray<TSharedPtr<FJsonValue>> EntryPoints;
        TArray<TSharedPtr<FJsonValue>> Reads;
        TArray<TSharedPtr<FJsonValue>> Writes;
        TArray<TSharedPtr<FJsonValue>> Calls;
        TArray<TSharedPtr<FJsonValue>> ComponentTouches;
        TArray<TSharedPtr<FJsonValue>> ExecEdges;
        TArray<TSharedPtr<FJsonValue>> ExecPaths;
        TSet<FString> ReadSeen;
        TSet<FString> WriteSeen;
        TSet<FString> CallSeen;
        TSet<FString> ComponentSeen;
        TArray<UEdGraphNode*> EntryNodes;

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }

            if (IsGraphEntryNode(Node))
            {
                EntryNodes.Add(Node);
                EntryPoints.Add(ABTJson::ObjectValue(ExportNodeBrief(Node)));
            }

            if (UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node))
            {
                const FString VariableName = Get->GetVarNameString();
                AddUniqueString(Reads, ReadSeen, VariableName);
                if (ComponentNames.Contains(VariableName))
                {
                    TSharedPtr<FJsonObject> Touch = ExportNodeBrief(Node);
                    Touch->SetStringField(TEXT("component"), VariableName);
                    Touch->SetStringField(TEXT("access"), TEXT("read"));
                    AddUniqueObjectByKey(ComponentTouches, ComponentSeen, VariableName + TEXT("|read|") + Touch->GetStringField(TEXT("id")), Touch);
                }
            }

            if (UK2Node_VariableSet* Set = Cast<UK2Node_VariableSet>(Node))
            {
                const FString VariableName = Set->GetVarNameString();
                AddUniqueString(Writes, WriteSeen, VariableName);
                if (ComponentNames.Contains(VariableName))
                {
                    TSharedPtr<FJsonObject> Touch = ExportNodeBrief(Node);
                    Touch->SetStringField(TEXT("component"), VariableName);
                    Touch->SetStringField(TEXT("access"), TEXT("write"));
                    AddUniqueObjectByKey(ComponentTouches, ComponentSeen, VariableName + TEXT("|write|") + Touch->GetStringField(TEXT("id")), Touch);
                }
            }

            if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
            {
                TSharedPtr<FJsonObject> CallJson = ExportNodeBrief(Node);
                const FString FunctionName = Call->FunctionReference.GetMemberName().ToString();
                UClass* OwnerClass = Call->FunctionReference.GetMemberParentClass(Call->GetBlueprintClassFromNode());
                CallJson->SetStringField(TEXT("function"), FunctionName);
                CallJson->SetStringField(TEXT("owner_class"), OwnerClass ? OwnerClass->GetPathName() : FString());
                AddUniqueObjectByKey(Calls, CallSeen, (OwnerClass ? OwnerClass->GetPathName() : FString()) + TEXT("::") + FunctionName + TEXT("|") + CallJson->GetStringField(TEXT("id")), CallJson);

                if (FunctionName.Contains(TEXT("Component")) || FunctionName.Contains(TEXT("Material")) || FunctionName.Contains(TEXT("StaticMesh")) || FunctionName.Contains(TEXT("Visibility")))
                {
                    TSharedPtr<FJsonObject> Touch = ExportNodeBrief(Node);
                    Touch->SetStringField(TEXT("component"), TEXT("<call-target>"));
                    Touch->SetStringField(TEXT("access"), FunctionName);
                    AddUniqueObjectByKey(ComponentTouches, ComponentSeen, Touch->GetStringField(TEXT("id")) + TEXT("|call"), Touch);
                }
            }

            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output || !IsExecPin(Pin))
                {
                    continue;
                }

                for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    if (LinkedPin && IsExecPin(LinkedPin))
                    {
                        ExecEdges.Add(ABTJson::ObjectValue(ExportExecEdge(Pin, LinkedPin)));
                    }
                }
            }
        }

        for (UEdGraphNode* EntryNode : EntryNodes)
        {
            WalkExecPaths(EntryNode, EntryNode, TSet<UEdGraphNode*>(), TArray<TSharedPtr<FJsonValue>>(), TArray<TSharedPtr<FJsonValue>>(), ExecPaths, 0);
            if (ExecPaths.Num() >= 64)
            {
                break;
            }
        }

        Analysis->SetArrayField(TEXT("entry_points"), EntryPoints);
        Analysis->SetArrayField(TEXT("variable_reads"), Reads);
        Analysis->SetArrayField(TEXT("variable_writes"), Writes);
        Analysis->SetArrayField(TEXT("external_calls"), Calls);
        Analysis->SetArrayField(TEXT("component_touches"), ComponentTouches);
        Analysis->SetArrayField(TEXT("execution_edges"), ExecEdges);
        Analysis->SetArrayField(TEXT("execution_paths"), ExecPaths);
        return Analysis;
    }

    TSharedPtr<FJsonObject> ExportWidgetSlot(UWidget* Widget)
    {
        TSharedPtr<FJsonObject> SlotJson = ABTJson::Object();
        if (!Widget || !Widget->Slot)
        {
            return SlotJson;
        }

        SlotJson->SetStringField(TEXT("class"), Widget->Slot->GetClass()->GetPathName());
        if (Widget->Slot->Parent)
        {
            SlotJson->SetStringField(TEXT("parent"), Widget->Slot->Parent->GetName());
        }

        if (const UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot))
        {
            SlotJson->SetField(TEXT("position"), Vector2DValue(CanvasSlot->GetPosition()));
            SlotJson->SetField(TEXT("size"), Vector2DValue(CanvasSlot->GetSize()));
            SlotJson->SetField(TEXT("alignment"), Vector2DValue(CanvasSlot->GetAlignment()));
            SlotJson->SetBoolField(TEXT("auto_size"), CanvasSlot->GetAutoSize());
            SlotJson->SetNumberField(TEXT("z_order"), CanvasSlot->GetZOrder());
        }

        return SlotJson;
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
    return ABT::Blueprint::FindGraph(Blueprint, GraphName);
}

UEdGraph* FABTBlueprintTools::EnsureFunctionGraph(UBlueprint* Blueprint, const FString& GraphName)
{
    return ABT::Blueprint::EnsureFunctionGraph(Blueprint, GraphName);
}

TSharedPtr<FJsonObject> FABTBlueprintTools::ExportPin(UEdGraphPin* Pin)
{
    TSharedPtr<FJsonObject> Json = ABTJson::Object();
    if (!Pin) return Json;

    Json->SetStringField(TEXT("id"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
    Json->SetStringField(TEXT("name"), Pin->PinName.ToString());
    Json->SetStringField(TEXT("direction"), ABT::Blueprint::PinDirectionToString(Pin->Direction));
    Json->SetStringField(TEXT("type"), ABT::Blueprint::PinTypeToString(Pin->PinType));
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

TSharedPtr<FJsonObject> FABTBlueprintTools::ExportNode(UEdGraphNode* Node, bool bIncludePins)
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

    Json->SetNumberField(TEXT("pin_count"), Node->Pins.Num());
    if (bIncludePins)
    {
        TArray<TSharedPtr<FJsonValue>> Pins;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            Pins.Add(ABTJson::ObjectValue(ExportPin(Pin)));
        }
        Json->SetArrayField(TEXT("pins"), Pins);
    }
    return Json;
}

TSharedPtr<FJsonObject> FABTBlueprintTools::ExportGraph(UEdGraph* Graph, bool bIncludePins)
{
    TSharedPtr<FJsonObject> Json = ABTJson::Object();
    if (!Graph) return Json;

    Json->SetStringField(TEXT("name"), Graph->GetName());
    Json->SetStringField(TEXT("schema"), Graph->Schema ? Graph->Schema->GetClass()->GetPathName() : TEXT(""));

    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        Nodes.Add(ABTJson::ObjectValue(ExportNode(Node, bIncludePins)));
    }
    Json->SetArrayField(TEXT("nodes"), Nodes);
    return Json;
}

bool FABTBlueprintTools::ExportBlueprint(const FString& AssetPath, TSharedPtr<FJsonObject>& OutJson, FString& OutError, const FABTBlueprintExportOptions& Options)
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
        V->SetStringField(TEXT("type"), ABT::Blueprint::PinTypeToString(Var.VarType));
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

    TArray<TSharedPtr<FJsonValue>> Timelines;
    for (UTimelineTemplate* Timeline : Blueprint->Timelines)
    {
        if (!Timeline)
        {
            continue;
        }

        TSharedPtr<FJsonObject> TimelineJson = ABTJson::Object();
        TimelineJson->SetStringField(TEXT("name"), Timeline->GetVariableName().ToString());
        TimelineJson->SetNumberField(TEXT("length"), Timeline->TimelineLength);
        TimelineJson->SetBoolField(TEXT("autoplay"), Timeline->bAutoPlay);
        TimelineJson->SetBoolField(TEXT("loop"), Timeline->bLoop);

        TArray<TSharedPtr<FJsonValue>> FloatTracks;
        for (const FTTFloatTrack& Track : Timeline->FloatTracks)
        {
            TSharedPtr<FJsonObject> TrackJson = ABTJson::Object();
            TrackJson->SetStringField(TEXT("name"), Track.GetTrackName().ToString());
            TrackJson->SetBoolField(TEXT("external_curve"), Track.bIsExternalCurve);
            TrackJson->SetStringField(TEXT("curve"), Track.CurveFloat ? Track.CurveFloat->GetPathName() : TEXT(""));
            TrackJson->SetArrayField(TEXT("keys"), Track.CurveFloat ? ExportFloatCurveKeys(Track.CurveFloat->FloatCurve) : TArray<TSharedPtr<FJsonValue>>());
            FloatTracks.Add(ABTJson::ObjectValue(TrackJson));
        }
        TimelineJson->SetArrayField(TEXT("float_tracks"), FloatTracks);

        TArray<TSharedPtr<FJsonValue>> VectorTracks;
        for (const FTTVectorTrack& Track : Timeline->VectorTracks)
        {
            TSharedPtr<FJsonObject> TrackJson = ABTJson::Object();
            TrackJson->SetStringField(TEXT("name"), Track.GetTrackName().ToString());
            TrackJson->SetBoolField(TEXT("external_curve"), Track.bIsExternalCurve);
            TrackJson->SetStringField(TEXT("curve"), Track.CurveVector ? Track.CurveVector->GetPathName() : TEXT(""));
            TrackJson->SetArrayField(TEXT("keys"), ExportVectorCurveKeys(Track.CurveVector));
            VectorTracks.Add(ABTJson::ObjectValue(TrackJson));
        }
        TimelineJson->SetArrayField(TEXT("vector_tracks"), VectorTracks);

        TArray<TSharedPtr<FJsonValue>> EventTracks;
        for (const FTTEventTrack& Track : Timeline->EventTracks)
        {
            TSharedPtr<FJsonObject> TrackJson = ABTJson::Object();
            TrackJson->SetStringField(TEXT("name"), Track.GetTrackName().ToString());
            TrackJson->SetStringField(TEXT("function"), Track.GetFunctionName().ToString());
            TrackJson->SetBoolField(TEXT("external_curve"), Track.bIsExternalCurve);
            TrackJson->SetStringField(TEXT("curve"), Track.CurveKeys ? Track.CurveKeys->GetPathName() : TEXT(""));
            TrackJson->SetArrayField(TEXT("keys"), Track.CurveKeys ? ExportFloatCurveKeys(Track.CurveKeys->FloatCurve) : TArray<TSharedPtr<FJsonValue>>());
            EventTracks.Add(ABTJson::ObjectValue(TrackJson));
        }
        TimelineJson->SetArrayField(TEXT("event_tracks"), EventTracks);

        TArray<TSharedPtr<FJsonValue>> ColorTracks;
        for (const FTTLinearColorTrack& Track : Timeline->LinearColorTracks)
        {
            TSharedPtr<FJsonObject> TrackJson = ABTJson::Object();
            TrackJson->SetStringField(TEXT("name"), Track.GetTrackName().ToString());
            TrackJson->SetBoolField(TEXT("external_curve"), Track.bIsExternalCurve);
            TrackJson->SetStringField(TEXT("curve"), Track.CurveLinearColor ? Track.CurveLinearColor->GetPathName() : TEXT(""));
            TrackJson->SetArrayField(TEXT("keys"), ExportColorCurveKeys(Track.CurveLinearColor));
            ColorTracks.Add(ABTJson::ObjectValue(TrackJson));
        }
        TimelineJson->SetArrayField(TEXT("color_tracks"), ColorTracks);
        Timelines.Add(ABTJson::ObjectValue(TimelineJson));
    }
    OutJson->SetArrayField(TEXT("timelines"), Timelines);

    if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint))
    {
        TArray<TSharedPtr<FJsonValue>> Widgets;
        if (WidgetBlueprint->WidgetTree)
        {
            TArray<UWidget*> AllWidgets;
            WidgetBlueprint->WidgetTree->GetAllWidgets(AllWidgets);
            if (WidgetBlueprint->WidgetTree->RootWidget && !AllWidgets.Contains(WidgetBlueprint->WidgetTree->RootWidget))
            {
                AllWidgets.Insert(WidgetBlueprint->WidgetTree->RootWidget, 0);
            }
            for (UWidget* Widget : AllWidgets)
            {
                if (!Widget) continue;
                TSharedPtr<FJsonObject> W = ABTJson::Object();
                W->SetStringField(TEXT("name"), Widget->GetName());
                W->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName());
                W->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
                W->SetBoolField(TEXT("is_root"), WidgetBlueprint->WidgetTree->RootWidget == Widget);
                if (Widget->Slot && Widget->Slot->Parent)
                {
                    W->SetStringField(TEXT("parent"), Widget->Slot->Parent->GetName());
                }
                W->SetStringField(TEXT("visibility"), StaticEnum<ESlateVisibility>()->GetNameStringByValue(static_cast<int64>(Widget->GetVisibility())));
                W->SetNumberField(TEXT("render_opacity"), Widget->GetRenderOpacity());
                const FWidgetTransform Transform = Widget->GetRenderTransform();
                TArray<TSharedPtr<FJsonValue>> Scale;
                Scale.Add(MakeShared<FJsonValueNumber>(Transform.Scale.X));
                Scale.Add(MakeShared<FJsonValueNumber>(Transform.Scale.Y));
                W->SetArrayField(TEXT("render_scale"), Scale);
                W->SetObjectField(TEXT("slot"), ExportWidgetSlot(Widget));
                if (const UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
                {
                    W->SetArrayField(TEXT("children"), ExportChildNames(Panel));
                }
                if (const UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
                {
                    W->SetStringField(TEXT("text"), TextBlock->GetText().ToString());
                    W->SetNumberField(TEXT("font_size"), TextBlock->GetFont().Size);
                    W->SetField(TEXT("text_color"), ColorValue(TextBlock->GetColorAndOpacity().GetSpecifiedColor()));
                }
                if (const UImage* Image = Cast<UImage>(Widget))
                {
                    W->SetStringField(TEXT("brush_resource"), BrushResourcePath(Image->GetBrush()));
                    W->SetField(TEXT("image_color"), ColorValue(Image->GetColorAndOpacity()));
                }
                if (const UBorder* Border = Cast<UBorder>(Widget))
                {
                    W->SetField(TEXT("brush_color"), ColorValue(Border->GetBrushColor()));
                }
                Widgets.Add(ABTJson::ObjectValue(W));
            }
        }
        OutJson->SetArrayField(TEXT("widgets"), Widgets);

        TArray<TSharedPtr<FJsonValue>> Animations;
        for (const UWidgetAnimation* Animation : WidgetBlueprint->Animations)
        {
            if (!Animation)
            {
                continue;
            }

            TSharedPtr<FJsonObject> A = ABTJson::Object();
            A->SetStringField(TEXT("name"), Animation->GetName());
            TArray<TSharedPtr<FJsonValue>> WidgetBindings;
            for (const FWidgetAnimationBinding& Binding : Animation->GetBindings())
            {
                TSharedPtr<FJsonObject> B = ABTJson::Object();
                B->SetStringField(TEXT("widget_name"), Binding.WidgetName.ToString());
                B->SetStringField(TEXT("animation_guid"), Binding.AnimationGuid.ToString(EGuidFormats::DigitsWithHyphens));
                WidgetBindings.Add(ABTJson::ObjectValue(B));
            }
            A->SetArrayField(TEXT("widget_bindings"), WidgetBindings);

            if (const UMovieScene* MovieScene = Animation->GetMovieScene())
            {
                const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
                A->SetNumberField(TEXT("playback_start_frame"), PlaybackRange.GetLowerBoundValue().Value);
                A->SetNumberField(TEXT("playback_end_frame"), PlaybackRange.GetUpperBoundValue().Value);

                int32 ObjectTrackCount = 0;
                int32 EventKeyCount = 0;
                for (const FMovieSceneBinding& MovieSceneBinding : MovieScene->GetBindings())
                {
                    ObjectTrackCount += MovieSceneBinding.GetTracks().Num();
                }
                for (const UMovieSceneTrack* Track : MovieScene->GetTracks())
                {
                    if (const UMovieSceneEventTrack* EventTrack = Cast<UMovieSceneEventTrack>(Track))
                    {
                        for (const UMovieSceneSection* Section : EventTrack->GetAllSections())
                        {
                            if (const UMovieSceneEventTriggerSection* EventSection = Cast<UMovieSceneEventTriggerSection>(Section))
                            {
                                EventKeyCount += EventSection->EventChannel.GetNumKeys();
                            }
                        }
                    }
                }
                A->SetNumberField(TEXT("object_track_count"), ObjectTrackCount);
                A->SetNumberField(TEXT("event_key_count"), EventKeyCount);
            }
            Animations.Add(ABTJson::ObjectValue(A));
        }
        OutJson->SetArrayField(TEXT("animations"), Animations);
    }

    TSharedPtr<FJsonObject> Summary = ABTJson::Object();
    TArray<TSharedPtr<FJsonValue>> EntryPoints;
    TArray<TSharedPtr<FJsonValue>> Reads;
    TArray<TSharedPtr<FJsonValue>> Writes;
    TArray<TSharedPtr<FJsonValue>> Calls;
    TArray<TSharedPtr<FJsonValue>> GraphSummaries;
    TArray<TSharedPtr<FJsonValue>> GraphAnalyses;
    TArray<UEdGraph*> GraphsRaw;
    Blueprint->GetAllGraphs(GraphsRaw);
    for (UEdGraph* Graph : GraphsRaw)
    {
        if (!Graph) continue;
        TSharedPtr<FJsonObject> GraphSummary = ABTJson::Object();
        GraphSummary->SetStringField(TEXT("name"), Graph->GetName());
        GraphSummary->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
        GraphSummaries.Add(ABTJson::ObjectValue(GraphSummary));

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node)) EntryPoints.Add(ABTJson::StringValue(Event->EventReference.GetMemberName().ToString()));
            if (UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Node)) EntryPoints.Add(ABTJson::StringValue(Custom->CustomFunctionName.ToString()));
            if (UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node)) Reads.Add(ABTJson::StringValue(Get->GetVarNameString()));
            if (UK2Node_VariableSet* Set = Cast<UK2Node_VariableSet>(Node)) Writes.Add(ABTJson::StringValue(Set->GetVarNameString()));
            if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node)) Calls.Add(ABTJson::StringValue(Call->FunctionReference.GetMemberName().ToString()));
        }

        TSharedPtr<FJsonObject> GraphAnalysis = BuildGraphAnalysis(Blueprint, Graph);
        GraphAnalysis->SetStringField(TEXT("name"), Graph->GetName());
        GraphAnalyses.Add(ABTJson::ObjectValue(GraphAnalysis));
    }
    Summary->SetArrayField(TEXT("entry_points"), EntryPoints);
    Summary->SetArrayField(TEXT("variable_reads"), Reads);
    Summary->SetArrayField(TEXT("variable_writes"), Writes);
    Summary->SetArrayField(TEXT("external_calls"), Calls);
    Summary->SetNumberField(TEXT("graph_count"), GraphsRaw.Num());
    OutJson->SetObjectField(TEXT("semantic_summary"), Summary);
    OutJson->SetArrayField(TEXT("graph_summaries"), GraphSummaries);
    OutJson->SetArrayField(TEXT("graph_analyses"), GraphAnalyses);

    if (Options.bIncludeGraphs)
    {
        TArray<TSharedPtr<FJsonValue>> Graphs;
        for (UEdGraph* Graph : GraphsRaw)
        {
            Graphs.Add(ABTJson::ObjectValue(ExportGraph(Graph, Options.bIncludePins)));
        }
        OutJson->SetArrayField(TEXT("graphs"), Graphs);
    }
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
    OutJson->SetObjectField(TEXT("analysis"), BuildGraphAnalysis(Blueprint, Graph));
    OutJson->SetObjectField(TEXT("graph_ir"), ExportGraph(Graph, true));
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
    else
    {
        for (const TSharedPtr<FJsonValue>& Value : *Ops)
        {
            ValidateBlueprintOperation(Value->AsObject(), OutMessages);
        }
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
        return ABT::Blueprint::Ops::EnsureVariable(Blueprint, Op, OutMessages, OutError);
    }

    if (OpName == TEXT("ensure_function"))
    {
        return ABT::Blueprint::Ops::EnsureFunction(Blueprint, Op, NodeMap, OutMessages, OutError);
    }

    if (OpName == TEXT("ensure_move_function"))
    {
        return ABT::Blueprint::Ops::EnsureMoveFunction(Blueprint, Op, OutMessages, OutError);
    }

    if (OpName == TEXT("ensure_timer_loop"))
    {
        return ABT::Blueprint::Ops::EnsureTimerLoop(Blueprint, Op, OutMessages, OutError);
    }

    if (OpName == TEXT("ensure_looping_move"))
    {
        return ABT::Blueprint::Ops::EnsureLoopingMove(Blueprint, Op, OutMessages, OutError);
    }

    if (OpName == TEXT("configure_interactable_actor"))
    {
        return ABT::Blueprint::Ops::ConfigureInteractableActor(Blueprint, Op, OutMessages, OutError);
    }

    if (OpName == TEXT("ensure_component"))
    {
        return ABT::Blueprint::Ops::EnsureComponent(Blueprint, Op, OutMessages, OutError);
    }

    if (OpName == TEXT("configure_button_pages_widget"))
    {
        return ABT::Blueprint::Ops::ConfigureButtonPagesWidget(Blueprint, Op, OutMessages, OutError);
    }

    if (OpName == TEXT("configure_figma_widget"))
    {
        return ABT::Blueprint::Ops::ConfigureFigmaWidget(Blueprint, Op, OutMessages, OutError);
    }

    if (OpName == TEXT("configure_message_plate_widget"))
    {
        return ABT::Blueprint::Ops::ConfigureMessagePlateWidget(Blueprint, Op, OutMessages, OutError);
    }

    if (OpName == TEXT("set_static_mesh"))
    {
        return ABT::Blueprint::Ops::SetStaticMesh(Blueprint, Op, OutMessages, OutError);
    }

    if (OpName == TEXT("set_component_material"))
    {
        return ABT::Blueprint::Ops::SetComponentMaterial(Blueprint, Op, OutMessages, OutError);
    }

    if (OpName == TEXT("add_node"))
    {
        return ABT::Blueprint::Ops::AddNode(Blueprint, Op, NodeMap, OutMessages, OutError);
    }

    if (OpName == TEXT("connect_exec") || OpName == TEXT("connect_data"))
    {
        return ABT::Blueprint::Ops::ConnectPins(Blueprint, Op, NodeMap, OutMessages, OutError);
    }

    if (OpName == TEXT("set_pin_default"))
    {
        return ABT::Blueprint::Ops::SetPinDefault(Blueprint, Op, NodeMap, OutMessages, OutError);
    }

    if (OpName == TEXT("remove_node"))
    {
        return ABT::Blueprint::Ops::RemoveNode(Blueprint, Op, NodeMap, OutMessages, OutError);
    }

    if (OpName == TEXT("layout_blueprint_graph"))
    {
        return ABT::Blueprint::Ops::LayoutGraphNodes(Blueprint, Op, NodeMap, OutMessages, OutError);
    }

    if (OpName == TEXT("configure_timeline"))
    {
        return ABT::Blueprint::Ops::ConfigureTimeline(Blueprint, Op, NodeMap, OutMessages, OutError);
    }

    if (OpName == TEXT("set_blueprint_property"))
    {
        return ABT::Blueprint::Ops::SetBlueprintProperty(Blueprint, Op, OutMessages, OutError);
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
        TArray<TSharedPtr<FJsonValue>> JsonMessages;
        for (const FString& Message : ValidationMessages)
        {
            JsonMessages.Add(ABTJson::StringValue(Message));
        }
        OutJson->SetArrayField(TEXT("messages"), JsonMessages);
        return true;
    }

    UBlueprint* Blueprint = LoadBlueprint(ABTJson::GetString(Patch, TEXT("target")), OutError);
    if (!Blueprint) return false;

    const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
    Patch->TryGetArrayField(TEXT("operations"), Ops);

    FScopedTransaction Transaction(NSLOCTEXT("AgentBlueprintTools", "ApplyBlueprintPatch", "Apply Blueprint Patch"));
    Blueprint->Modify();

    TArray<FString> Messages;
    TMap<FString, UEdGraphNode*> NodeMap;

    for (const TSharedPtr<FJsonValue>& Value : *Ops)
    {
        TSharedPtr<FJsonObject> Op = Value->AsObject();
        if (!Op.IsValid())
        {
            OutError = TEXT("operation is not an object");
            Transaction.Cancel();
            return false;
        }
        if (!ApplyOperation(Blueprint, Op, NodeMap, Messages, OutError))
        {
            Transaction.Cancel();
            return false;
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> CompileJson;
    if (!CompileBlueprint(Blueprint, CompileJson, OutError))
    {
        Transaction.Cancel();
        return false;
    }

    OutJson = ABTJson::Ok();
    OutJson->SetObjectField(TEXT("compile"), CompileJson);
    OutJson->SetBoolField(TEXT("saved"), false);

    TArray<TSharedPtr<FJsonValue>> JsonMessages;
    for (const FString& Message : Messages) JsonMessages.Add(ABTJson::StringValue(Message));
    OutJson->SetArrayField(TEXT("messages"), JsonMessages);

    if (!CompileJson->GetBoolField(TEXT("compile_ok")))
    {
        Transaction.Cancel();
        OutJson->SetBoolField(TEXT("rolledBack"), true);
        return true;
    }

    if (bSaveOnSuccess)
    {
        UPackage* Package = Blueprint->GetOutermost();
        if (Package) Package->SetDirtyFlag(true);
        OutJson->SetBoolField(TEXT("saved"), UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false));
    }

    return true;
}

bool FABTBlueprintTools::CompileBlueprintAsset(const FString& AssetPath, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    UBlueprint* Blueprint = LoadBlueprint(AssetPath, OutError);
    if (!Blueprint) return false;
    return CompileBlueprint(Blueprint, OutJson, OutError);
}
