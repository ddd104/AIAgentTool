#include "Assets/ABTAssetTools.h"
#include "Utils/ABTJson.h"

#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Factories/BlueprintFactory.h"
#include "Factories/MaterialFactoryNew.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "GameFramework/Actor.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "ScopedTransaction.h"
#include "Engine/UserDefinedStruct.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "EdGraphSchema_K2.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "UObject/Interface.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Blueprint/UserWidget.h"

namespace
{
    bool SplitAssetPath(const FString& FullPath, FString& OutPackagePath, FString& OutAssetName)
    {
        int32 Slash = INDEX_NONE;
        if (!FullPath.FindLastChar(TEXT('/'), Slash) || Slash <= 0) return false;
        OutPackagePath = FullPath.Left(Slash);
        OutAssetName = FullPath.Mid(Slash + 1);
        return !OutPackagePath.IsEmpty() && !OutAssetName.IsEmpty();
    }

    bool SetSimpleProperty(UObject* Object, const FString& PropertyName, const TSharedPtr<FJsonObject>& Request, FString& OutError)
    {
        if (!Object) { OutError = TEXT("object is null"); return false; }
        FProperty* Property = Object->GetClass()->FindPropertyByName(*PropertyName);
        if (!Property)
        {
            OutError = FString::Printf(TEXT("Property not found: %s"), *PropertyName);
            return false;
        }

        void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
        const TSharedPtr<FJsonValue> Value = Request->TryGetField(TEXT("value"));
        if (!Value.IsValid()) { OutError = TEXT("value field missing"); return false; }

        if (FBoolProperty* Bool = CastField<FBoolProperty>(Property))
        {
            Bool->SetPropertyValue(ValuePtr, Value->AsBool());
            return true;
        }
        if (FNumericProperty* Num = CastField<FNumericProperty>(Property))
        {
            if (Num->IsInteger()) Num->SetIntPropertyValue(ValuePtr, static_cast<int64>(Value->AsNumber()));
            else Num->SetFloatingPointPropertyValue(ValuePtr, Value->AsNumber());
            return true;
        }
        if (FStrProperty* Str = CastField<FStrProperty>(Property))
        {
            Str->SetPropertyValue(ValuePtr, Value->AsString());
            return true;
        }
        if (FNameProperty* Name = CastField<FNameProperty>(Property))
        {
            Name->SetPropertyValue(ValuePtr, *Value->AsString());
            return true;
        }

        OutError = FString::Printf(TEXT("Unsupported property type: %s"), *Property->GetClass()->GetName());
        return false;
    }

