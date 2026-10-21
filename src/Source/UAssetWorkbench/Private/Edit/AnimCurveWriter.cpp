#include "Edit/AnimAssetWriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequenceBase.h"
#include "Curves/RichCurve.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
    // Undo is not on the table in a commandlet, and a transaction without a buffer only costs time.
    constexpr bool kShouldTransact = false;

    FAnimationCurveIdentifier MakeFloatCurveId(const FString& Name)
    {
        return FAnimationCurveIdentifier(FName(*Name), ERawCurveTrackTypes::RCT_Float);
    }

    // Labels AnimAssetExport prints. Material and Morph only survive on assets saved before those two
    // moved onto the skeleton, and are accepted so an exported block pastes back unedited.
    bool ReadCurveFlag(const FString& Label, int32& OutFlags)
    {
        if (Label == TEXT("Editable"))
        {
            OutFlags |= AACF_Editable;
            return true;
        }

        if (Label == TEXT("Metadata"))
        {
            OutFlags |= AACF_Metadata;
            return true;
        }

        if (Label == TEXT("DriveTrack"))
        {
            OutFlags |= AACF_DriveTrack;
            return true;
        }

        if (Label == TEXT("Disabled"))
        {
            OutFlags |= AACF_Disabled;
            return true;
        }

        if (Label == TEXT("Material"))
        {
            OutFlags |= AACF_DriveMaterial_DEPRECATED;
            return true;
        }

        if (Label == TEXT("Morph"))
        {
            OutFlags |= AACF_DriveMorphTarget_DEPRECATED;
            return true;
        }

        return false;
    }

    // An unresolvable curve name is the most common spec error, so failures print what was there.
    FString DescribeCurves(const IAnimationDataModel* DataModel)
    {
        TArray<FString> Lines;
        for (const FFloatCurve& Curve : DataModel->GetFloatCurves())
        {
            Lines.Add(FString::Printf(TEXT("%s (%d key(s))"), *Curve.GetName().ToString(), Curve.FloatCurve.GetNumKeys()));
        }

        return FString::Join(Lines, TEXT(", "));
    }

    class FAnimCurveWriter : public IAnimAssetWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Curves");
        }

        virtual bool Apply(FAnimAssetEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
            if (!Section->TryGetArray(Operations))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Curves must be an array of ops"), *Context.AssetPath);
                return false;
            }

            if (!Context.AnimAsset->IsDataModelValid())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s carries no animation data model, curves cannot be written"), *Context.AssetPath);
                return false;
            }

            // One bracket per target, so the model broadcasts a single rebuild instead of one per op.
            IAnimationDataController& Controller = Context.AnimAsset->GetController();
            IAnimationDataController::FScopedBracket Bracket(Controller, FText::FromString(TEXT("UAssetWorkbench Curves")), kShouldTransact);

            for (const TSharedPtr<FJsonValue>& Value : *Operations)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                if (!Desc.IsValid())
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Curves carries an entry that is not an object"), *Context.AssetPath);
                    return false;
                }

                FString Op;
                if (!Desc->TryGetStringField(TEXT("Op"), Op))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: curve entry has no Op"), *Context.AssetPath);
                    return false;
                }

                if (!ApplyOp(Context, Controller, Desc, Op))
                {
                    return false;
                }

                ++Context.Ops;
            }

            return true;
        }

    private:
        bool ApplyOp(FAnimAssetEditContext& Context, IAnimationDataController& Controller, const TSharedPtr<FJsonObject>& Desc, const FString& Op)
        {
            if (Op == TEXT("Add"))
            {
                return ApplyAdd(Context, Controller, Desc);
            }

            if (Op == TEXT("Remove"))
            {
                return ApplyRemove(Context, Controller, Desc);
            }

            if (Op == TEXT("Rename"))
            {
                return ApplyRename(Context, Controller, Desc);
            }

            if (Op == TEXT("SetKeys"))
            {
                return ApplySetKeys(Context, Controller, Desc);
            }

            if (Op == TEXT("Modify"))
            {
                return ApplyModify(Context, Controller, Desc);
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown curve op '%s'. Expected Add, Remove, Rename, SetKeys or Modify"), *Context.AssetPath, *Op);
            return false;
        }

        bool ReadName(const FAnimAssetEditContext& Context, const TSharedPtr<FJsonObject>& Desc, const TCHAR* Field, FString& OutName) const
        {
            if (!Desc->TryGetStringField(Field, OutName) || OutName.IsEmpty())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: curve op needs %s"), *Context.AssetPath, Field);
                return false;
            }

            return true;
        }

        bool RequireCurve(const FAnimAssetEditContext& Context, const FString& Name) const
        {
            if (Context.AnimAsset->GetDataModel()->FindFloatCurve(MakeFloatCurveId(Name)))
            {
                return true;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no float curve named '%s'. Present: %s"), *Context.AssetPath, *Name, *DescribeCurves(Context.AnimAsset->GetDataModel()));
            return false;
        }

        bool RefuseExistingCurve(const FAnimAssetEditContext& Context, const FString& Name) const
        {
            if (!Context.AnimAsset->GetDataModel()->FindFloatCurve(MakeFloatCurveId(Name)))
            {
                return true;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s already carries a float curve named '%s'"), *Context.AssetPath, *Name);
            return false;
        }

        bool ReadFlags(const FAnimAssetEditContext& Context, const TSharedPtr<FJsonObject>& Desc, int32& OutFlags, bool& bOutPresent) const
        {
            const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
            if (!Desc->TryGetArrayField(TEXT("Flags"), Items))
            {
                bOutPresent = false;
                return true;
            }

            bOutPresent = true;
            OutFlags = 0;
            for (const TSharedPtr<FJsonValue>& Item : *Items)
            {
                FString Label;
                if (!Item->TryGetString(Label) || !ReadCurveFlag(Label, OutFlags))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown curve flag. Expected Editable, Metadata, DriveTrack, Disabled, Material or Morph"), *Context.AssetPath);
                    return false;
                }
            }

            return true;
        }

        bool ReadKeys(const FAnimAssetEditContext& Context, const TSharedPtr<FJsonObject>& Desc, TArray<FRichCurveKey>& OutKeys, bool& bOutPresent) const
        {
            const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
            if (!Desc->TryGetArrayField(TEXT("Keys"), Items))
            {
                bOutPresent = false;
                return true;
            }

            bOutPresent = true;
            for (const TSharedPtr<FJsonValue>& Item : *Items)
            {
                const TSharedPtr<FJsonObject>& KeyDesc = Item->AsObject();
                if (!KeyDesc.IsValid())
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Keys carries an entry that is not an object"), *Context.AssetPath);
                    return false;
                }

                double Time = 0.0;
                double CurveValue = 0.0;
                if (!KeyDesc->TryGetNumberField(TEXT("Time"), Time) || !KeyDesc->TryGetNumberField(TEXT("Value"), CurveValue))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: a curve key needs Time and Value"), *Context.AssetPath);
                    return false;
                }

                FRichCurveKey Key(static_cast<float>(Time), static_cast<float>(CurveValue));

                FString InterpText;
                if (KeyDesc->TryGetStringField(TEXT("InterpMode"), InterpText))
                {
                    ERichCurveInterpMode InterpMode = RCIM_Linear;
                    if (!UAssetWorkbench::ReadRichCurveInterpMode(InterpText, InterpMode))
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown InterpMode '%s'. Expected Linear, Constant, Cubic or None"), *Context.AssetPath, *InterpText);
                        return false;
                    }

                    Key.InterpMode = InterpMode;
                }

                OutKeys.Add(Key);
            }

            return true;
        }

        bool ApplyAdd(FAnimAssetEditContext& Context, IAnimationDataController& Controller, const TSharedPtr<FJsonObject>& Desc)
        {
            FString Name;
            if (!ReadName(Context, Desc, TEXT("Name"), Name) || !RefuseExistingCurve(Context, Name))
            {
                return false;
            }

            int32 Flags = AACF_DefaultCurve;
            bool bHasFlags = false;
            if (!ReadFlags(Context, Desc, Flags, bHasFlags))
            {
                return false;
            }

            TArray<FRichCurveKey> Keys;
            bool bHasKeys = false;
            if (!ReadKeys(Context, Desc, Keys, bHasKeys))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: add curve '%s' (%d key(s))"), *Context.AnimAsset->GetName(), *Name, Keys.Num());

            const FAnimationCurveIdentifier CurveId = MakeFloatCurveId(Name);
            if (!Controller.AddCurve(CurveId, Flags, kShouldTransact))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: the data model refused curve '%s'"), *Context.AssetPath, *Name);
                return false;
            }

            if (bHasKeys && !Controller.SetCurveKeys(CurveId, Keys, kShouldTransact))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: the data model refused the keys of curve '%s'"), *Context.AssetPath, *Name);
                return false;
            }

            return true;
        }

        bool ApplyRemove(FAnimAssetEditContext& Context, IAnimationDataController& Controller, const TSharedPtr<FJsonObject>& Desc)
        {
            FString Name;
            if (!ReadName(Context, Desc, TEXT("Name"), Name) || !RequireCurve(Context, Name))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: remove curve '%s'"), *Context.AnimAsset->GetName(), *Name);

            if (!Controller.RemoveCurve(MakeFloatCurveId(Name), kShouldTransact))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: the data model refused to remove curve '%s'"), *Context.AssetPath, *Name);
                return false;
            }

            return true;
        }

        bool ApplyRename(FAnimAssetEditContext& Context, IAnimationDataController& Controller, const TSharedPtr<FJsonObject>& Desc)
        {
            FString Name;
            FString NewName;
            if (!ReadName(Context, Desc, TEXT("Name"), Name) || !ReadName(Context, Desc, TEXT("NewName"), NewName))
            {
                return false;
            }

            if (!RequireCurve(Context, Name) || !RefuseExistingCurve(Context, NewName))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: rename curve '%s' to '%s'"), *Context.AnimAsset->GetName(), *Name, *NewName);

            if (!Controller.RenameCurve(MakeFloatCurveId(Name), MakeFloatCurveId(NewName), kShouldTransact))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: the data model refused to rename curve '%s'"), *Context.AssetPath, *Name);
                return false;
            }

            return true;
        }

        bool ApplySetKeys(FAnimAssetEditContext& Context, IAnimationDataController& Controller, const TSharedPtr<FJsonObject>& Desc) const
        {
            FString Name;
            if (!ReadName(Context, Desc, TEXT("Name"), Name) || !RequireCurve(Context, Name))
            {
                return false;
            }

            TArray<FRichCurveKey> Keys;
            bool bHasKeys = false;
            if (!ReadKeys(Context, Desc, Keys, bHasKeys))
            {
                return false;
            }

            if (!bHasKeys)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: SetKeys needs Keys. Emptying a curve is Remove"), *Context.AssetPath);
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: set %d key(s) on curve '%s'"), *Context.AnimAsset->GetName(), Keys.Num(), *Name);

            if (!Controller.SetCurveKeys(MakeFloatCurveId(Name), Keys, kShouldTransact))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: the data model refused the keys of curve '%s'"), *Context.AssetPath, *Name);
                return false;
            }

            return true;
        }

        bool ApplyModify(FAnimAssetEditContext& Context, IAnimationDataController& Controller, const TSharedPtr<FJsonObject>& Desc) const
        {
            FString Name;
            if (!ReadName(Context, Desc, TEXT("Name"), Name) || !RequireCurve(Context, Name))
            {
                return false;
            }

            int32 Flags = 0;
            bool bHasFlags = false;
            if (!ReadFlags(Context, Desc, Flags, bHasFlags))
            {
                return false;
            }

            if (!bHasFlags)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Modify writes nothing. Expected Flags"), *Context.AssetPath);
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: modify curve '%s' flags 0x%02x"), *Context.AnimAsset->GetName(), *Name, Flags);

            if (!Controller.SetCurveFlags(MakeFloatCurveId(Name), Flags, kShouldTransact))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: the data model refused the flags of curve '%s'"), *Context.AssetPath, *Name);
                return false;
            }

            return true;
        }
    };
}

TUniquePtr<IAnimAssetWriter> MakeAnimCurveWriter()
{
    return MakeUnique<FAnimCurveWriter>();
}
