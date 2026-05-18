#include "Blueprint/Ops/ABTGraphBlueprintOps.h"
#include "Blueprint/Utils/ABTBlueprintGraphUtils.h"
#include "Utils/ABTJson.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace ABT::Blueprint::Ops
{
    bool EnsureVariable(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError)
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

    bool EnsureFunction(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, TArray<FString>& OutMessages, FString& OutError)
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

    bool AddNode(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, TArray<FString>& OutMessages, FString& OutError)
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

    bool ConnectPins(const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, TArray<FString>& OutMessages, FString& OutError)
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

    bool SetPinDefault(const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, FString& OutError)
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
}
