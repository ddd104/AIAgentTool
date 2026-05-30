#include "Blueprint/Ops/ABTActorBlueprintOps.h"
#include "Blueprint/Utils/ABTBlueprintGraphUtils.h"
#include "Utils/ABTJson.h"

#include "Components/ActorComponent.h"
#include "Components/MeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet/KismetSystemLibrary.h"
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
        UEdGraph* Graph = Blueprint && Blueprint->UbergraphPages.Num() ? Blueprint->UbergraphPages[0] : nullptr;
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
}
