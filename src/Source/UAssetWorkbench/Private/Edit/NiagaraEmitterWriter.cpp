#include "Edit/NiagaraSystemWriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraSystem.h"

namespace
{
    // Op "Add" | "Delete" | "Modify", Modify by default. Modify writes Enabled, NewName and the
    // FVersionedNiagaraEmitterData fields that NiagaraSystemExport prints under the emitter's Properties.
    class FNiagaraEmitterWriter : public INiagaraSystemWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Emitters");
        }

        virtual bool Apply(FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            return NiagaraSystemEdit::ForEachOp(Section, GetSpecKey(), [&](const TSharedPtr<FJsonObject>& Op)
            {
                FString OpName = TEXT("Modify");
                Op->TryGetStringField(TEXT("Op"), OpName);

                if (OpName == TEXT("Add"))
                {
                    return ApplyAdd(Context, Op);
                }

                FString EmitterName;
                if (!Op->TryGetStringField(TEXT("Emitter"), EmitterName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Emitters op needs an Emitter"));
                    return false;
                }
                const FNiagaraEmitterHandle* Handle = NiagaraSystemEdit::ResolveEmitter(Context, EmitterName);
                if (!Handle)
                {
                    return false;
                }

                if (OpName == TEXT("Delete"))
                {
                    Context.System->Modify();
                    Context.System->RemoveEmitterHandle(*Handle);
                    ++Context.Ops;
                    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Deleted emitter %s"), *EmitterName);
                    return true;
                }
                if (OpName != TEXT("Modify"))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Emitters op '%s' unknown. Expected Add, Delete, Modify"), *OpName);
                    return false;
                }
                return ApplyModify(Context, Op, *const_cast<FNiagaraEmitterHandle*>(Handle));
            });
        }

    private:
        static bool ApplyAdd(FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonObject>& Op)
        {
            FString AssetPath;
            UNiagaraEmitter* Source = Op->TryGetStringField(TEXT("Asset"), AssetPath) ? LoadObject<UNiagaraEmitter>(nullptr, *AssetPath) : nullptr;
            if (!Source)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Add needs an emitter Asset, failed to load '%s'"), *AssetPath);
                return false;
            }

            Context.System->Modify();
            const FGuid HandleId = FNiagaraEditorUtilities::AddEmitterToSystem(*Context.System, *Source, Source->GetExposedVersion().VersionGuid);

            FString NewName;
            for (FNiagaraEmitterHandle& Handle : Context.System->GetEmitterHandles())
            {
                if (Handle.GetId() == HandleId)
                {
                    if (Op->TryGetStringField(TEXT("Name"), NewName))
                    {
                        Handle.SetName(FName(*NewName), *Context.System);
                    }
                    ++Context.Ops;
                    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Added emitter %s from %s"), *Handle.GetName().ToString(), *AssetPath);
                    return true;
                }
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to add emitter %s"), *AssetPath);
            return false;
        }

        static bool ApplyModify(FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonObject>& Op, FNiagaraEmitterHandle& Handle)
        {
            const FString EmitterName = Handle.GetName().ToString();

            bool bEnabled = true;
            if (Op->TryGetBoolField(TEXT("Enabled"), bEnabled) && bEnabled == Handle.GetIsEnabled())
            {
                ++Context.Unchanged;
            }
            else if (Op->HasField(TEXT("Enabled")))
            {
                Context.System->Modify();
                Handle.SetIsEnabled(bEnabled, *Context.System, /* bRecompileIfChanged */ false);
                ++Context.Ops;
                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s emitter %s"), bEnabled ? TEXT("Enabled") : TEXT("Disabled"), *EmitterName);
            }

            const TSharedPtr<FJsonObject>* Properties = nullptr;
            if (Op->TryGetObjectField(TEXT("Properties"), Properties))
            {
                const FVersionedNiagaraEmitter Emitter = Handle.GetInstance();
                FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
                if (!Emitter.Emitter || !EmitterData)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Emitter %s has no editable data"), *EmitterName);
                    return false;
                }

                const TSharedPtr<FJsonObject> Changed = NiagaraSystemEdit::DropUnchangedStructProperties(FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, Emitter.Emitter, *Properties, Context.Unchanged);
                if (Changed->Values.Num() == 0)
                {
                    return ApplyRename(Context, Op, Handle);
                }

                int32 Failures = 0;
                Emitter.Emitter->Modify();
                const int32 Applied = UAssetWorkbench::ApplyStructProperties(FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, Emitter.Emitter, Changed, Failures);
                if (Failures > 0)
                {
                    return false;
                }
                // Sim target and bounds changes rebuild the emitter's compiled data on the change broadcast.
                Emitter.Emitter->PostEditChange();
                Context.Ops += Applied;
                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Set %d propert%s on emitter %s"), Applied, Applied == 1 ? TEXT("y") : TEXT("ies"), *EmitterName);
            }
            return ApplyRename(Context, Op, Handle);
        }

        // Renaming last, earlier fields of the same op still address the old name.
        static bool ApplyRename(FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonObject>& Op, FNiagaraEmitterHandle& Handle)
        {
            FString NewName;
            if (!Op->TryGetStringField(TEXT("NewName"), NewName))
            {
                return true;
            }

            const FString OldName = Handle.GetName().ToString();
            Context.System->Modify();
            Handle.SetName(FName(*NewName), *Context.System);
            ++Context.Ops;
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Renamed emitter %s to %s"), *OldName, *Handle.GetName().ToString());
            return true;
        }
    };
}

TUniquePtr<INiagaraSystemWriter> MakeNiagaraEmitterWriter()
{
    return MakeUnique<FNiagaraEmitterWriter>();
}
