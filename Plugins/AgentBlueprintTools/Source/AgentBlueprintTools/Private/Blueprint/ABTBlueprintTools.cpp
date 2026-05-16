#include "Blueprint/ABTBlueprintTools.h"
#include "Utils/ABTJson.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintActionDatabase.h"
#include "Components/ActorComponent.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/MeshComponent.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
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
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"
#include "Editor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"

namespace
{
    const FName NAME_ReceiveBeginPlay(TEXT("ReceiveBeginPlay"));

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

    TArray<TSharedPtr<FJsonObject>> ReadPageSpecs(const TSharedPtr<FJsonObject>& Op)
    {
        TArray<TSharedPtr<FJsonObject>> Result;
        const TArray<TSharedPtr<FJsonValue>>* Pages = nullptr;
        if (Op.IsValid() && Op->TryGetArrayField(TEXT("pages"), Pages) && Pages)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Pages)
            {
                if (Value.IsValid() && Value->AsObject().IsValid())
                {
                    Result.Add(Value->AsObject());
                }
            }
        }

        if (Result.Num() == 0)
        {
            for (int32 Index = 0; Index < 3; ++Index)
            {
                TSharedPtr<FJsonObject> Page = ABTJson::Object();
                Page->SetStringField(TEXT("buttonText"), FString::Printf(TEXT("Page %d"), Index + 1));
                Page->SetStringField(TEXT("title"), FString::Printf(TEXT("Sub Page %d"), Index + 1));
                Page->SetStringField(TEXT("body"), TEXT("Created by AgentBlueprintTools."));
                Result.Add(Page);
            }
        }

        return Result;
    }

    bool ConfigureButtonPagesWidget(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError)
    {
        UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint);
        if (!WidgetBlueprint)
        {
            OutError = TEXT("configure_button_pages_widget requires a WidgetBlueprint target");
            return false;
        }
        if (!WidgetBlueprint->WidgetTree)
        {
            WidgetBlueprint->WidgetTree = NewObject<UWidgetTree>(WidgetBlueprint, TEXT("WidgetTree"), RF_Transactional);
        }

        UWidgetTree* Tree = WidgetBlueprint->WidgetTree;
        Tree->Modify();

        const TArray<TSharedPtr<FJsonObject>> Pages = ReadPageSpecs(Op);
        if (Pages.Num() > 10)
        {
            OutError = TEXT("configure_button_pages_widget supports at most 10 pages");
            return false;
        }

        UVerticalBox* Root = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Root"));
        UHorizontalBox* ButtonList = Tree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("ButtonList"));
        UOverlay* PageLayer = Tree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("PageLayer"));

        if (!Root || !ButtonList || !PageLayer)
        {
            OutError = TEXT("Failed to construct widget tree");
            return false;
        }

        Tree->RootWidget = Root;
        if (UVerticalBoxSlot* ButtonSlot = Root->AddChildToVerticalBox(ButtonList))
        {
            ButtonSlot->SetPadding(FMargin(16.0f, 16.0f, 16.0f, 8.0f));
            ButtonSlot->SetHorizontalAlignment(HAlign_Center);
        }
        if (UVerticalBoxSlot* PageSlot = Root->AddChildToVerticalBox(PageLayer))
        {
            PageSlot->SetPadding(FMargin(16.0f, 8.0f, 16.0f, 16.0f));
            PageSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        }

        for (int32 Index = 0; Index < Pages.Num(); ++Index)
        {
            const TSharedPtr<FJsonObject>& Page = Pages[Index];
            const FString ButtonText = ABTJson::GetString(Page, TEXT("buttonText"), FString::Printf(TEXT("Page %d"), Index + 1));
            const FString TitleText = ABTJson::GetString(Page, TEXT("title"), ButtonText);
            const FString BodyText = ABTJson::GetString(Page, TEXT("body"), TEXT(""));

            UButton* Button = Tree->ConstructWidget<UButton>(UButton::StaticClass(), *FString::Printf(TEXT("PageButton_%d"), Index));
            UTextBlock* ButtonLabel = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), *FString::Printf(TEXT("PageButtonLabel_%d"), Index));
            UBorder* PagePanel = Tree->ConstructWidget<UBorder>(UBorder::StaticClass(), *FString::Printf(TEXT("PagePanel_%d"), Index));
            UVerticalBox* PageContent = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), *FString::Printf(TEXT("PageContent_%d"), Index));
            UTextBlock* PageTitle = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), *FString::Printf(TEXT("PageTitle_%d"), Index));
            UTextBlock* PageBody = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), *FString::Printf(TEXT("PageBody_%d"), Index));

            if (!Button || !ButtonLabel || !PagePanel || !PageContent || !PageTitle || !PageBody)
            {
                OutError = TEXT("Failed to construct a button page widget");
                return false;
            }

            Button->bIsVariable = true;
            PagePanel->bIsVariable = true;

            ButtonLabel->SetText(FText::FromString(ButtonText));
            ButtonLabel->SetJustification(ETextJustify::Center);
            Button->AddChild(ButtonLabel);
            if (UHorizontalBoxSlot* ButtonBoxSlot = ButtonList->AddChildToHorizontalBox(Button))
            {
                ButtonBoxSlot->SetPadding(FMargin(4.0f, 0.0f));
            }

            PageTitle->SetText(FText::FromString(TitleText));
            PageTitle->SetJustification(ETextJustify::Center);
            PageBody->SetText(FText::FromString(BodyText));
            PageBody->SetAutoWrapText(true);
            PageBody->SetJustification(ETextJustify::Center);
            PageContent->AddChildToVerticalBox(PageTitle);
            PageContent->AddChildToVerticalBox(PageBody);

            PagePanel->SetPadding(FMargin(24.0f));
            PagePanel->SetVisibility(ESlateVisibility::Collapsed);
            PagePanel->SetRenderOpacity(0.5f);
            FWidgetTransform Transform = PagePanel->GetRenderTransform();
            Transform.Scale = FVector2D(0.2f, 0.2f);
            PagePanel->SetRenderTransform(Transform);
            PagePanel->AddChild(PageContent);

            if (UOverlaySlot* OverlaySlot = PageLayer->AddChildToOverlay(PagePanel))
            {
                OverlaySlot->SetHorizontalAlignment(HAlign_Center);
                OverlaySlot->SetVerticalAlignment(VAlign_Center);
                OverlaySlot->SetPadding(FMargin(24.0f));
            }
        }

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
        OutMessages.Add(FString::Printf(TEXT("Configured button pages widget with %d pages"), Pages.Num()));
        return true;
    }

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
        return FindPinByName(Node, ParamName.ToString());
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
            UEdGraphPin* FunctionNamePin = FindPinByName(Call, TEXT("FunctionName"));
            if (FunctionNamePin && FunctionNamePin->DefaultValue.Equals(FunctionName, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
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
            OutError = TEXT("Could not connect BeginPlay to timer loop");
            return false;
        }

        OutMessages.Add(FString::Printf(TEXT("Ensured timer loop for %s"), *FunctionName));
        return true;
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

    if (OpName == TEXT("ensure_timer_loop"))
    {
        return EnsureTimerLoop(Blueprint, Op, OutMessages, OutError);
    }

    if (OpName == TEXT("ensure_looping_move"))
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

        if (!ApplyOperation(Blueprint, MoveOp, NodeMap, OutMessages, OutError))
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

    if (OpName == TEXT("configure_button_pages_widget"))
    {
        return ConfigureButtonPagesWidget(Blueprint, Op, OutMessages, OutError);
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
