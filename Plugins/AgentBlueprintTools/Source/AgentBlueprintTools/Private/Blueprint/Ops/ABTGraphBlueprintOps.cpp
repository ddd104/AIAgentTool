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
#include "UObject/UnrealType.h"

namespace ABT::Blueprint::Ops
{
    namespace
    {
        UEdGraphNode* ResolvePatchNode(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, const FString& NodeRef)
        {
            if (NodeRef.IsEmpty())
            {
                return nullptr;
            }

            if (UEdGraphNode* const* Existing = NodeMap.Find(NodeRef))
            {
                return *Existing;
            }

            FString GraphName = ABTJson::GetString(Op, TEXT("graph"));
            FString NodeId = NodeRef;
            FString RefGraphName;
            FString RefNodeId;
            if (NodeRef.Split(TEXT(":"), &RefGraphName, &RefNodeId) && !RefGraphName.IsEmpty() && !RefNodeId.IsEmpty())
            {
                GraphName = RefGraphName;
                NodeId = RefNodeId;
            }

            if (UEdGraphNode* const* Existing = NodeMap.Find(NodeId))
            {
                return *Existing;
            }

            return FindExistingNodeInBlueprint(Blueprint, GraphName, NodeId);
        }

        bool SetSimpleObjectProperty(UObject* Object, const TSharedPtr<FJsonObject>& Op, FString& OutError)
        {
            if (!Object)
            {
                OutError = TEXT("object is null");
                return false;
            }

            const FString PropertyName = ABTJson::GetString(Op, TEXT("property"));
            if (PropertyName.IsEmpty())
            {
                OutError = TEXT("set_blueprint_property.property missing");
                return false;
            }

            FProperty* Property = Object->GetClass()->FindPropertyByName(*PropertyName);
            if (!Property)
            {
                OutError = FString::Printf(TEXT("Property not found: %s"), *PropertyName);
                return false;
            }

            const TSharedPtr<FJsonValue> Value = Op->TryGetField(TEXT("value"));
            if (!Value.IsValid())
            {
                OutError = TEXT("set_blueprint_property.value missing");
                return false;
            }

            void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
            if (FBoolProperty* Bool = CastField<FBoolProperty>(Property))
            {
                Bool->SetPropertyValue(ValuePtr, Value->AsBool());
                return true;
            }

            if (FNumericProperty* Number = CastField<FNumericProperty>(Property))
            {
                if (Number->IsInteger())
                {
                    Number->SetIntPropertyValue(ValuePtr, static_cast<int64>(Value->AsNumber()));
                }
                else
                {
                    Number->SetFloatingPointPropertyValue(ValuePtr, Value->AsNumber());
                }
                return true;
            }

            if (FStrProperty* String = CastField<FStrProperty>(Property))
            {
                String->SetPropertyValue(ValuePtr, Value->AsString());
                return true;
            }

            if (FNameProperty* Name = CastField<FNameProperty>(Property))
            {
                Name->SetPropertyValue(ValuePtr, *Value->AsString());
                return true;
            }

            if (FTextProperty* Text = CastField<FTextProperty>(Property))
            {
                Text->SetPropertyValue(ValuePtr, FText::FromString(Value->AsString()));
                return true;
            }

            if (FEnumProperty* Enum = CastField<FEnumProperty>(Property))
            {
                const int64 EnumValue = Enum->GetEnum()->GetValueByNameString(Value->AsString());
                if (EnumValue == INDEX_NONE)
                {
                    OutError = FString::Printf(TEXT("Invalid enum value '%s' for %s"), *Value->AsString(), *PropertyName);
                    return false;
                }
                Enum->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, EnumValue);
                return true;
            }

            if (FByteProperty* Byte = CastField<FByteProperty>(Property))
            {
                if (Byte->Enum)
                {
                    const int64 EnumValue = Byte->Enum->GetValueByNameString(Value->AsString());
                    if (EnumValue == INDEX_NONE)
                    {
                        OutError = FString::Printf(TEXT("Invalid enum value '%s' for %s"), *Value->AsString(), *PropertyName);
                        return false;
                    }
                    Byte->SetPropertyValue(ValuePtr, static_cast<uint8>(EnumValue));
                }
                else
                {
                    Byte->SetPropertyValue(ValuePtr, static_cast<uint8>(Value->AsNumber()));
                }
                return true;
            }

