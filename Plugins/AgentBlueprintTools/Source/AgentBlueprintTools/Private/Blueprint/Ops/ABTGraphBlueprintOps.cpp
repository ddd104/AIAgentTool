#include "Blueprint/Ops/ABTGraphBlueprintOps.h"
#include "Blueprint/Utils/ABTBlueprintGraphUtils.h"
#include "Utils/ABTJson.h"

#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Curves/RichCurve.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/TimelineTemplate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Timeline.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UnrealType.h"

namespace ABT::Blueprint::Ops
{
    namespace
    {
        enum class ETimelineTrackKind
        {
            Float,
            Vector,
            Event,
            LinearColor,
            Unknown
        };

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

        ETimelineTrackKind ParseTimelineTrackKind(const FString& Type)
        {
            if (Type.Equals(TEXT("float"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("floatTrack"), ESearchCase::IgnoreCase))
            {
                return ETimelineTrackKind::Float;
            }
            if (Type.Equals(TEXT("vector"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("vectorTrack"), ESearchCase::IgnoreCase))
            {
                return ETimelineTrackKind::Vector;
            }
            if (Type.Equals(TEXT("event"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("eventTrack"), ESearchCase::IgnoreCase))
            {
                return ETimelineTrackKind::Event;
            }
            if (Type.Equals(TEXT("color"), ESearchCase::IgnoreCase) ||
                Type.Equals(TEXT("linearColor"), ESearchCase::IgnoreCase) ||
                Type.Equals(TEXT("colorTrack"), ESearchCase::IgnoreCase))
            {
                return ETimelineTrackKind::LinearColor;
            }
            return ETimelineTrackKind::Unknown;
        }

        UObject* TimelineCurveOuter(UBlueprint* Blueprint)
        {
            return Blueprint && Blueprint->GeneratedClass ? static_cast<UObject*>(Blueprint->GeneratedClass) : static_cast<UObject*>(Blueprint);
        }

        ERichCurveInterpMode ReadInterpMode(const TSharedPtr<FJsonObject>& KeyObject, ERichCurveInterpMode DefaultMode = RCIM_Cubic)
        {
            const FString Interp = ABTJson::GetString(KeyObject, TEXT("interp"), ABTJson::GetString(KeyObject, TEXT("interpolation")));
            if (Interp.Equals(TEXT("linear"), ESearchCase::IgnoreCase))
            {
                return RCIM_Linear;
            }
            if (Interp.Equals(TEXT("constant"), ESearchCase::IgnoreCase))
            {
                return RCIM_Constant;
            }
            if (Interp.Equals(TEXT("cubic"), ESearchCase::IgnoreCase) || Interp.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
            {
                return RCIM_Cubic;
            }
            return DefaultMode;
        }

        bool ReadKeyTime(const TSharedPtr<FJsonValue>& KeyValue, double& OutTime, TSharedPtr<FJsonObject>& OutKeyObject, FString& OutError)
        {
            if (!KeyValue.IsValid())
            {
                OutError = TEXT("Timeline key is invalid");
                return false;
            }

            if (KeyValue->Type == EJson::Number)
            {
                OutTime = KeyValue->AsNumber();
                OutKeyObject.Reset();
                return true;
            }

            if (KeyValue->Type == EJson::Object)
            {
                OutKeyObject = KeyValue->AsObject();
                if (!OutKeyObject.IsValid() || !OutKeyObject->TryGetNumberField(TEXT("time"), OutTime))
                {
                    OutError = TEXT("Timeline key object requires numeric time");
                    return false;
                }
                return true;
            }

            if (KeyValue->Type == EJson::Array)
            {
                const TArray<TSharedPtr<FJsonValue>> Values = KeyValue->AsArray();
                if (Values.Num() < 1)
                {
                    OutError = TEXT("Timeline key array requires time at index 0");
                    return false;
                }
                OutTime = Values[0]->AsNumber();
                OutKeyObject.Reset();
                return true;
            }

            OutError = TEXT("Timeline keys must be objects, arrays, or numbers for event keys");
            return false;
        }

        bool ReadFloatValue(const TSharedPtr<FJsonValue>& Value, float& OutValue)
        {
            if (!Value.IsValid())
            {
                return false;
            }
            OutValue = static_cast<float>(Value->AsNumber());
            return true;
        }

        bool ReadVectorValue(const TSharedPtr<FJsonValue>& Value, FVector& OutValue)
        {
            if (!Value.IsValid())
            {
                return false;
            }

            if (Value->Type == EJson::Array)
            {
                const TArray<TSharedPtr<FJsonValue>> Values = Value->AsArray();
                if (Values.Num() < 3)
                {
                    return false;
                }
                OutValue = FVector(Values[0]->AsNumber(), Values[1]->AsNumber(), Values[2]->AsNumber());
                return true;
            }

            if (Value->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject> Object = Value->AsObject();
                if (!Object.IsValid())
                {
                    return false;
                }
                OutValue = FVector(
                    ABTJson::GetNumber(Object, TEXT("x"), ABTJson::GetNumber(Object, TEXT("X"), 0.0)),
                    ABTJson::GetNumber(Object, TEXT("y"), ABTJson::GetNumber(Object, TEXT("Y"), 0.0)),
                    ABTJson::GetNumber(Object, TEXT("z"), ABTJson::GetNumber(Object, TEXT("Z"), 0.0)));
                return true;
            }
            return false;
        }

        bool ReadColorValue(const TSharedPtr<FJsonValue>& Value, FLinearColor& OutValue)
        {
            if (!Value.IsValid())
            {
                return false;
            }

            if (Value->Type == EJson::String)
            {
                const FString Hex = Value->AsString();
                if (!Hex.IsEmpty())
                {
                    OutValue = FLinearColor::FromSRGBColor(FColor::FromHex(Hex));
                    return true;
                }
            }

            if (Value->Type == EJson::Array)
            {
                const TArray<TSharedPtr<FJsonValue>> Values = Value->AsArray();
                if (Values.Num() < 3)
                {
                    return false;
                }
                OutValue = FLinearColor(
                    Values[0]->AsNumber(),
                    Values[1]->AsNumber(),
                    Values[2]->AsNumber(),
                    Values.Num() > 3 ? Values[3]->AsNumber() : 1.0);
                return true;
            }

            if (Value->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject> Object = Value->AsObject();
                if (!Object.IsValid())
                {
                    return false;
                }
                OutValue = FLinearColor(
                    ABTJson::GetNumber(Object, TEXT("r"), ABTJson::GetNumber(Object, TEXT("R"), 0.0)),
                    ABTJson::GetNumber(Object, TEXT("g"), ABTJson::GetNumber(Object, TEXT("G"), 0.0)),
                    ABTJson::GetNumber(Object, TEXT("b"), ABTJson::GetNumber(Object, TEXT("B"), 0.0)),
                    ABTJson::GetNumber(Object, TEXT("a"), ABTJson::GetNumber(Object, TEXT("A"), 1.0)));
                return true;
            }
            return false;
        }

        bool TrackNameExistsInOtherKind(const UTimelineTemplate* Timeline, FName TrackName, ETimelineTrackKind Kind)
        {
            if (!Timeline || TrackName.IsNone())
            {
                return false;
            }

            if (Kind != ETimelineTrackKind::Float)
            {
                for (const FTTFloatTrack& Track : Timeline->FloatTracks)
                {
                    if (Track.GetTrackName() == TrackName) return true;
                }
            }
            if (Kind != ETimelineTrackKind::Vector)
            {
                for (const FTTVectorTrack& Track : Timeline->VectorTracks)
                {
                    if (Track.GetTrackName() == TrackName) return true;
                }
            }
            if (Kind != ETimelineTrackKind::Event)
            {
                for (const FTTEventTrack& Track : Timeline->EventTracks)
                {
                    if (Track.GetTrackName() == TrackName) return true;
                }
            }
            if (Kind != ETimelineTrackKind::LinearColor)
            {
                for (const FTTLinearColorTrack& Track : Timeline->LinearColorTracks)
                {
                    if (Track.GetTrackName() == TrackName) return true;
                }
            }
            return false;
        }

        void ApplyFloatCurveKeys(UCurveFloat* Curve, const TArray<TSharedPtr<FJsonValue>>& Keys, FString& OutError)
        {
            if (!Curve)
            {
                return;
            }

            Curve->Modify();
            Curve->FloatCurve.Reset();
            for (const TSharedPtr<FJsonValue>& KeyValue : Keys)
            {
                double Time = 0.0;
                TSharedPtr<FJsonObject> KeyObject;
                if (!ReadKeyTime(KeyValue, Time, KeyObject, OutError))
                {
                    return;
                }

                float Value = 0.0f;
                const TSharedPtr<FJsonValue> ValueField = KeyObject.IsValid() ? KeyObject->TryGetField(TEXT("value")) : nullptr;
                if (!ReadFloatValue(ValueField, Value))
                {
                    const TArray<TSharedPtr<FJsonValue>> Values = KeyValue->Type == EJson::Array ? KeyValue->AsArray() : TArray<TSharedPtr<FJsonValue>>();
                    if (Values.Num() >= 2)
                    {
                        Value = static_cast<float>(Values[1]->AsNumber());
                    }
                }
                const FKeyHandle Handle = Curve->FloatCurve.AddKey(static_cast<float>(Time), Value);
                Curve->FloatCurve.SetKeyInterpMode(Handle, ReadInterpMode(KeyObject));
            }
        }

        void ApplyEventCurveKeys(UCurveFloat* Curve, const TArray<TSharedPtr<FJsonValue>>& Keys, FString& OutError)
        {
            if (!Curve)
            {
                return;
            }

            Curve->Modify();
            Curve->bIsEventCurve = true;
            Curve->FloatCurve.Reset();
            for (const TSharedPtr<FJsonValue>& KeyValue : Keys)
            {
                double Time = 0.0;
                TSharedPtr<FJsonObject> KeyObject;
                if (!ReadKeyTime(KeyValue, Time, KeyObject, OutError))
                {
                    return;
                }

                const FKeyHandle Handle = Curve->FloatCurve.AddKey(static_cast<float>(Time), 0.0f);
                Curve->FloatCurve.SetKeyInterpMode(Handle, RCIM_Constant);
            }
        }

        void ApplyVectorCurveKeys(UCurveVector* Curve, const TArray<TSharedPtr<FJsonValue>>& Keys, FString& OutError)
        {
            if (!Curve)
            {
                return;
            }

            Curve->Modify();
            for (FRichCurve& ComponentCurve : Curve->FloatCurves)
            {
                ComponentCurve.Reset();
            }

            for (const TSharedPtr<FJsonValue>& KeyValue : Keys)
            {
                double Time = 0.0;
                TSharedPtr<FJsonObject> KeyObject;
                if (!ReadKeyTime(KeyValue, Time, KeyObject, OutError))
                {
                    return;
                }

                FVector Value = FVector::ZeroVector;
                const TSharedPtr<FJsonValue> ValueField = KeyObject.IsValid() ? KeyObject->TryGetField(TEXT("value")) : nullptr;
                if (!ReadVectorValue(ValueField, Value))
                {
                    const TArray<TSharedPtr<FJsonValue>> Values = KeyValue->Type == EJson::Array ? KeyValue->AsArray() : TArray<TSharedPtr<FJsonValue>>();
                    if (Values.Num() >= 4)
                    {
                        Value = FVector(Values[1]->AsNumber(), Values[2]->AsNumber(), Values[3]->AsNumber());
                    }
                }

                const ERichCurveInterpMode Interp = ReadInterpMode(KeyObject);
                const float Components[3] = { static_cast<float>(Value.X), static_cast<float>(Value.Y), static_cast<float>(Value.Z) };
                for (int32 Index = 0; Index < 3; ++Index)
                {
                    const FKeyHandle Handle = Curve->FloatCurves[Index].AddKey(static_cast<float>(Time), Components[Index]);
                    Curve->FloatCurves[Index].SetKeyInterpMode(Handle, Interp);
                }
            }
        }

        void ApplyColorCurveKeys(UCurveLinearColor* Curve, const TArray<TSharedPtr<FJsonValue>>& Keys, FString& OutError)
        {
            if (!Curve)
            {
                return;
            }

            Curve->Modify();
            for (FRichCurve& ComponentCurve : Curve->FloatCurves)
            {
                ComponentCurve.Reset();
            }

            for (const TSharedPtr<FJsonValue>& KeyValue : Keys)
            {
                double Time = 0.0;
                TSharedPtr<FJsonObject> KeyObject;
                if (!ReadKeyTime(KeyValue, Time, KeyObject, OutError))
                {
                    return;
                }

                FLinearColor Value = FLinearColor::White;
                const TSharedPtr<FJsonValue> ValueField = KeyObject.IsValid() ? KeyObject->TryGetField(TEXT("value")) : nullptr;
                if (!ReadColorValue(ValueField, Value))
                {
                    const TArray<TSharedPtr<FJsonValue>> Values = KeyValue->Type == EJson::Array ? KeyValue->AsArray() : TArray<TSharedPtr<FJsonValue>>();
                    if (Values.Num() >= 4)
                    {
                        Value = FLinearColor(
                            Values[1]->AsNumber(),
                            Values[2]->AsNumber(),
                            Values[3]->AsNumber(),
                            Values.Num() > 4 ? Values[4]->AsNumber() : 1.0);
                    }
                }

                const ERichCurveInterpMode Interp = ReadInterpMode(KeyObject);
                const float Components[4] = { Value.R, Value.G, Value.B, Value.A };
                for (int32 Index = 0; Index < 4; ++Index)
                {
                    const FKeyHandle Handle = Curve->FloatCurves[Index].AddKey(static_cast<float>(Time), Components[Index]);
                    Curve->FloatCurves[Index].SetKeyInterpMode(Handle, Interp);
                }
            }
        }

        template <typename TrackType>
        int32 FindTrackIndexByName(const TArray<TrackType>& Tracks, FName TrackName)
        {
            for (int32 Index = 0; Index < Tracks.Num(); ++Index)
            {
                if (Tracks[Index].GetTrackName() == TrackName)
                {
                    return Index;
                }
            }
            return INDEX_NONE;
        }

        bool ConfigureTimelineTrack(UBlueprint* Blueprint, UTimelineTemplate* Timeline, const TSharedPtr<FJsonObject>& TrackObject, TArray<FString>& OutMessages, FString& OutError)
        {
            if (!Blueprint || !Timeline || !TrackObject.IsValid())
            {
                OutError = TEXT("configure_timeline track requires a valid Blueprint, Timeline, and track object");
                return false;
            }

            const FString TypeString = ABTJson::GetString(TrackObject, TEXT("type"));
            const ETimelineTrackKind Kind = ParseTimelineTrackKind(TypeString);
            if (Kind == ETimelineTrackKind::Unknown)
            {
                OutError = FString::Printf(TEXT("Unsupported timeline track type: %s"), *TypeString);
                return false;
            }

            const FName TrackName(*ABTJson::GetString(TrackObject, TEXT("name")));
            if (TrackName.IsNone())
            {
                OutError = TEXT("configure_timeline track.name is required");
                return false;
            }
            if (TrackNameExistsInOtherKind(Timeline, TrackName, Kind))
            {
                OutError = FString::Printf(TEXT("Timeline track name is already used by another track type: %s"), *TrackName.ToString());
                return false;
            }

            const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
            TrackObject->TryGetArrayField(TEXT("keys"), Keys);
            const bool bUseInlineKeys = Keys != nullptr;
            const FString CurvePath = ABTJson::GetString(TrackObject, TEXT("curve"), ABTJson::GetString(TrackObject, TEXT("curvePath")));
            UObject* Owner = TimelineCurveOuter(Blueprint);

            if (Kind == ETimelineTrackKind::Float)
            {
                int32 TrackIndex = FindTrackIndexByName(Timeline->FloatTracks, TrackName);
                const bool bIsNewTrack = TrackIndex == INDEX_NONE;
                if (bIsNewTrack)
                {
                    TrackIndex = Timeline->FloatTracks.Num();
                    FTTFloatTrack NewTrack;
                    NewTrack.SetTrackName(TrackName, Timeline);
                    Timeline->FloatTracks.Add(NewTrack);
                    Timeline->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_FloatInterp, TrackIndex));
                }

                FTTFloatTrack& Track = Timeline->FloatTracks[TrackIndex];
                if (!CurvePath.IsEmpty())
                {
                    UCurveFloat* Curve = LoadObject<UCurveFloat>(nullptr, *CurvePath);
                    if (!Curve)
                    {
                        OutError = FString::Printf(TEXT("Could not load float curve: %s"), *CurvePath);
                        return false;
                    }
                    Track.CurveFloat = Curve;
                    Track.bIsExternalCurve = true;
                }
                else if (!Track.CurveFloat)
                {
                    Track.CurveFloat = NewObject<UCurveFloat>(Owner, NAME_None, RF_Public | RF_Transactional);
                    Track.bIsExternalCurve = false;
                }
                else if (bUseInlineKeys && Track.bIsExternalCurve)
                {
                    Track.CurveFloat = NewObject<UCurveFloat>(Owner, NAME_None, RF_Public | RF_Transactional);
                    Track.bIsExternalCurve = false;
                }

                if (Keys && !Track.bIsExternalCurve)
                {
                    ApplyFloatCurveKeys(Track.CurveFloat, *Keys, OutError);
                    if (!OutError.IsEmpty()) return false;
                }
                OutMessages.Add(FString::Printf(TEXT("%s float timeline track %s"), bIsNewTrack ? TEXT("Added") : TEXT("Updated"), *TrackName.ToString()));
                return true;
            }

            if (Kind == ETimelineTrackKind::Vector)
            {
                int32 TrackIndex = FindTrackIndexByName(Timeline->VectorTracks, TrackName);
                const bool bIsNewTrack = TrackIndex == INDEX_NONE;
                if (bIsNewTrack)
                {
                    TrackIndex = Timeline->VectorTracks.Num();
                    FTTVectorTrack NewTrack;
                    NewTrack.SetTrackName(TrackName, Timeline);
                    Timeline->VectorTracks.Add(NewTrack);
                    Timeline->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_VectorInterp, TrackIndex));
                }

                FTTVectorTrack& Track = Timeline->VectorTracks[TrackIndex];
                if (!CurvePath.IsEmpty())
                {
                    UCurveVector* Curve = LoadObject<UCurveVector>(nullptr, *CurvePath);
                    if (!Curve)
                    {
                        OutError = FString::Printf(TEXT("Could not load vector curve: %s"), *CurvePath);
                        return false;
                    }
                    Track.CurveVector = Curve;
                    Track.bIsExternalCurve = true;
                }
                else if (!Track.CurveVector)
                {
                    Track.CurveVector = NewObject<UCurveVector>(Owner, NAME_None, RF_Public | RF_Transactional);
                    Track.bIsExternalCurve = false;
                }
                else if (bUseInlineKeys && Track.bIsExternalCurve)
                {
                    Track.CurveVector = NewObject<UCurveVector>(Owner, NAME_None, RF_Public | RF_Transactional);
                    Track.bIsExternalCurve = false;
                }

                if (Keys && !Track.bIsExternalCurve)
                {
                    ApplyVectorCurveKeys(Track.CurveVector, *Keys, OutError);
                    if (!OutError.IsEmpty()) return false;
                }
                OutMessages.Add(FString::Printf(TEXT("%s vector timeline track %s"), bIsNewTrack ? TEXT("Added") : TEXT("Updated"), *TrackName.ToString()));
                return true;
            }

            if (Kind == ETimelineTrackKind::Event)
            {
                int32 TrackIndex = FindTrackIndexByName(Timeline->EventTracks, TrackName);
                const bool bIsNewTrack = TrackIndex == INDEX_NONE;
                if (bIsNewTrack)
                {
                    TrackIndex = Timeline->EventTracks.Num();
                    FTTEventTrack NewTrack;
                    NewTrack.SetTrackName(TrackName, Timeline);
                    Timeline->EventTracks.Add(NewTrack);
                    Timeline->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_Event, TrackIndex));
                }