    TSharedPtr<FJsonValue> ExportSimpleProperty(UObject* Object, const FString& PropertyName, FString& OutError)
    {
        if (!Object)
        {
            OutError = TEXT("object is null");
            return nullptr;
        }

        FProperty* Property = Object->GetClass()->FindPropertyByName(*PropertyName);
        if (!Property)
        {
            OutError = FString::Printf(TEXT("Property not found: %s"), *PropertyName);
            return nullptr;
        }

        const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
        if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property))
        {
            return MakeShared<FJsonValueBoolean>(Bool->GetPropertyValue(ValuePtr));
        }
        if (const FNumericProperty* Num = CastField<FNumericProperty>(Property))
        {
            if (Num->IsInteger())
            {
                return MakeShared<FJsonValueNumber>(static_cast<double>(Num->GetSignedIntPropertyValue(ValuePtr)));
            }
            return MakeShared<FJsonValueNumber>(Num->GetFloatingPointPropertyValue(ValuePtr));
        }
        if (const FStrProperty* Str = CastField<FStrProperty>(Property))
        {
            return MakeShared<FJsonValueString>(Str->GetPropertyValue(ValuePtr));
        }
        if (const FNameProperty* Name = CastField<FNameProperty>(Property))
        {
            return MakeShared<FJsonValueString>(Name->GetPropertyValue(ValuePtr).ToString());
        }
        if (const FTextProperty* Text = CastField<FTextProperty>(Property))
        {
            return MakeShared<FJsonValueString>(Text->GetPropertyValue(ValuePtr).ToString());
        }
        if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
        {
            const int64 Value = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
            return MakeShared<FJsonValueString>(EnumProperty->GetEnum()->GetNameStringByValue(Value));
        }
        if (const FByteProperty* Byte = CastField<FByteProperty>(Property))
        {
            if (Byte->Enum)
            {
                return MakeShared<FJsonValueString>(Byte->Enum->GetNameStringByValue(Byte->GetPropertyValue(ValuePtr)));
            }
            return MakeShared<FJsonValueNumber>(Byte->GetPropertyValue(ValuePtr));
        }
        if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
        {
            UObject* Referenced = ObjectProperty->GetObjectPropertyValue(ValuePtr);
            return MakeShared<FJsonValueString>(Referenced ? Referenced->GetPathName() : FString());
        }

        OutError = FString::Printf(TEXT("Unsupported property type: %s"), *Property->GetClass()->GetName());
        return nullptr;
    }

    UClass* ResolveClass(const FString& ClassNameOrPath)
    {
        if (ClassNameOrPath.IsEmpty())
        {
            return nullptr;
        }

        if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassNameOrPath))
        {
            return Loaded;
        }

        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Candidate = *It;
            if (Candidate && (Candidate->GetName().Equals(ClassNameOrPath, ESearchCase::IgnoreCase) ||
                Candidate->GetPathName().Equals(ClassNameOrPath, ESearchCase::IgnoreCase)))
            {
                return Candidate;
            }
        }

        return nullptr;
    }

    FEdGraphPinType MakeStructPinTypeFromString(const FString& TypeName)
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
        else if (TypeName.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
        }
        else
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
        }
        return PinType;
    }

    FVector ReadVectorField(const TSharedPtr<FJsonObject>& Object, const FString& Field, const FVector& DefaultValue)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values || Values->Num() < 3)
        {
            return DefaultValue;
        }
        return FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
    }

    FRotator ReadRotatorField(const TSharedPtr<FJsonObject>& Object, const FString& Field, const FRotator& DefaultValue)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values || Values->Num() < 3)
        {
            return DefaultValue;
        }
        return FRotator((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
    }

    bool ConfigureUserDefinedStruct(UUserDefinedStruct* Struct, const TSharedPtr<FJsonObject>& Request, FString& OutError)
    {
        if (!Struct)
        {
            OutError = TEXT("Struct is null");
            return false;
        }

        TArray<FStructVariableDescription>& Existing = FStructureEditorUtils::GetVarDesc(Struct);
        for (int32 Index = Existing.Num() - 1; Index >= 0; --Index)
        {
            FStructureEditorUtils::RemoveVariable(Struct, Existing[Index].VarGuid);
        }

        const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
        if (Request->TryGetArrayField(TEXT("fields"), Fields) && Fields)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Fields)
            {
                const TSharedPtr<FJsonObject> Field = Value->AsObject();
                if (!Field.IsValid()) continue;

                const FString Name = ABTJson::GetString(Field, TEXT("name"));
                if (Name.IsEmpty())
                {
                    OutError = TEXT("Struct field name missing");
                    return false;
                }

                if (!FStructureEditorUtils::AddVariable(Struct, MakeStructPinTypeFromString(ABTJson::GetString(Field, TEXT("type"), TEXT("float")))))
                {
                    OutError = FString::Printf(TEXT("Add struct field failed: %s"), *Name);
                    return false;
                }

                TArray<FStructVariableDescription>& Variables = FStructureEditorUtils::GetVarDesc(Struct);
                if (Variables.Num() == 0)
                {
                    OutError = TEXT("Struct field was not added");
                    return false;
                }

                const FGuid Guid = Variables.Last().VarGuid;
                FStructureEditorUtils::RenameVariable(Struct, Guid, Name);
                const FString Default = ABTJson::GetString(Field, TEXT("default"));
                if (!Default.IsEmpty())
                {
                    FStructureEditorUtils::ChangeVariableDefaultValue(Struct, Guid, Default);
                }
            }
        }

        FStructureEditorUtils::CompileStructure(Struct);
        Struct->MarkPackageDirty();
        return true;
    }
}

