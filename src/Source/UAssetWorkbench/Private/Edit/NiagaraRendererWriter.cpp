#include "Edit/NiagaraSystemWriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"

namespace
{
    // Op "Add" | "Delete" | "Modify", Modify by default. Index is the renderer Index NiagaraSystemExport prints.
    class FNiagaraRendererWriter : public INiagaraSystemWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Renderers");
        }

        virtual bool Apply(FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            return NiagaraSystemEdit::ForEachOp(Section, GetSpecKey(), [&](const TSharedPtr<FJsonObject>& Op)
            {
                FString EmitterName;
                if (!Op->TryGetStringField(TEXT("Emitter"), EmitterName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Renderers op needs an Emitter"));
                    return false;
                }
                const FNiagaraEmitterHandle* Handle = NiagaraSystemEdit::ResolveEmitter(Context, EmitterName);
                if (!Handle)
                {
                    return false;
                }

                const FVersionedNiagaraEmitter Emitter = Handle->GetInstance();
                FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
                if (!Emitter.Emitter || !EmitterData)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Emitter %s has no editable data"), *EmitterName);
                    return false;
                }

                FString OpName = TEXT("Modify");
                Op->TryGetStringField(TEXT("Op"), OpName);

                UNiagaraRendererProperties* Renderer = nullptr;
                if (OpName == TEXT("Add"))
                {
                    FString ClassName;
                    UClass* Class = Op->TryGetStringField(TEXT("Class"), ClassName) ? UAssetWorkbench::ResolveClass(ClassName) : nullptr;
                    if (!Class || !Class->IsChildOf(UNiagaraRendererProperties::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Add needs a concrete renderer Class, got '%s'"), *ClassName);
                        return false;
                    }
                    Emitter.Emitter->Modify();
                    Renderer = NewObject<UNiagaraRendererProperties>(Emitter.Emitter, Class, NAME_None, RF_Transactional);
                    Emitter.Emitter->AddRenderer(Renderer, Emitter.Version);
                    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Added %s to %s at index %d"), *Class->GetName(), *EmitterName, EmitterData->GetRenderers().Num() - 1);
                }
                else
                {
                    int32 Index = INDEX_NONE;
                    const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
                    if (!Op->TryGetNumberField(TEXT("Index"), Index) || !Renderers.IsValidIndex(Index))
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s needs an Index in [0, %d) on %s"), *OpName, Renderers.Num(), *EmitterName);
                        return false;
                    }
                    Renderer = Renderers[Index];

                    if (OpName == TEXT("Delete"))
                    {
                        Emitter.Emitter->Modify();
                        Emitter.Emitter->RemoveRenderer(Renderer, Emitter.Version);
                        ++Context.Ops;
                        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Deleted renderer %d from %s"), Index, *EmitterName);
                        return true;
                    }
                    if (OpName != TEXT("Modify"))
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Renderers op '%s' unknown. Expected Add, Delete, Modify"), *OpName);
                        return false;
                    }
                }

                const TSharedPtr<FJsonObject>* Properties = nullptr;
                const TSharedPtr<FJsonObject> Changed = Op->TryGetObjectField(TEXT("Properties"), Properties) ? NiagaraSystemEdit::DropUnchangedProperties(Renderer, *Properties, Context.Unchanged) : nullptr;
                if (OpName == TEXT("Modify") && (!Changed.IsValid() || Changed->Values.Num() == 0))
                {
                    return true;
                }
                if (Changed.IsValid() && Changed->Values.Num() > 0)
                {
                    int32 Failures = 0;
                    Renderer->Modify();
                    const int32 Applied = UAssetWorkbench::ApplyProperties(Renderer, Changed, Failures);
                    if (Failures > 0)
                    {
                        return false;
                    }
                    // Bindings and material caches refresh on the edit-change broadcast.
                    Renderer->PostEditChange();
                    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Set %d propert%s on %s renderer %s"), Applied, Applied == 1 ? TEXT("y") : TEXT("ies"), *EmitterName, *Renderer->GetName());
                }

                ++Context.Ops;
                return true;
            });
        }
    };
}

TUniquePtr<INiagaraSystemWriter> MakeNiagaraRendererWriter()
{
    return MakeUnique<FNiagaraRendererWriter>();
}