                FTTEventTrack& Track = Timeline->EventTracks[TrackIndex];
                if (!CurvePath.IsEmpty())
                {
                    UCurveFloat* Curve = LoadObject<UCurveFloat>(nullptr, *CurvePath);
                    if (!Curve)
                    {
                        OutError = FString::Printf(TEXT("Could not load event curve: %s"), *CurvePath);
                        return false;
                    }
                    Track.CurveKeys = Curve;
                    Track.CurveKeys->bIsEventCurve = true;
                    Track.bIsExternalCurve = true;
                }
                else if (!Track.CurveKeys)
                {
                    Track.CurveKeys = NewObject<UCurveFloat>(Owner, NAME_None, RF_Public | RF_Transactional);
                    Track.CurveKeys->bIsEventCurve = true;
                    Track.bIsExternalCurve = false;
                }
                else if (bUseInlineKeys && Track.bIsExternalCurve)
                {
                    Track.CurveKeys = NewObject<UCurveFloat>(Owner, NAME_None, RF_Public | RF_Transactional);
                    Track.CurveKeys->bIsEventCurve = true;
                    Track.bIsExternalCurve = false;
                }

                if (Keys && !Track.bIsExternalCurve)
                {
                    ApplyEventCurveKeys(Track.CurveKeys, *Keys, OutError);
                    if (!OutError.IsEmpty()) return false;
                }
                OutMessages.Add(FString::Printf(TEXT("%s event timeline track %s"), bIsNewTrack ? TEXT("Added") : TEXT("Updated"), *TrackName.ToString()));
                return true;
            }