bool FABTAssetTools::CreateAsset(const TSharedPtr<FJsonObject>& Request, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    const FString AssetType = ABTJson::GetString(Request, TEXT("assetType"));
    const FString Path = ABTJson::GetString(Request, TEXT("path"));
    FString PackagePath, AssetName;
    if (!SplitAssetPath(Path, PackagePath, AssetName))
    {
        OutError = TEXT("Invalid asset path");
        return false;
    }

    UObject* Created = nullptr;
    IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

    if (AssetType.Equals(TEXT("Material"), ESearchCase::IgnoreCase))
    {
        UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
        Created = AssetTools.CreateAsset(AssetName, PackagePath, UMaterial::StaticClass(), Factory);
    }
    else if (AssetType.Equals(TEXT("MaterialInstanceConstant"), ESearchCase::IgnoreCase))
    {
        UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
        if (UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *ABTJson::GetString(Request, TEXT("parentMaterial"))))
        {
            Factory->InitialParent = Parent;
        }
        Created = AssetTools.CreateAsset(AssetName, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory);
    }
    else if (AssetType.Equals(TEXT("Blueprint"), ESearchCase::IgnoreCase) || AssetType.Equals(TEXT("BlueprintInterface"), ESearchCase::IgnoreCase))
    {
        UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
        UClass* ParentClass = AActor::StaticClass();
        if (AssetType.Equals(TEXT("BlueprintInterface"), ESearchCase::IgnoreCase))
        {
            Factory->BlueprintType = BPTYPE_Interface;
            ParentClass = UInterface::StaticClass();
        }
        const FString ParentClassPath = ABTJson::GetString(Request, TEXT("parentClass"));
        if (!ParentClassPath.IsEmpty())
        {
            if (UClass* Resolved = ResolveClass(ParentClassPath)) ParentClass = Resolved;
        }
        Factory->ParentClass = ParentClass;
        Created = AssetTools.CreateAsset(AssetName, PackagePath, UBlueprint::StaticClass(), Factory);
    }
    else if (AssetType.Equals(TEXT("WidgetBlueprint"), ESearchCase::IgnoreCase))
    {
        UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
        Factory->BlueprintType = BPTYPE_Normal;
        Factory->ParentClass = UUserWidget::StaticClass();
        const FString ParentClassPath = ABTJson::GetString(Request, TEXT("parentClass"));
        if (!ParentClassPath.IsEmpty())
        {
            if (UClass* Resolved = ResolveClass(ParentClassPath))
            {
                if (!Resolved->IsChildOf(UUserWidget::StaticClass()))
                {
                    OutError = FString::Printf(TEXT("WidgetBlueprint parentClass must derive from UserWidget: %s"), *ParentClassPath);
                    return false;
                }
                Factory->ParentClass = Resolved;
            }
        }
        Created = AssetTools.CreateAsset(AssetName, PackagePath, UWidgetBlueprint::StaticClass(), Factory);
    }
    else if (AssetType.Equals(TEXT("UserDefinedStruct"), ESearchCase::IgnoreCase))
    {
        UPackage* Package = CreatePackage(*Path);
        UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(Package, *AssetName, RF_Public | RF_Standalone);
        if (!Struct)
        {
            OutError = TEXT("CreateUserDefinedStruct failed");
            return false;
        }
        if (!ConfigureUserDefinedStruct(Struct, Request, OutError))
        {
            return false;
        }
        FAssetRegistryModule::AssetCreated(Struct);
        Created = Struct;
    }
    else
    {
        OutError = FString::Printf(TEXT("Unsupported assetType: %s"), *AssetType);
        return false;
    }

    if (!Created)
    {
        OutError = TEXT("CreateAsset failed");
        return false;
    }

    const bool bSave = ABTJson::GetBool(Request, TEXT("save"), false);
    if (bSave)
    {
        UEditorAssetLibrary::SaveLoadedAsset(Created, false);
    }

    OutJson = ABTJson::Ok();
    OutJson->SetStringField(TEXT("asset_path"), Created->GetPathName());
    OutJson->SetStringField(TEXT("class"), Created->GetClass()->GetPathName());
    OutJson->SetBoolField(TEXT("saved"), bSave);
    return true;
}