            if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
            {
                UObject* Referenced = nullptr;
                const FString ObjectPath = Value->AsString();
                if (!ObjectPath.IsEmpty())
                {
                    Referenced = LoadObject<UObject>(nullptr, *ObjectPath);
                    if (!Referenced)
                    {
                        OutError = FString::Printf(TEXT("Could not load object value: %s"), *ObjectPath);
                        return false;
                    }
                    if (!Referenced->IsA(ObjectProperty->PropertyClass))
                    {
                        OutError = FString::Printf(TEXT("Object value is not a %s: %s"), *ObjectProperty->PropertyClass->GetName(), *ObjectPath);
                        return false;
                    }
                }
                ObjectProperty->SetObjectPropertyValue(ValuePtr, Referenced);
                return true;
            }

            OutError = FString::Printf(TEXT("Unsupported property type: %s"), *Property->GetClass()->GetName());
            return false;
        }
    }

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

    bool ConnectPins(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, TArray<FString>& OutMessages, FString& OutError)
    {
        FString FromNodeId, FromPinName, ToNodeId, ToPinName;
        if (!ParseNodePinRef(ABTJson::GetString(Op, TEXT("from")), FromNodeId, FromPinName) ||
            !ParseNodePinRef(ABTJson::GetString(Op, TEXT("to")), ToNodeId, ToPinName))
        {
            OutError = TEXT("connect op requires from/to in node.pin form");
            return false;
        }

        UEdGraphNode* FromNode = ResolvePatchNode(Blueprint, Op, NodeMap, FromNodeId);
        UEdGraphNode* ToNode = ResolvePatchNode(Blueprint, Op, NodeMap, ToNodeId);
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

    bool SetPinDefault(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, TArray<FString>& OutMessages, FString& OutError)
    {
        FString NodeId, PinName;
        if (!ParseNodePinRef(ABTJson::GetString(Op, TEXT("target")), NodeId, PinName))
        {
            OutError = TEXT("set_pin_default.target must be node.pin");
            return false;
        }
        UEdGraphNode* Node = ResolvePatchNode(Blueprint, Op, NodeMap, NodeId);
        UEdGraphPin* Pin = FindPinByName(Node, PinName);
        if (!Pin) { OutError = TEXT("pin not found"); return false; }

        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        Schema->TrySetDefaultValue(*Pin, ABTJson::GetString(Op, TEXT("value")));
        OutMessages.Add(FString::Printf(TEXT("Set default %s = %s"), *ABTJson::GetString(Op, TEXT("target")), *ABTJson::GetString(Op, TEXT("value"))));
        return true;
    }

    bool RemoveNode(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, TArray<FString>& OutMessages, FString& OutError)
    {
        const FString NodeRef = ABTJson::GetString(Op, TEXT("node"), ABTJson::GetString(Op, TEXT("target")));
        UEdGraphNode* Node = ResolvePatchNode(Blueprint, Op, NodeMap, NodeRef);
        if (!Node)
        {
            OutError = FString::Printf(TEXT("remove_node referenced unknown node: %s"), *NodeRef);
            return false;
        }

        const FString RemovedTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
        FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);

        for (auto It = NodeMap.CreateIterator(); It; ++It)
        {
            if (It.Value() == Node)
            {
                It.RemoveCurrent();
            }
        }

        OutMessages.Add(FString::Printf(TEXT("Removed node %s (%s)"), *NodeRef, *RemovedTitle));
        return true;
    }

    bool SetBlueprintProperty(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError)
    {
        if (!Blueprint)
        {
            OutError = TEXT("set_blueprint_property called with null Blueprint");
            return false;
        }

        UObject* Object = Blueprint;
        const FString ObjectName = ABTJson::GetString(Op, TEXT("object"), TEXT("DefaultObject"));
        if (ObjectName.Equals(TEXT("DefaultObject"), ESearchCase::IgnoreCase))
        {
            Object = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
        }
        else if (!ObjectName.Equals(TEXT("Blueprint"), ESearchCase::IgnoreCase) && !ObjectName.Equals(TEXT("Asset"), ESearchCase::IgnoreCase))
        {
            OutError = FString::Printf(TEXT("Unsupported set_blueprint_property.object: %s"), *ObjectName);
            return false;
        }

        if (!Object)
        {
            OutError = TEXT("Could not resolve requested Blueprint object");
            return false;
        }

        Object->Modify();
        if (!SetSimpleObjectProperty(Object, Op, OutError))
        {
            return false;
        }

        if (UPackage* Package = Blueprint->GetOutermost())
        {
            Package->SetDirtyFlag(true);
        }

        OutMessages.Add(FString::Printf(TEXT("Set %s.%s"), *ObjectName, *ABTJson::GetString(Op, TEXT("property"))));
        return true;
    }
}
