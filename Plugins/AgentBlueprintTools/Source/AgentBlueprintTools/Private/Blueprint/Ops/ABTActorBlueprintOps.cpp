#include "Blueprint/Ops/ABTActorBlueprintOps.h"
#include "Blueprint/Utils/ABTBlueprintGraphUtils.h"
#include "Utils/ABTJson.h"

#include "Components/ActorComponent.h"
#include "Components/BoxComponent.h"
#include "Components/MeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Components/WidgetComponent.h"
#include "Curves/CurveFloat.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/TimelineTemplate.h"
#include "GameFramework/Actor.h"
#include "InputCoreTypes.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_InputKey.h"
#include "K2Node_Timeline.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Materials/MaterialInterface.h"

namespace
{
    const FName NAME_ReceiveBeginPlay(TEXT("ReceiveBeginPlay"));

    UK2Node_Event* FindBeginPlayEvent(UEdGraph* Graph)
    {
        if (!Graph) return nullptr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
            if (Event && Event->bOverrideFunction &&
                Event->EventReference.GetMemberName() == NAME_ReceiveBeginPlay)
            {
                return Event;
            }
        }
        return nullptr;
    }

    UK2Node_Event* EnsureBeginPlayEvent(UBlueprint* Blueprint, UEdGraph* Graph, TArray<FString>& OutMessages)
    {
        if (UK2Node_Event* Existing = FindBeginPlayEvent(Graph))
        {
            return Existing;
        }

        UK2Node_Event* Event = NewObject<UK2Node_Event>(Graph);
        Event->EventReference.SetExternalMember(NAME_ReceiveBeginPlay, AActor::StaticClass());
        Event->bOverrideFunction = true;
        Graph->AddNode(Event, true, false);
        Event->CreateNewGuid();
        Event->NodePosX = -420;
        Event->NodePosY = 0;
        Event->AllocateDefaultPins();
        OutMessages.Add(TEXT("Added ReceiveBeginPlay event"));
        return Event;
    }

    UEdGraphPin* FindFunctionParamPin(UK2Node_CallFunction* Node, UFunction* Function, const FName& ParamName)
    {
        if (!Node || !Function || !Function->FindPropertyByName(ParamName))
        {
            return nullptr;
        }
        return ABT::Blueprint::FindPinByName(Node, ParamName.ToString());
    }

    bool HasTimerCallForFunction(UEdGraph* Graph, const FString& FunctionName)
    {
        if (!Graph) return false;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
            if (!Call || Call->FunctionReference.GetMemberName() != GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, K2_SetTimer))
            {
                continue;
            }
            UEdGraphPin* FunctionNamePin = ABT::Blueprint::FindPinByName(Call, TEXT("FunctionName"));
            if (FunctionNamePin && FunctionNamePin->DefaultValue.Equals(FunctionName, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    bool EnsureBoolVariable(UBlueprint* Blueprint, const FString& Name, TArray<FString>& OutMessages)
    {
        for (const FBPVariableDescription& Var : Blueprint->NewVariables)
        {
            if (Var.VarName.ToString().Equals(Name, ESearchCase::IgnoreCase))
            {
                return false;
            }
        }

        FBlueprintEditorUtils::AddMemberVariable(Blueprint, *Name, ABT::Blueprint::MakePinTypeFromString(TEXT("bool")));
        OutMessages.Add(FString::Printf(TEXT("Added variable %s"), *Name));
        return true;
    }

    UStaticMesh* LoadStaticMeshAsset(const FString& MeshPath)
    {
        if (MeshPath.IsEmpty())
        {
            return nullptr;
        }

        if (UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath))
        {
            return Mesh;
        }

        FString PackagePath;
        FString AssetName;
        int32 SlashIndex = INDEX_NONE;
        if (MeshPath.FindLastChar(TEXT('/'), SlashIndex) && SlashIndex != INDEX_NONE)
        {
            PackagePath = MeshPath;
            AssetName = MeshPath.Mid(SlashIndex + 1);
            return LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName));
        }
        return nullptr;
    }

    UClass* LoadWidgetClassAsset(const FString& WidgetClassPath)
    {
        if (WidgetClassPath.IsEmpty())
        {
            return nullptr;
        }

        if (UClass* DirectClass = LoadObject<UClass>(nullptr, *WidgetClassPath))
        {
            return DirectClass;
        }

        if (UBlueprint* WidgetBlueprint = LoadObject<UBlueprint>(nullptr, *WidgetClassPath))
        {
            return WidgetBlueprint->GeneratedClass;
        }

        FString AssetName;
        int32 SlashIndex = INDEX_NONE;
        if (WidgetClassPath.FindLastChar(TEXT('/'), SlashIndex) && SlashIndex != INDEX_NONE)
        {
            AssetName = WidgetClassPath.Mid(SlashIndex + 1);
            if (UClass* GeneratedClass = LoadObject<UClass>(nullptr, *FString::Printf(TEXT("%s.%s_C"), *WidgetClassPath, *AssetName)))
            {
                return GeneratedClass;
            }
            if (UBlueprint* WidgetBlueprint = LoadObject<UBlueprint>(nullptr, *FString::Printf(TEXT("%s.%s"), *WidgetClassPath, *AssetName)))
            {
                return WidgetBlueprint->GeneratedClass;
            }
        }

        return nullptr;
    }

    USCS_Node* EnsureSceneComponentNode(
        UBlueprint* Blueprint,
        const FString& Name,
        UClass* ComponentClass,
        const FString& AttachTo,
        TArray<FString>& OutMessages,
        FString& OutError)
    {
        if (!Blueprint || !Blueprint->SimpleConstructionScript || !ComponentClass)
        {
            OutError = TEXT("Blueprint SCS or component class is not available");
            return nullptr;
        }

        if (USCS_Node* Existing = ABT::Blueprint::FindSCSNodeByName(Blueprint, Name))
        {
            return Existing;
        }

        USCS_Node* Node = Blueprint->SimpleConstructionScript->CreateNode(ComponentClass, *Name);
        if (!Node)
        {
            OutError = FString::Printf(TEXT("Failed to create component node: %s"), *Name);
            return nullptr;
        }

        USCS_Node* Parent = nullptr;
        if (!AttachTo.IsEmpty())
        {
            Parent = ABT::Blueprint::FindSCSNodeByName(Blueprint, AttachTo);
            if (!Parent)
            {
                OutError = FString::Printf(TEXT("Parent component not found: %s"), *AttachTo);
                return nullptr;
            }
        }

        if (Parent)
        {
            Parent->AddChildNode(Node);
        }
        else
        {
            Blueprint->SimpleConstructionScript->AddNode(Node);
        }

        OutMessages.Add(FString::Printf(TEXT("Added component %s (%s)"), *Name, *ComponentClass->GetName()));
        return Node;
    }

    USCS_Node* EnsureDefaultSceneRoot(UBlueprint* Blueprint, TArray<FString>& OutMessages, FString& OutError)
    {
        if (USCS_Node* Existing = ABT::Blueprint::FindSCSNodeByName(Blueprint, TEXT("DefaultSceneRoot")))
        {
            return Existing;
        }

        return EnsureSceneComponentNode(
            Blueprint,
            TEXT("DefaultSceneRoot"),
            USceneComponent::StaticClass(),
            FString(),
            OutMessages,
            OutError);
    }

    void ApplySceneTransform(USCS_Node* Node, const FVector& Location, const FRotator& Rotation, const FVector& Scale)
    {
        if (USceneComponent* SceneTemplate = Node ? Cast<USceneComponent>(Node->ComponentTemplate) : nullptr)
        {
            SceneTemplate->Modify();
            SceneTemplate->SetRelativeLocation(Location);
            SceneTemplate->SetRelativeRotation(Rotation);
            SceneTemplate->SetRelativeScale3D(Scale);
        }
    }

    bool ConnectPins(UEdGraphPin* From, UEdGraphPin* To, FString& OutError)
    {
        if (!From || !To)
        {
            OutError = TEXT("Missing pin while creating interactable actor flow");
            return false;
        }

        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        if (!Schema->TryCreateConnection(From, To))
        {
            OutError = FString::Printf(TEXT("Could not connect %s to %s"), *From->PinName.ToString(), *To->PinName.ToString());
            return false;
        }
        return true;
    }

    void SetPinDefaultValue(UEdGraphPin* Pin, const FString& Value)
    {
        if (Pin)
        {
            const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
            Schema->TrySetDefaultValue(*Pin, Value);
        }
    }

    template <typename NodeType>
    NodeType* AddGraphNode(UEdGraph* Graph, int32 X, int32 Y)
    {
        NodeType* Node = NewObject<NodeType>(Graph);
        Graph->AddNode(Node, true, false);
        Node->CreateNewGuid();
        Node->NodePosX = X;
        Node->NodePosY = Y;
        return Node;
    }

    UK2Node_CallFunction* AddCallNode(UEdGraph* Graph, UFunction* Function, int32 X, int32 Y, FString& OutError)
    {
        if (!Function)
        {
            OutError = TEXT("Function was not found while creating interactable actor flow");
            return nullptr;
        }

        UK2Node_CallFunction* Node = AddGraphNode<UK2Node_CallFunction>(Graph, X, Y);
        Node->SetFromFunction(Function);
        Node->AllocateDefaultPins();
        return Node;
    }

    UK2Node_VariableGet* AddVariableGetNode(UEdGraph* Graph, const FString& VariableName, int32 X, int32 Y)
    {
        UK2Node_VariableGet* Node = AddGraphNode<UK2Node_VariableGet>(Graph, X, Y);
        Node->VariableReference.SetSelfMember(*VariableName);
        Node->AllocateDefaultPins();
        return Node;
    }

    UK2Node_VariableSet* AddVariableSetNode(UEdGraph* Graph, const FString& VariableName, bool bValue, int32 X, int32 Y)
    {
        UK2Node_VariableSet* Node = AddGraphNode<UK2Node_VariableSet>(Graph, X, Y);
        Node->VariableReference.SetSelfMember(*VariableName);
        Node->AllocateDefaultPins();
        SetPinDefaultValue(ABT::Blueprint::FindPinByName(Node, VariableName), bValue ? TEXT("true") : TEXT("false"));
        return Node;
    }

    UK2Node_Event* FindActorEvent(UEdGraph* Graph, const FName& EventName)
    {
        if (!Graph)
        {
            return nullptr;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
            if (Event && Event->bOverrideFunction && Event->EventReference.GetMemberName() == EventName)
            {
                return Event;
            }
        }
        return nullptr;
    }

    UK2Node_Event* EnsureActorEvent(UEdGraph* Graph, const FName& EventName, int32 X, int32 Y, TArray<FString>& OutMessages)
    {
        if (UK2Node_Event* Existing = FindActorEvent(Graph, EventName))
        {
            return Existing;
        }

        UK2Node_Event* Event = AddGraphNode<UK2Node_Event>(Graph, X, Y);
        Event->EventReference.SetExternalMember(EventName, AActor::StaticClass());
        Event->bOverrideFunction = true;
        Event->AllocateDefaultPins();
        OutMessages.Add(FString::Printf(TEXT("Added actor event %s"), *EventName.ToString()));
        return Event;
    }

    bool HasInputKeyNode(UEdGraph* Graph, const FKey& Key)
    {
        if (!Graph)
        {
            return false;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            const UK2Node_InputKey* InputKey = Cast<UK2Node_InputKey>(Node);
            if (InputKey && InputKey->InputKey == Key)
            {
                return true;
            }
        }
        return false;
    }

    UK2Node_InputKey* FindInputKeyNode(UEdGraph* Graph, const FKey& Key)
    {
        if (!Graph)
        {
            return nullptr;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_InputKey* InputKey = Cast<UK2Node_InputKey>(Node);
            if (InputKey && InputKey->InputKey == Key)
            {
                return InputKey;
            }
        }
        return nullptr;
    }

    UK2Node_CallFunction* FindCallNodeByFunction(UEdGraph* Graph, const FName& FunctionName, int32 MatchIndex = 0)
    {
        if (!Graph || MatchIndex < 0)
        {
            return nullptr;
        }

        int32 Seen = 0;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
            if (Call && Call->FunctionReference.GetMemberName() == FunctionName)
            {
                if (Seen == MatchIndex)
                {
                    return Call;
                }
                ++Seen;
            }
        }
        return nullptr;
    }

    UK2Node_VariableGet* FindVariableGetNode(UEdGraph* Graph, const FString& VariableName, int32 MatchIndex = 0)
    {
        if (!Graph || MatchIndex < 0)
        {
            return nullptr;
        }

        int32 Seen = 0;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node);
            if (Get && Get->VariableReference.GetMemberName().ToString().Equals(VariableName, ESearchCase::IgnoreCase))
            {
                if (Seen == MatchIndex)
                {
                    return Get;
                }
                ++Seen;
            }
        }
        return nullptr;
    }

    UK2Node_VariableSet* FindVariableSetNodeByDefault(UEdGraph* Graph, const FString& VariableName, bool bDefaultValue)
    {
        if (!Graph)
        {
            return nullptr;
        }

        const FString DesiredDefault = bDefaultValue ? TEXT("true") : TEXT("false");
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_VariableSet* Set = Cast<UK2Node_VariableSet>(Node);
            if (!Set || !Set->VariableReference.GetMemberName().ToString().Equals(VariableName, ESearchCase::IgnoreCase))
            {
                continue;
            }

            UEdGraphPin* ValuePin = ABT::Blueprint::FindPinByName(Set, VariableName);
            if (ValuePin && ValuePin->DefaultValue.Equals(DesiredDefault, ESearchCase::IgnoreCase))
            {
                return Set;
            }
        }
        return nullptr;
    }

    void MoveNode(UEdGraphNode* Node, int32 X, int32 Y, TArray<UEdGraphNode*>& OutMovedNodes)
    {
        if (!Node)
        {
            return;
        }

        Node->Modify();
        Node->NodePosX = X;
        Node->NodePosY = Y;
        OutMovedNodes.AddUnique(Node);
    }

    void ResolveInteractableLayoutOverlaps(TArray<UEdGraphNode*>& Nodes)
    {
        Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
        {
            return A.NodePosY == B.NodePosY ? A.NodePosX < B.NodePosX : A.NodePosY < B.NodePosY;
        });

        constexpr int32 MinXSpacing = 220;
        constexpr int32 MinYSpacing = 120;
        for (int32 Index = 0; Index < Nodes.Num(); ++Index)
        {
            UEdGraphNode* Node = Nodes[Index];
            if (!Node)
            {
                continue;
            }

            bool bMoved = true;
            while (bMoved)
            {
                bMoved = false;
                for (int32 PreviousIndex = 0; PreviousIndex < Index; ++PreviousIndex)
                {
                    UEdGraphNode* Previous = Nodes[PreviousIndex];
                    if (!Previous)
                    {
                        continue;
                    }

                    if (FMath::Abs(Node->NodePosX - Previous->NodePosX) < MinXSpacing &&
                        FMath::Abs(Node->NodePosY - Previous->NodePosY) < MinYSpacing)
                    {
                        Node->NodePosY = Previous->NodePosY + MinYSpacing;
                        bMoved = true;
                    }
                }
            }
        }
    }

    void LayoutInteractableActorGraph(
        UEdGraph* Graph,
        const FKey& InteractionKey,
        const FName& TimelineName,
        const FString& RotationComponentName,
        const FString& PromptComponentName,
        const FString& CanInteractVar,
        const FString& IsActiveVar,
        TArray<FString>& OutMessages)
    {
        if (!Graph)
        {
            return;
        }

        TArray<UEdGraphNode*> MovedNodes;
        MoveNode(FindActorEvent(Graph, TEXT("ReceiveBeginPlay")), -1180, -620, MovedNodes);

        MoveNode(FindActorEvent(Graph, TEXT("ReceiveActorBeginOverlap")), -1180, -360, MovedNodes);
        MoveNode(FindVariableSetNodeByDefault(Graph, CanInteractVar, true), -840, -360, MovedNodes);
        MoveNode(FindVariableGetNode(Graph, PromptComponentName, 0), -560, -150, MovedNodes);
        MoveNode(FindCallNodeByFunction(Graph, TEXT("SetVisibility"), 0), -520, -360, MovedNodes);
        MoveNode(FindCallNodeByFunction(Graph, TEXT("GetPlayerController"), 0), -520, -70, MovedNodes);
        MoveNode(FindCallNodeByFunction(Graph, TEXT("EnableInput"), 0), -180, -360, MovedNodes);

        MoveNode(FindActorEvent(Graph, TEXT("ReceiveActorEndOverlap")), -1180, 40, MovedNodes);
        MoveNode(FindVariableSetNodeByDefault(Graph, CanInteractVar, false), -840, 40, MovedNodes);
        MoveNode(FindVariableGetNode(Graph, PromptComponentName, 1), -560, 250, MovedNodes);
        MoveNode(FindCallNodeByFunction(Graph, TEXT("SetVisibility"), 1), -520, 40, MovedNodes);
        MoveNode(FindCallNodeByFunction(Graph, TEXT("GetPlayerController"), 1), -520, 330, MovedNodes);
        MoveNode(FindCallNodeByFunction(Graph, TEXT("DisableInput"), 0), -180, 40, MovedNodes);

        MoveNode(FindInputKeyNode(Graph, InteractionKey), -1180, 560, MovedNodes);
        MoveNode(FindVariableGetNode(Graph, CanInteractVar, 0), -1180, 760, MovedNodes);
        MoveNode(FindCallNodeByFunction(Graph, TEXT("K2_SetRelativeRotation"), 0), 1180, 560, MovedNodes);
        MoveNode(FindVariableGetNode(Graph, RotationComponentName, 0), 880, 420, MovedNodes);

        TArray<UK2Node_IfThenElse*> Branches;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_IfThenElse* Branch = Cast<UK2Node_IfThenElse>(Node))
            {
                Branches.Add(Branch);
            }
        }
        Branches.Sort([](const UK2Node_IfThenElse& A, const UK2Node_IfThenElse& B)
        {
            return A.NodePosX == B.NodePosX ? A.NodePosY < B.NodePosY : A.NodePosX < B.NodePosX;
        });
        if (Branches.Num() > 0)
        {
            MoveNode(Branches[0], -840, 560, MovedNodes);
        }
        if (Branches.Num() > 1)
        {
            MoveNode(Branches[1], -520, 560, MovedNodes);
        }

        MoveNode(FindVariableGetNode(Graph, IsActiveVar, 0), -840, 900, MovedNodes);
        MoveNode(FindVariableSetNodeByDefault(Graph, IsActiveVar, false), -180, 460, MovedNodes);
        MoveNode(FindVariableSetNodeByDefault(Graph, IsActiveVar, true), -180, 700, MovedNodes);

        UBlueprint* OwningBlueprint = Graph->GetTypedOuter<UBlueprint>();
        if (OwningBlueprint)
        {
            if (UTimelineTemplate* Timeline = OwningBlueprint->FindTimelineTemplateByVariableName(TimelineName))
            {
                MoveNode(FBlueprintEditorUtils::FindNodeForTimeline(OwningBlueprint, Timeline), 180, 560, MovedNodes);
            }
        }
        MoveNode(FindCallNodeByFunction(Graph, TEXT("Lerp"), 0), 520, 800, MovedNodes);
        MoveNode(FindCallNodeByFunction(Graph, TEXT("MakeRotator"), 0), 820, 760, MovedNodes);

        ResolveInteractableLayoutOverlaps(MovedNodes);
        if (MovedNodes.Num() > 0)
        {
            OutMessages.Add(FString::Printf(TEXT("Laid out %d interactable graph node(s)"), MovedNodes.Num()));
        }
    }

    UK2Node_Timeline* EnsureTimelineNode(
        UBlueprint* Blueprint,
        UEdGraph* Graph,
        const FName& TimelineName,
        const FName& TrackName,
        float Duration,
        float EndValue,
        int32 X,
        int32 Y,
        TArray<FString>& OutMessages,
        FString& OutError)
    {
        UTimelineTemplate* Timeline = Blueprint->FindTimelineTemplateByVariableName(TimelineName);
        if (!Timeline)
        {
            Timeline = FBlueprintEditorUtils::AddNewTimeline(Blueprint, TimelineName);
            if (!Timeline)
            {
                OutError = FString::Printf(TEXT("Failed to create timeline: %s"), *TimelineName.ToString());
                return nullptr;
            }
            OutMessages.Add(FString::Printf(TEXT("Added timeline %s"), *TimelineName.ToString()));
        }

        Timeline->Modify();
        Timeline->TimelineLength = Duration;
        Timeline->LengthMode = TL_TimelineLength;
        Timeline->bAutoPlay = false;
        Timeline->bLoop = false;

        int32 TrackIndex = INDEX_NONE;
        for (int32 Index = 0; Index < Timeline->FloatTracks.Num(); ++Index)
        {
            if (Timeline->FloatTracks[Index].GetTrackName() == TrackName)
            {
                TrackIndex = Index;
                break;
            }
        }
        if (TrackIndex == INDEX_NONE)
        {
            TrackIndex = Timeline->FloatTracks.Num();
            FTTFloatTrack NewTrack;
            NewTrack.SetTrackName(TrackName, Timeline);
            NewTrack.CurveFloat = NewObject<UCurveFloat>(Blueprint->GeneratedClass, NAME_None, RF_Public | RF_Transactional);
            Timeline->FloatTracks.Add(NewTrack);
            Timeline->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_FloatInterp, TrackIndex));
            OutMessages.Add(FString::Printf(TEXT("Added timeline float track %s"), *TrackName.ToString()));
        }

        FTTFloatTrack& Track = Timeline->FloatTracks[TrackIndex];
        if (!Track.CurveFloat)
        {
            Track.CurveFloat = NewObject<UCurveFloat>(Blueprint->GeneratedClass, NAME_None, RF_Public | RF_Transactional);
        }

        Track.CurveFloat->Modify();
        Track.CurveFloat->FloatCurve.Reset();
        FKeyHandle StartKey = Track.CurveFloat->FloatCurve.AddKey(0.0f, 0.0f);
        FKeyHandle EndKey = Track.CurveFloat->FloatCurve.AddKey(Duration, EndValue);
        Track.CurveFloat->FloatCurve.SetKeyInterpMode(StartKey, RCIM_Cubic);
        Track.CurveFloat->FloatCurve.SetKeyInterpMode(EndKey, RCIM_Cubic);

        if (UK2Node_Timeline* ExistingNode = FBlueprintEditorUtils::FindNodeForTimeline(Blueprint, Timeline))
        {
            ExistingNode->ReconstructNode();
            return ExistingNode;
        }

        UK2Node_Timeline* Node = AddGraphNode<UK2Node_Timeline>(Graph, X, Y);
        Node->TimelineName = TimelineName;
        Node->TimelineGuid = Timeline->TimelineGuid;
        Node->AllocateDefaultPins();
        return Node;
    }

    bool AddPromptVisibilityCall(
        UEdGraph* Graph,
        const FString& PromptComponentName,
        bool bVisible,
        int32 X,
        int32 Y,
        UK2Node_CallFunction*& OutCall,
        FString& OutError)
    {
        OutCall = nullptr;
        if (PromptComponentName.IsEmpty())
        {
            return true;
        }

        UFunction* SetVisibilityFunction = USceneComponent::StaticClass()->FindFunctionByName(TEXT("SetVisibility"));
        UK2Node_CallFunction* Call = AddCallNode(Graph, SetVisibilityFunction, X, Y, OutError);
        if (!Call)
        {
            return false;
        }

        UK2Node_VariableGet* PromptGet = AddVariableGetNode(Graph, PromptComponentName, X - 220, Y + 90);
        if (!ConnectPins(
            ABT::Blueprint::FindPinByName(PromptGet, PromptComponentName),
            ABT::Blueprint::FindPinByName(Call, TEXT("self")),
            OutError))
        {
            if (!ConnectPins(
                ABT::Blueprint::FindPinByName(PromptGet, PromptComponentName),
                ABT::Blueprint::FindPinByName(Call, TEXT("Target")),
                OutError))
            {
                return false;
            }
        }

        SetPinDefaultValue(ABT::Blueprint::FindPinByName(Call, TEXT("bNewVisibility")), bVisible ? TEXT("true") : TEXT("false"));
        SetPinDefaultValue(ABT::Blueprint::FindPinByName(Call, TEXT("bPropagateToChildren")), TEXT("true"));
        OutCall = Call;
        return true;
    }
}