bool FABTAssetTools::ReadAsset(const TSharedPtr<FJsonObject>& Request, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    UObject* Object = LoadObject<UObject>(nullptr, *ABTJson::GetString(Request, TEXT("assetPath")));
    if (!Object)
    {
        OutError = TEXT("Could not load asset");
        return false;
    }

    if (ABTJson::GetString(Request, TEXT("object"), TEXT("Asset")) == TEXT("DefaultObject"))
    {
        if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
        {
            Object = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
        }
    }

    if (!Object)
    {
        OutError = TEXT("Could not resolve requested object");
        return false;
    }

    OutJson = ABTJson::Ok();
    OutJson->SetStringField(TEXT("asset_path"), ABTJson::GetString(Request, TEXT("assetPath")));
    OutJson->SetStringField(TEXT("object_path"), Object->GetPathName());
    OutJson->SetStringField(TEXT("name"), Object->GetName());
    OutJson->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
    OutJson->SetStringField(TEXT("package"), Object->GetOutermost() ? Object->GetOutermost()->GetName() : FString());
    OutJson->SetBoolField(TEXT("dirty"), Object->GetOutermost() ? Object->GetOutermost()->IsDirty() : false);

    if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
    {
        OutJson->SetStringField(TEXT("blueprint_type"), StaticEnum<EBlueprintType>()->GetNameStringByValue(Blueprint->BlueprintType));
        OutJson->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString());
        OutJson->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString());
    }

    if (UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(Object))
    {
        TArray<TSharedPtr<FJsonValue>> Fields;
        for (const FStructVariableDescription& Var : FStructureEditorUtils::GetVarDesc(Struct))
        {
            TSharedPtr<FJsonObject> Field = ABTJson::Object();
            Field->SetStringField(TEXT("name"), Var.FriendlyName);
            Field->SetStringField(TEXT("internal_name"), Var.VarName.ToString());
            Field->SetStringField(TEXT("default"), Var.DefaultValue);
            Field->SetStringField(TEXT("type"), Var.ToPinType().PinCategory.ToString());
            Fields.Add(ABTJson::ObjectValue(Field));
        }
        OutJson->SetArrayField(TEXT("fields"), Fields);
    }

    const TArray<TSharedPtr<FJsonValue>>* PropertyNames = nullptr;
    if (Request->TryGetArrayField(TEXT("properties"), PropertyNames) && PropertyNames)
    {
        TSharedPtr<FJsonObject> Properties = ABTJson::Object();
        for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertyNames)
        {
            const FString PropertyName = PropertyValue->AsString();
            FString PropertyError;
            TSharedPtr<FJsonValue> ExportedValue = ExportSimpleProperty(Object, PropertyName, PropertyError);
            if (ExportedValue.IsValid())
            {
                Properties->SetField(PropertyName, ExportedValue);
            }
            else
            {
                Properties->SetStringField(PropertyName, FString::Printf(TEXT("<error: %s>"), *PropertyError));
            }
        }
        OutJson->SetObjectField(TEXT("properties"), Properties);
    }

    return true;
}

bool FABTAssetTools::SetAssetProperty(const TSharedPtr<FJsonObject>& Request, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    UObject* Object = LoadObject<UObject>(nullptr, *ABTJson::GetString(Request, TEXT("assetPath")));
    if (!Object)
    {
        OutError = TEXT("Could not load asset");
        return false;
    }

    if (ABTJson::GetString(Request, TEXT("object"), TEXT("Asset")) == TEXT("DefaultObject"))
    {
        if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
        {
            Object = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
        }
    }

    UObject* SaveTarget = Object;
    if (Object && Object->HasAnyFlags(RF_ClassDefaultObject))
    {
        if (UClass* OwnerClass = Object->GetClass())
        {
            if (UBlueprint* Blueprint = Cast<UBlueprint>(OwnerClass->ClassGeneratedBy))
            {
                SaveTarget = Blueprint;
            }
        }
    }

    const FScopedTransaction Transaction(NSLOCTEXT("AgentBlueprintTools", "SetAssetProperty", "Set Asset Property"));
    if (Object)
    {
        Object->Modify();
    }

    if (!SetSimpleProperty(Object, ABTJson::GetString(Request, TEXT("propertyPath")), Request, OutError))
    {
        return false;
    }

    const bool bSave = ABTJson::GetBool(Request, TEXT("save"), false);
    if (bSave)
    {
        UEditorAssetLibrary::SaveLoadedAsset(SaveTarget, false);
    }

    OutJson = ABTJson::Ok();
    OutJson->SetBoolField(TEXT("saved"), bSave);
    return true;
}

