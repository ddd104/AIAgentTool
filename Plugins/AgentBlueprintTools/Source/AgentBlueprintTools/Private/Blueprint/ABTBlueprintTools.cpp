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
#include "Components/ActorComponent.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/Widget.h"
#include "EditorAssetLibrary.h"
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
#include "Kismet/KismetSystemLibrary.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopedSlowTask.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "ScopedTransaction.h"
#include "Sections/MovieSceneEventTriggerSection.h"
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
        else if (OpName == TEXT("ensure_timer_loop"))
        {
            RequireField(Op, OpName, TEXT("function"), OutMessages);
        }
        else if (OpName == TEXT("ensure_looping_move"))
        {
            // function is optional here; the op defaults to MoveByDelta.
        }
        else if (OpName == TEXT("configure_button_pages_widget"))
        {
            // pages is optional; the tool supplies useful defaults for quick UMG prototypes.
        }
        else
        {
            OutMessages.Add(FString::Printf(TEXT("Unsupported op: %s."), *OpName));
        }
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

    if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint))
    {
        TArray<TSharedPtr<FJsonValue>> Widgets;
        if (WidgetBlueprint->WidgetTree)
        {
            TArray<UWidget*> AllWidgets;
            WidgetBlueprint->WidgetTree->GetAllWidgets(AllWidgets);
            for (UWidget* Widget : AllWidgets)
            {
                if (!Widget) continue;
                TSharedPtr<FJsonObject> W = ABTJson::Object();
                W->SetStringField(TEXT("name"), Widget->GetName());
                W->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName());
                W->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
                W->SetStringField(TEXT("visibility"), StaticEnum<ESlateVisibility>()->GetNameStringByValue(static_cast<int64>(Widget->GetVisibility())));
                W->SetNumberField(TEXT("render_opacity"), Widget->GetRenderOpacity());
                const FWidgetTransform Transform = Widget->GetRenderTransform();
                TArray<TSharedPtr<FJsonValue>> Scale;
                Scale.Add(MakeShared<FJsonValueNumber>(Transform.Scale.X));
                Scale.Add(MakeShared<FJsonValueNumber>(Transform.Scale.Y));
                W->SetArrayField(TEXT("render_scale"), Scale);
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
    }
    Summary->SetArrayField(TEXT("entry_points"), EntryPoints);
    Summary->SetArrayField(TEXT("variable_reads"), Reads);
    Summary->SetArrayField(TEXT("variable_writes"), Writes);
    Summary->SetArrayField(TEXT("external_calls"), Calls);
    Summary->SetNumberField(TEXT("graph_count"), GraphsRaw.Num());
    OutJson->SetObjectField(TEXT("semantic_summary"), Summary);
    OutJson->SetArrayField(TEXT("graph_summaries"), GraphSummaries);

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

    if (OpName == TEXT("ensure_component"))
    {
        return ABT::Blueprint::Ops::EnsureComponent(Blueprint, Op, OutMessages, OutError);
    }

    if (OpName == TEXT("configure_button_pages_widget"))
    {
        return ABT::Blueprint::Ops::ConfigureButtonPagesWidget(Blueprint, Op, OutMessages, OutError);
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
        return ABT::Blueprint::Ops::ConnectPins(Op, NodeMap, OutMessages, OutError);
    }

    if (OpName == TEXT("set_pin_default"))
    {
        return ABT::Blueprint::Ops::SetPinDefault(Op, NodeMap, OutError);
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