            if (Kind == ETimelineTrackKind::LinearColor)
            {
                int32 TrackIndex = FindTrackIndexByName(Timeline->LinearColorTracks, TrackName);
                const bool bIsNewTrack = TrackIndex == INDEX_NONE;
                if (bIsNewTrack)
                {
                    TrackIndex = Timeline->LinearColorTracks.Num();
                    FTTLinearColorTrack NewTrack;
                    NewTrack.SetTrackName(TrackName, Timeline);
                    Timeline->LinearColorTracks.Add(NewTrack);
                    Timeline->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_LinearColorInterp, TrackIndex));
                }

                FTTLinearColorTrack& Track = Timeline->LinearColorTracks[TrackIndex];
                if (!CurvePath.IsEmpty())
                {
                    UCurveLinearColor* Curve = LoadObject<UCurveLinearColor>(nullptr, *CurvePath);
                    if (!Curve)
                    {
                        OutError = FString::Printf(TEXT("Could not load color curve: %s"), *CurvePath);
                        return false;
                    }
                    Track.CurveLinearColor = Curve;
                    Track.bIsExternalCurve = true;
                }
                else if (!Track.CurveLinearColor)
                {
                    Track.CurveLinearColor = NewObject<UCurveLinearColor>(Owner, NAME_None, RF_Public | RF_Transactional);
                    Track.bIsExternalCurve = false;
                }
                else if (bUseInlineKeys && Track.bIsExternalCurve)
                {
                    Track.CurveLinearColor = NewObject<UCurveLinearColor>(Owner, NAME_None, RF_Public | RF_Transactional);
                    Track.bIsExternalCurve = false;
                }

                if (Keys && !Track.bIsExternalCurve)
                {
                    ApplyColorCurveKeys(Track.CurveLinearColor, *Keys, OutError);
                    if (!OutError.IsEmpty()) return false;
                }
                OutMessages.Add(FString::Printf(TEXT("%s color timeline track %s"), bIsNewTrack ? TEXT("Added") : TEXT("Updated"), *TrackName.ToString()));
                return true;
            }

            OutError = TEXT("Unhandled timeline track type");
            return false;
        }

        void AddTypedTrackObjects(const TSharedPtr<FJsonObject>& Op, const FString& Field, const FString& Type, TArray<TSharedPtr<FJsonObject>>& OutTracks)
        {
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (!Op.IsValid() || !Op->TryGetArrayField(Field, Values) || !Values)
            {
                return;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Values)
            {
                TSharedPtr<FJsonObject> Track = Value.IsValid() ? Value->AsObject() : nullptr;
                if (Track.IsValid())
                {
                    Track->SetStringField(TEXT("type"), Type);
                    OutTracks.Add(Track);
                }
            }
        }

        void ResolveLayoutOverlaps(TArray<UEdGraphNode*>& Nodes, int32 MinXSpacing, int32 MinYSpacing)
        {
            Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
            {
                return A.NodePosY == B.NodePosY ? A.NodePosX < B.NodePosX : A.NodePosY < B.NodePosY;
            });

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

    bool LayoutGraphNodes(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, TArray<FString>& OutMessages, FString& OutError)
    {
        const FString GraphName = ABTJson::GetString(Op, TEXT("graph"), TEXT("EventGraph"));
        UEdGraph* Graph = FindGraph(Blueprint, GraphName);
        if (!Graph)
        {
            OutError = FString::Printf(TEXT("Graph not found: %s"), *GraphName);
            return false;
        }

        TArray<UEdGraphNode*> LayoutNodes;
        const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
        if (Op->TryGetArrayField(TEXT("nodes"), NodesArray) && NodesArray)
        {
            for (const TSharedPtr<FJsonValue>& Value : *NodesArray)
            {
                const TSharedPtr<FJsonObject> NodeObject = Value.IsValid() ? Value->AsObject() : nullptr;
                if (!NodeObject.IsValid())
                {
                    OutError = TEXT("layout_blueprint_graph.nodes entries must be objects");
                    return false;
                }

                const FString NodeRef = ABTJson::GetString(NodeObject, TEXT("node"), ABTJson::GetString(NodeObject, TEXT("target")));
                UEdGraphNode* Node = ResolvePatchNode(Blueprint, Op, NodeMap, NodeRef);
                if (!Node)
                {
                    OutError = FString::Printf(TEXT("layout_blueprint_graph referenced unknown node: %s"), *NodeRef);
                    return false;
                }

                Node->Modify();
                Node->NodePosX = ABTJson::GetInt(NodeObject, TEXT("x"), Node->NodePosX);
                Node->NodePosY = ABTJson::GetInt(NodeObject, TEXT("y"), Node->NodePosY);
                LayoutNodes.AddUnique(Node);
            }
        }
        else
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node)
                {
                    LayoutNodes.Add(Node);
                }
            }
        }

        if (ABTJson::GetBool(Op, TEXT("avoidOverlap"), true))
        {
            const int32 MinXSpacing = FMath::Max(1, ABTJson::GetInt(Op, TEXT("minXSpacing"), 220));
            const int32 MinYSpacing = FMath::Max(1, ABTJson::GetInt(Op, TEXT("minYSpacing"), 120));
            ResolveLayoutOverlaps(LayoutNodes, MinXSpacing, MinYSpacing);
        }

        OutMessages.Add(FString::Printf(TEXT("Laid out %d node(s) in %s"), LayoutNodes.Num(), *GraphName));
        return true;
    }

    bool ConfigureTimeline(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TMap<FString, UEdGraphNode*>& NodeMap, TArray<FString>& OutMessages, FString& OutError)
    {
        if (!Blueprint)
        {
            OutError = TEXT("configure_timeline called with null Blueprint");
            return false;
        }

        const FString TimelineNameString = ABTJson::GetString(Op, TEXT("name"), ABTJson::GetString(Op, TEXT("timeline")));
        if (TimelineNameString.IsEmpty())
        {
            OutError = TEXT("configure_timeline.name is required");
            return false;
        }

        const FName TimelineName(*TimelineNameString);
        UTimelineTemplate* Timeline = Blueprint->FindTimelineTemplateByVariableName(TimelineName);
        if (!Timeline)
        {
            Timeline = FBlueprintEditorUtils::AddNewTimeline(Blueprint, TimelineName);
            if (!Timeline)
            {
                OutError = FString::Printf(TEXT("Failed to create timeline: %s"), *TimelineNameString);
                return false;
            }
            OutMessages.Add(FString::Printf(TEXT("Added timeline %s"), *TimelineNameString));
        }

        Timeline->Modify();
        Timeline->TimelineLength = static_cast<float>(ABTJson::GetNumber(Op, TEXT("length"), ABTJson::GetNumber(Op, TEXT("duration"), Timeline->TimelineLength > 0.0f ? Timeline->TimelineLength : 1.0f)));
        const FString LengthMode = ABTJson::GetString(Op, TEXT("lengthMode"), TEXT("TimelineLength"));
        Timeline->LengthMode = LengthMode.Equals(TEXT("LastKeyFrame"), ESearchCase::IgnoreCase) ? TL_LastKeyFrame : TL_TimelineLength;
        Timeline->bAutoPlay = ABTJson::GetBool(Op, TEXT("autoplay"), ABTJson::GetBool(Op, TEXT("autoPlay"), Timeline->bAutoPlay != 0));
        Timeline->bLoop = ABTJson::GetBool(Op, TEXT("loop"), Timeline->bLoop != 0);
        Timeline->bReplicated = ABTJson::GetBool(Op, TEXT("replicated"), Timeline->bReplicated != 0);
        Timeline->bIgnoreTimeDilation = ABTJson::GetBool(Op, TEXT("ignoreTimeDilation"), Timeline->bIgnoreTimeDilation != 0);

        TArray<TSharedPtr<FJsonObject>> TrackObjects;
        const TArray<TSharedPtr<FJsonValue>>* GenericTracks = nullptr;
        if (Op->TryGetArrayField(TEXT("tracks"), GenericTracks) && GenericTracks)
        {
            for (const TSharedPtr<FJsonValue>& Value : *GenericTracks)
            {
                TSharedPtr<FJsonObject> Track = Value.IsValid() ? Value->AsObject() : nullptr;
                if (!Track.IsValid())
                {
                    OutError = TEXT("configure_timeline.tracks entries must be objects");
                    return false;
                }
                TrackObjects.Add(Track);
            }
        }
        AddTypedTrackObjects(Op, TEXT("floatTracks"), TEXT("float"), TrackObjects);
        AddTypedTrackObjects(Op, TEXT("vectorTracks"), TEXT("vector"), TrackObjects);
        AddTypedTrackObjects(Op, TEXT("eventTracks"), TEXT("event"), TrackObjects);
        AddTypedTrackObjects(Op, TEXT("colorTracks"), TEXT("color"), TrackObjects);
        AddTypedTrackObjects(Op, TEXT("linearColorTracks"), TEXT("linearColor"), TrackObjects);

        for (const TSharedPtr<FJsonObject>& Track : TrackObjects)
        {
            if (!ConfigureTimelineTrack(Blueprint, Timeline, Track, OutMessages, OutError))
            {
                return false;
            }
        }

        const FString GraphName = ABTJson::GetString(Op, TEXT("graph"), TEXT("EventGraph"));
        UEdGraph* Graph = FindGraph(Blueprint, GraphName);
        if (!Graph)
        {
            OutError = FString::Printf(TEXT("Graph not found: %s"), *GraphName);
            return false;
        }

        UK2Node_Timeline* TimelineNode = FBlueprintEditorUtils::FindNodeForTimeline(Blueprint, Timeline);
        if (TimelineNode)
        {
            TimelineNode->Modify();
            TimelineNode->ReconstructNode();
        }
        else
        {
            TimelineNode = NewObject<UK2Node_Timeline>(Graph);
            Graph->AddNode(TimelineNode, true, false);
            TimelineNode->CreateNewGuid();
            TimelineNode->TimelineName = TimelineName;
            TimelineNode->TimelineGuid = Timeline->TimelineGuid;
            TimelineNode->AllocateDefaultPins();
        }

        TimelineNode->NodePosX = ABTJson::GetInt(Op, TEXT("x"), TimelineNode->NodePosX);
        TimelineNode->NodePosY = ABTJson::GetInt(Op, TEXT("y"), TimelineNode->NodePosY);

        const FString Id = ABTJson::GetString(Op, TEXT("id"));
        if (!Id.IsEmpty())
        {
            NodeMap.Add(Id, TimelineNode);
        }
        NodeMap.Add(TimelineNameString, TimelineNode);

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        OutMessages.Add(FString::Printf(TEXT("Configured timeline %s with %d track spec(s)"), *TimelineNameString, TrackObjects.Num()));
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