bool FABTAssetTools::SaveAsset(const FString& AssetPath, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    UObject* Object = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Object)
    {
        OutError = TEXT("Could not load asset");
        return false;
    }

    const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Object, false);
    OutJson = ABTJson::Ok();
    OutJson->SetBoolField(TEXT("saved"), bSaved);
    return true;
}

bool FABTAssetTools::DeleteAsset(const FString& AssetPath, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    if (AssetPath.IsEmpty())
    {
        OutError = TEXT("assetPath missing");
        return false;
    }

    if (!UEditorAssetLibrary::DoesAssetExist(AssetPath))
    {
        OutJson = ABTJson::Ok();
        OutJson->SetBoolField(TEXT("deleted"), false);
        OutJson->SetStringField(TEXT("asset_path"), AssetPath);
        OutJson->SetStringField(TEXT("message"), TEXT("Asset did not exist"));
        return true;
    }

    const bool bDeleted = UEditorAssetLibrary::DeleteAsset(AssetPath);
    OutJson = ABTJson::Ok();
    OutJson->SetBoolField(TEXT("deleted"), bDeleted);
    OutJson->SetStringField(TEXT("asset_path"), AssetPath);
    return true;
}

bool FABTAssetTools::PlaceActor(const TSharedPtr<FJsonObject>& Request, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    const FString AssetPath = ABTJson::GetString(Request, TEXT("assetPath"));
    UObject* Object = LoadObject<UObject>(nullptr, *AssetPath);
    UClass* ActorClass = Cast<UClass>(Object);
    if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
    {
        ActorClass = Blueprint->GeneratedClass;
    }

    if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass()))
    {
        OutError = FString::Printf(TEXT("Asset is not an Actor class or Blueprint: %s"), *AssetPath);
        return false;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        OutError = TEXT("Editor world is not available");
        return false;
    }

    const FVector Location = ReadVectorField(Request, TEXT("location"), FVector::ZeroVector);
    const FRotator Rotation = ReadRotatorField(Request, TEXT("rotation"), FRotator::ZeroRotator);
    FActorSpawnParameters SpawnParameters;
    SpawnParameters.ObjectFlags = RF_Transactional;
    if (ABTJson::GetBool(Request, TEXT("transient"), false))
    {
        SpawnParameters.ObjectFlags |= RF_Transient;
    }
    AActor* Actor = World->SpawnActor<AActor>(ActorClass, Location, Rotation, SpawnParameters);
    if (!Actor)
    {
        OutError = TEXT("SpawnActor failed");
        return false;
    }

    Actor->Modify();
    const FString Label = ABTJson::GetString(Request, TEXT("label"));
    if (!Label.IsEmpty() && ABTJson::GetBool(Request, TEXT("replaceExistingLabel"), false))
    {
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* ExistingActor = *It;
            if (ExistingActor && ExistingActor->GetActorLabel() == Label)
            {
                ExistingActor->Destroy();
            }
        }
    }

    if (!Label.IsEmpty())
    {
        Actor->SetActorLabel(Label);
    }

    if (ABTJson::GetBool(Request, TEXT("saveLevel"), false))
    {
        FEditorFileUtils::SaveCurrentLevel();
    }

    OutJson = ABTJson::Ok();
    OutJson->SetStringField(TEXT("actor_name"), Actor->GetName());
    OutJson->SetStringField(TEXT("actor_path"), Actor->GetPathName());
    OutJson->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
    OutJson->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
    OutJson->SetStringField(TEXT("level"), Actor->GetLevel() ? Actor->GetLevel()->GetPathName() : FString());
    OutJson->SetBoolField(TEXT("transient"), Actor->HasAnyFlags(RF_Transient));
    return true;
}