namespace ABT::Blueprint::Ops
{
    bool EnsureMoveFunction(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError)
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

    bool EnsureTimerLoop(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError)
    {
        UEdGraph* Graph = Blueprint && Blueprint->UbergraphPages.Num() ? Blueprint->UbergraphPages[0].Get() : nullptr;
        if (!Graph)
        {
            OutError = TEXT("Blueprint has no EventGraph");
            return false;
        }

        const FString FunctionName = ABTJson::GetString(Op, TEXT("function"), TEXT("MoveByDelta"));
        if (HasTimerCallForFunction(Graph, FunctionName))
        {
            OutMessages.Add(FString::Printf(TEXT("Timer loop already exists for %s"), *FunctionName));
            return true;
        }

        UFunction* TimerFunction = UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, K2_SetTimer));
        if (!TimerFunction)
        {
            OutError = TEXT("UKismetSystemLibrary::K2_SetTimer not found");
            return false;
        }

        UK2Node_Event* BeginPlay = EnsureBeginPlayEvent(Blueprint, Graph, OutMessages);
        UK2Node_CallFunction* TimerNode = NewObject<UK2Node_CallFunction>(Graph);
        TimerNode->SetFromFunction(TimerFunction);
        Graph->AddNode(TimerNode, true, false);
        TimerNode->CreateNewGuid();
        TimerNode->NodePosX = 60;
        TimerNode->NodePosY = 0;
        TimerNode->AllocateDefaultPins();

        UEdGraphPin* FunctionNamePin = FindFunctionParamPin(TimerNode, TimerFunction, TEXT("FunctionName"));
        UEdGraphPin* TimePin = FindFunctionParamPin(TimerNode, TimerFunction, TEXT("Time"));
        UEdGraphPin* LoopingPin = FindFunctionParamPin(TimerNode, TimerFunction, TEXT("bLooping"));
        if (!FunctionNamePin || !TimePin || !LoopingPin)
        {
            OutError = TEXT("K2_SetTimer pins did not match reflected function parameters");
            return false;
        }

        FunctionNamePin->DefaultValue = FunctionName;
        TimePin->DefaultValue = FString::SanitizeFloat(ABTJson::GetNumber(Op, TEXT("interval"), 0.25));
        LoopingPin->DefaultValue = ABTJson::GetBool(Op, TEXT("looping"), true) ? TEXT("true") : TEXT("false");

        if (UEdGraphPin* MaxOncePin = FindFunctionParamPin(TimerNode, TimerFunction, TEXT("bMaxOncePerFrame")))
        {
            MaxOncePin->DefaultValue = ABTJson::GetBool(Op, TEXT("maxOncePerFrame"), true) ? TEXT("true") : TEXT("false");
        }

        UEdGraphPin* BeginThen = FindFirstPin(BeginPlay, EGPD_Output, UEdGraphSchema_K2::PC_Exec);
        UEdGraphPin* TimerExec = FindFirstPin(TimerNode, EGPD_Input, UEdGraphSchema_K2::PC_Exec);
        if (!BeginThen || !TimerExec)
        {
            OutError = TEXT("Could not find exec pins for timer loop");
            return false;
        }

        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        if (!Schema->TryCreateConnection(BeginThen, TimerExec))
        {
            OutError = TEXT("Could not connect BeginPlay to timer");
            return false;
        }

        OutMessages.Add(FString::Printf(TEXT("Ensured timer loop for %s"), *FunctionName));
        return true;
    }

    bool EnsureLoopingMove(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError)
    {
        const FString FunctionName = ABTJson::GetString(Op, TEXT("function"), TEXT("MoveByDelta"));
        TSharedPtr<FJsonObject> MoveOp = ABTJson::Object();
        MoveOp->SetStringField(TEXT("op"), TEXT("ensure_move_function"));
        MoveOp->SetStringField(TEXT("name"), FunctionName);

        const TArray<TSharedPtr<FJsonValue>>* Delta = nullptr;
        if (Op->TryGetArrayField(TEXT("delta"), Delta) && Delta)
        {
            MoveOp->SetArrayField(TEXT("delta"), *Delta);
        }

        if (!EnsureMoveFunction(Blueprint, MoveOp, OutMessages, OutError))
        {
            return false;
        }

        TSharedPtr<FJsonObject> TimerOp = ABTJson::Object();
        TimerOp->SetStringField(TEXT("op"), TEXT("ensure_timer_loop"));
        TimerOp->SetStringField(TEXT("function"), FunctionName);
        TimerOp->SetNumberField(TEXT("interval"), ABTJson::GetNumber(Op, TEXT("interval"), 0.25));
        TimerOp->SetBoolField(TEXT("looping"), ABTJson::GetBool(Op, TEXT("looping"), true));
        TimerOp->SetBoolField(TEXT("maxOncePerFrame"), ABTJson::GetBool(Op, TEXT("maxOncePerFrame"), true));
        return EnsureTimerLoop(Blueprint, TimerOp, OutMessages, OutError);
    }

    bool EnsureComponent(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError)
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

    bool SetStaticMesh(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError)
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

    bool SetComponentMaterial(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError)
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

    bool ConfigureInteractableActor(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError)
    {
        if (!Blueprint || !Blueprint->SimpleConstructionScript)
        {
            OutError = TEXT("configure_interactable_actor requires an Actor Blueprint with a SimpleConstructionScript");
            return false;
        }

        const FString RootName = ABTJson::GetString(Op, TEXT("rootComponent"), TEXT("DefaultSceneRoot"));
        const FString MeshName = ABTJson::GetString(Op, TEXT("meshComponent"), TEXT("InteractableMesh"));
        const FString RotationComponentName = ABTJson::GetString(Op, TEXT("rotationComponent"), MeshName);
        const FString CollisionName = ABTJson::GetString(Op, TEXT("collisionComponent"), TEXT("InteractionCollision"));
        const FString PromptName = ABTJson::GetString(Op, TEXT("promptComponent"), TEXT("InteractionPrompt"));
        const FString CanInteractVar = ABTJson::GetString(Op, TEXT("canInteractVariable"), TEXT("bCanInteract"));
        const FString IsActiveVar = ABTJson::GetString(Op, TEXT("activeVariable"), TEXT("bIsActive"));
        const FString TimelineNameString = ABTJson::GetString(Op, TEXT("timeline"), TEXT("InteractionTimeline"));
        const FString TimelineTrackString = ABTJson::GetString(Op, TEXT("track"), TEXT("Alpha"));
        const FString MeshPath = ABTJson::GetString(Op, TEXT("mesh"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        const FString PromptText = ABTJson::GetString(Op, TEXT("promptText"), TEXT("Press E"));
        const FString PromptWidgetClassPath = ABTJson::GetString(Op, TEXT("promptWidgetClass"));
        const FString KeyName = ABTJson::GetString(Op, TEXT("key"), TEXT("E"));

        const float OpenAngle = static_cast<float>(ABTJson::GetNumber(Op, TEXT("rotationAngle"), ABTJson::GetNumber(Op, TEXT("openAngle"), 90.0)));
        const float Duration = FMath::Max(0.01f, static_cast<float>(ABTJson::GetNumber(Op, TEXT("duration"), 0.8)));
        const FVector MeshScale = ABT::Blueprint::ReadVectorFromOp(Op, TEXT("meshScale"), FVector(0.12, 0.9, 2.1));
        const FVector MeshLocation = ABT::Blueprint::ReadVectorFromOp(Op, TEXT("meshLocation"), FVector(0.0, 45.0, 105.0));
        const FVector CollisionExtent = ABT::Blueprint::ReadVectorFromOp(Op, TEXT("collisionExtent"), FVector(120.0, 140.0, 130.0));
        const FVector CollisionLocation = ABT::Blueprint::ReadVectorFromOp(Op, TEXT("collisionLocation"), FVector(40.0, 45.0, 105.0));
        const FVector PromptLocation = ABT::Blueprint::ReadVectorFromOp(Op, TEXT("promptLocation"), FVector(0.0, 45.0, 240.0));
        const FVector PromptScale = ABT::Blueprint::ReadVectorFromOp(Op, TEXT("promptScale"), FVector(0.35, 0.35, 0.35));

        USCS_Node* RootNode = EnsureDefaultSceneRoot(Blueprint, OutMessages, OutError);
        if (!RootNode)
        {
            return false;
        }

        const bool bUseSeparateRotationComponent = !RotationComponentName.Equals(MeshName, ESearchCase::IgnoreCase);
        if (bUseSeparateRotationComponent)
        {
            if (!EnsureSceneComponentNode(Blueprint, RotationComponentName, USceneComponent::StaticClass(), RootName, OutMessages, OutError))
            {
                return false;
            }
        }

        const FString MeshParentName = bUseSeparateRotationComponent ? RotationComponentName : RootName;
        USCS_Node* MeshNode = EnsureSceneComponentNode(Blueprint, MeshName, UStaticMeshComponent::StaticClass(), MeshParentName, OutMessages, OutError);
        if (!MeshNode)
        {
            return false;
        }

        UStaticMeshComponent* MeshTemplate = Cast<UStaticMeshComponent>(MeshNode->ComponentTemplate);
        if (!MeshTemplate)
        {
            OutError = FString::Printf(TEXT("%s is not a StaticMeshComponent"), *MeshName);
            return false;
        }

        if (UStaticMesh* Mesh = LoadStaticMeshAsset(MeshPath))
        {
            MeshTemplate->Modify();
            MeshTemplate->SetStaticMesh(Mesh);
        }
        else
        {
            OutError = FString::Printf(TEXT("Static mesh not found: %s"), *MeshPath);
            return false;
        }
        ApplySceneTransform(MeshNode, MeshLocation, FRotator::ZeroRotator, MeshScale);
        MeshTemplate->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        MeshTemplate->SetCollisionProfileName(TEXT("BlockAllDynamic"));

        USCS_Node* CollisionNode = EnsureSceneComponentNode(Blueprint, CollisionName, UBoxComponent::StaticClass(), RootName, OutMessages, OutError);
        if (!CollisionNode)
        {
            return false;
        }

        UBoxComponent* CollisionTemplate = Cast<UBoxComponent>(CollisionNode->ComponentTemplate);
        if (!CollisionTemplate)
        {
            OutError = FString::Printf(TEXT("%s is not a BoxComponent"), *CollisionName);
            return false;
        }
        CollisionTemplate->Modify();
        CollisionTemplate->SetBoxExtent(CollisionExtent);
        CollisionTemplate->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        CollisionTemplate->SetCollisionResponseToAllChannels(ECR_Ignore);
        CollisionTemplate->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
        CollisionTemplate->SetGenerateOverlapEvents(true);
        ApplySceneTransform(CollisionNode, CollisionLocation, FRotator::ZeroRotator, FVector::OneVector);

        FString PromptComponentForGraph;
        USCS_Node* PromptNode = nullptr;
        if (UClass* WidgetClass = LoadWidgetClassAsset(PromptWidgetClassPath))
        {
            PromptNode = EnsureSceneComponentNode(Blueprint, PromptName, UWidgetComponent::StaticClass(), RootName, OutMessages, OutError);
            if (!PromptNode)
            {
                return false;
            }

            UWidgetComponent* WidgetTemplate = Cast<UWidgetComponent>(PromptNode->ComponentTemplate);
            if (!WidgetTemplate)
            {
                OutError = FString::Printf(TEXT("%s is not a WidgetComponent"), *PromptName);
                return false;
            }
            WidgetTemplate->Modify();
            WidgetTemplate->SetWidgetClass(WidgetClass);
            WidgetTemplate->SetDrawSize(FVector2D(280.0, 80.0));
            WidgetTemplate->SetWidgetSpace(EWidgetSpace::Screen);
            WidgetTemplate->SetVisibility(false);
            ApplySceneTransform(PromptNode, PromptLocation, FRotator::ZeroRotator, FVector::OneVector);
            PromptComponentForGraph = PromptName;
        }
        else
        {
            PromptNode = EnsureSceneComponentNode(Blueprint, PromptName, UTextRenderComponent::StaticClass(), RootName, OutMessages, OutError);
            if (!PromptNode)
            {
                return false;
            }

            UTextRenderComponent* TextTemplate = Cast<UTextRenderComponent>(PromptNode->ComponentTemplate);
            if (!TextTemplate)
            {
                OutError = FString::Printf(TEXT("%s is not a TextRenderComponent"), *PromptName);
                return false;
            }
            TextTemplate->Modify();
            TextTemplate->SetText(FText::FromString(PromptText));
            TextTemplate->SetHorizontalAlignment(EHTA_Center);
            TextTemplate->SetVerticalAlignment(EVRTA_TextCenter);
            TextTemplate->SetTextRenderColor(FColor::White);
            TextTemplate->SetVisibility(false);
            ApplySceneTransform(PromptNode, PromptLocation, FRotator(0.0, 180.0, 0.0), PromptScale);
            PromptComponentForGraph = PromptName;
        }

        EnsureBoolVariable(Blueprint, CanInteractVar, OutMessages);
        EnsureBoolVariable(Blueprint, IsActiveVar, OutMessages);

        UEdGraph* Graph = Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0].Get() : nullptr;
        if (!Graph)
        {
            OutError = TEXT("Blueprint has no EventGraph");
            return false;
        }

        const FKey InteractionKey = KeyName.Equals(TEXT("E"), ESearchCase::IgnoreCase) ? EKeys::E : FKey(FName(*KeyName));
        const FName TimelineName(*TimelineNameString);
        const FName TrackName(*TimelineTrackString);
        const bool bHasKeyNode = HasInputKeyNode(Graph, InteractionKey);
        UK2Node_Timeline* TimelineNode = EnsureTimelineNode(Blueprint, Graph, TimelineName, TrackName, Duration, 1.0f, 180, 560, OutMessages, OutError);
        if (!TimelineNode)
        {
            return false;
        }

        if (bHasKeyNode)
        {
            LayoutInteractableActorGraph(Graph, InteractionKey, TimelineName, RotationComponentName, PromptComponentForGraph, CanInteractVar, IsActiveVar, OutMessages);
            OutMessages.Add(FString::Printf(TEXT("Input key %s already exists; refreshed normalized timeline and graph layout"), *InteractionKey.ToString()));
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            return true;
        }

        UFunction* GetPlayerControllerFunction = UGameplayStatics::StaticClass()->FindFunctionByName(TEXT("GetPlayerController"));
        UFunction* EnableInputFunction = AActor::StaticClass()->FindFunctionByName(TEXT("EnableInput"));
        UFunction* DisableInputFunction = AActor::StaticClass()->FindFunctionByName(TEXT("DisableInput"));
        UFunction* LerpFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Lerp"));
        UFunction* MakeRotatorFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("MakeRotator"));
        UFunction* SetRelativeRotationFunction = USceneComponent::StaticClass()->FindFunctionByName(TEXT("K2_SetRelativeRotation"));
        if (!GetPlayerControllerFunction || !EnableInputFunction || !DisableInputFunction || !LerpFunction || !MakeRotatorFunction || !SetRelativeRotationFunction)
        {
            OutError = TEXT("One or more reflected functions for interactable actor flow were not found");
            return false;
        }

        UK2Node_Event* BeginOverlap = EnsureActorEvent(Graph, TEXT("ReceiveActorBeginOverlap"), -1180, -360, OutMessages);
        UK2Node_VariableSet* SetCanInteractTrue = AddVariableSetNode(Graph, CanInteractVar, true, -840, -360);
        UK2Node_CallFunction* GetPcForEnable = AddCallNode(Graph, GetPlayerControllerFunction, -520, -70, OutError);
        UK2Node_CallFunction* EnableInput = AddCallNode(Graph, EnableInputFunction, -180, -360, OutError);
        UK2Node_CallFunction* ShowPrompt = nullptr;
        if (!GetPcForEnable || !EnableInput ||
            !AddPromptVisibilityCall(Graph, PromptComponentForGraph, true, -520, -360, ShowPrompt, OutError))
        {
            return false;
        }
        SetPinDefaultValue(ABT::Blueprint::FindPinByName(GetPcForEnable, TEXT("PlayerIndex")), TEXT("0"));
        if (!ConnectPins(ABT::Blueprint::FindFirstPin(BeginOverlap, EGPD_Output, UEdGraphSchema_K2::PC_Exec), ABT::Blueprint::FindFirstPin(SetCanInteractTrue, EGPD_Input, UEdGraphSchema_K2::PC_Exec), OutError) ||
            !ConnectPins(ABT::Blueprint::FindFirstPin(SetCanInteractTrue, EGPD_Output, UEdGraphSchema_K2::PC_Exec), ABT::Blueprint::FindFirstPin(EnableInput, EGPD_Input, UEdGraphSchema_K2::PC_Exec), OutError) ||
            !ConnectPins(ABT::Blueprint::FindPinByName(GetPcForEnable, TEXT("ReturnValue")), ABT::Blueprint::FindPinByName(EnableInput, TEXT("PlayerController")), OutError))
        {
            return false;
        }
        if (ShowPrompt && !ConnectPins(ABT::Blueprint::FindFirstPin(EnableInput, EGPD_Output, UEdGraphSchema_K2::PC_Exec), ABT::Blueprint::FindFirstPin(ShowPrompt, EGPD_Input, UEdGraphSchema_K2::PC_Exec), OutError))
        {
            return false;
        }

        UK2Node_Event* EndOverlap = EnsureActorEvent(Graph, TEXT("ReceiveActorEndOverlap"), -1180, 40, OutMessages);
        UK2Node_VariableSet* SetCanInteractFalse = AddVariableSetNode(Graph, CanInteractVar, false, -840, 40);
        UK2Node_CallFunction* GetPcForDisable = AddCallNode(Graph, GetPlayerControllerFunction, -520, 330, OutError);
        UK2Node_CallFunction* DisableInput = AddCallNode(Graph, DisableInputFunction, -180, 40, OutError);
        UK2Node_CallFunction* HidePrompt = nullptr;
        if (!GetPcForDisable || !DisableInput ||
            !AddPromptVisibilityCall(Graph, PromptComponentForGraph, false, -520, 40, HidePrompt, OutError))
        {
            return false;
        }
        SetPinDefaultValue(ABT::Blueprint::FindPinByName(GetPcForDisable, TEXT("PlayerIndex")), TEXT("0"));
        if (!ConnectPins(ABT::Blueprint::FindFirstPin(EndOverlap, EGPD_Output, UEdGraphSchema_K2::PC_Exec), ABT::Blueprint::FindFirstPin(SetCanInteractFalse, EGPD_Input, UEdGraphSchema_K2::PC_Exec), OutError) ||
            !ConnectPins(ABT::Blueprint::FindFirstPin(SetCanInteractFalse, EGPD_Output, UEdGraphSchema_K2::PC_Exec), ABT::Blueprint::FindFirstPin(DisableInput, EGPD_Input, UEdGraphSchema_K2::PC_Exec), OutError) ||
            !ConnectPins(ABT::Blueprint::FindPinByName(GetPcForDisable, TEXT("ReturnValue")), ABT::Blueprint::FindPinByName(DisableInput, TEXT("PlayerController")), OutError))
        {
            return false;
        }
        if (HidePrompt && !ConnectPins(ABT::Blueprint::FindFirstPin(DisableInput, EGPD_Output, UEdGraphSchema_K2::PC_Exec), ABT::Blueprint::FindFirstPin(HidePrompt, EGPD_Input, UEdGraphSchema_K2::PC_Exec), OutError))
        {
            return false;
        }

        UK2Node_InputKey* InputKeyNode = AddGraphNode<UK2Node_InputKey>(Graph, -1180, 560);
        InputKeyNode->InputKey = InteractionKey;
        InputKeyNode->bConsumeInput = true;
        InputKeyNode->AllocateDefaultPins();

        UK2Node_VariableGet* GetCanInteract = AddVariableGetNode(Graph, CanInteractVar, -1180, 760);
        UK2Node_IfThenElse* CanInteractBranch = AddGraphNode<UK2Node_IfThenElse>(Graph, -840, 560);
        CanInteractBranch->AllocateDefaultPins();
        UK2Node_VariableGet* GetIsActive = AddVariableGetNode(Graph, IsActiveVar, -840, 900);
        UK2Node_IfThenElse* IsActiveBranch = AddGraphNode<UK2Node_IfThenElse>(Graph, -520, 560);
        IsActiveBranch->AllocateDefaultPins();
        UK2Node_VariableSet* SetInactive = AddVariableSetNode(Graph, IsActiveVar, false, -180, 460);
        UK2Node_VariableSet* SetActive = AddVariableSetNode(Graph, IsActiveVar, true, -180, 700);

        if (!ConnectPins(InputKeyNode->GetPressedPin(), CanInteractBranch->GetExecPin(), OutError) ||
            !ConnectPins(ABT::Blueprint::FindPinByName(GetCanInteract, CanInteractVar), CanInteractBranch->GetConditionPin(), OutError) ||
            !ConnectPins(CanInteractBranch->GetThenPin(), IsActiveBranch->GetExecPin(), OutError) ||
            !ConnectPins(ABT::Blueprint::FindPinByName(GetIsActive, IsActiveVar), IsActiveBranch->GetConditionPin(), OutError) ||
            !ConnectPins(IsActiveBranch->GetThenPin(), ABT::Blueprint::FindFirstPin(SetInactive, EGPD_Input, UEdGraphSchema_K2::PC_Exec), OutError) ||
            !ConnectPins(IsActiveBranch->GetElsePin(), ABT::Blueprint::FindFirstPin(SetActive, EGPD_Input, UEdGraphSchema_K2::PC_Exec), OutError) ||
            !ConnectPins(ABT::Blueprint::FindFirstPin(SetInactive, EGPD_Output, UEdGraphSchema_K2::PC_Exec), TimelineNode->GetReverseFromEndPin(), OutError) ||
            !ConnectPins(ABT::Blueprint::FindFirstPin(SetActive, EGPD_Output, UEdGraphSchema_K2::PC_Exec), TimelineNode->GetPlayFromStartPin(), OutError))
        {
            return false;
        }

        UK2Node_VariableGet* GetRotationComponent = AddVariableGetNode(Graph, RotationComponentName, 880, 420);
        UK2Node_CallFunction* Lerp = AddCallNode(Graph, LerpFunction, 520, 800, OutError);
        UK2Node_CallFunction* MakeRotator = AddCallNode(Graph, MakeRotatorFunction, 820, 760, OutError);
        UK2Node_CallFunction* SetRelativeRotation = AddCallNode(Graph, SetRelativeRotationFunction, 1180, 560, OutError);
        if (!GetRotationComponent || !Lerp || !MakeRotator || !SetRelativeRotation)
        {
            return false;
        }

        SetPinDefaultValue(ABT::Blueprint::FindPinByName(Lerp, TEXT("A")), TEXT("0.0"));
        SetPinDefaultValue(ABT::Blueprint::FindPinByName(Lerp, TEXT("B")), FString::SanitizeFloat(OpenAngle));
        SetPinDefaultValue(ABT::Blueprint::FindPinByName(MakeRotator, TEXT("Roll")), TEXT("0.0"));
        SetPinDefaultValue(ABT::Blueprint::FindPinByName(MakeRotator, TEXT("Pitch")), TEXT("0.0"));
        SetPinDefaultValue(ABT::Blueprint::FindPinByName(SetRelativeRotation, TEXT("bSweep")), TEXT("false"));
        SetPinDefaultValue(ABT::Blueprint::FindPinByName(SetRelativeRotation, TEXT("bTeleport")), TEXT("false"));

        if (!ConnectPins(TimelineNode->GetUpdatePin(), ABT::Blueprint::FindFirstPin(SetRelativeRotation, EGPD_Input, UEdGraphSchema_K2::PC_Exec), OutError) ||
            !ConnectPins(ABT::Blueprint::FindPinByName(TimelineNode, TrackName.ToString()), ABT::Blueprint::FindPinByName(Lerp, TEXT("Alpha")), OutError) ||
            !ConnectPins(ABT::Blueprint::FindPinByName(Lerp, TEXT("ReturnValue")), ABT::Blueprint::FindPinByName(MakeRotator, TEXT("Yaw")), OutError) ||
            !ConnectPins(ABT::Blueprint::FindPinByName(MakeRotator, TEXT("ReturnValue")), ABT::Blueprint::FindPinByName(SetRelativeRotation, TEXT("NewRotation")), OutError))
        {
            return false;
        }

        UEdGraphPin* RotationTargetPin = ABT::Blueprint::FindPinByName(SetRelativeRotation, TEXT("self"));
        if (!RotationTargetPin)
        {
            RotationTargetPin = ABT::Blueprint::FindPinByName(SetRelativeRotation, TEXT("Target"));
        }
        if (!ConnectPins(ABT::Blueprint::FindPinByName(GetRotationComponent, RotationComponentName), RotationTargetPin, OutError))
        {
            return false;
        }

        LayoutInteractableActorGraph(Graph, InteractionKey, TimelineName, RotationComponentName, PromptComponentForGraph, CanInteractVar, IsActiveVar, OutMessages);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        OutMessages.Add(FString::Printf(TEXT("Configured interactable actor with key %s, normalized timeline %s, and %.2f degree rotation"), *InteractionKey.ToString(), *TimelineName.ToString(), OpenAngle));
        return true;
    }
}
