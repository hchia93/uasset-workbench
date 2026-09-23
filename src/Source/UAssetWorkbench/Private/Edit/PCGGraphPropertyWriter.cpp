#include "Edit/PCGGraphWriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "PCGNode.h"
#include "PCGSettings.h"

namespace
{
    // Keyed by node, one property object each. Paths follow ApplyProperties, so "MeshSelectorParameters.MeshEntries[0].Descriptor.StaticMesh" reaches into an instanced selector.
    class FPCGGraphPropertyWriter : public IPCGGraphWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Properties");
        }

        virtual bool Apply(FPCGGraphEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TSharedPtr<FJsonObject>* ByNode = nullptr;
            if (!Section->TryGetObject(ByNode))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Properties must be an object keyed by node"));
                return false;
            }

            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ByNode)->Values)
            {
                UPCGNode* Node = PCGGraphEdit::ResolveNode(Context, Pair.Key);
                if (!Node)
                {
                    return false;
                }

                const TSharedPtr<FJsonObject> Properties = Pair.Value->AsObject();
                if (!Properties.IsValid())
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Properties for %s must be an object"), *Pair.Key);
                    return false;
                }

                // An instance node shares a settings asset with every graph that uses it, editing through
                // one node would silently change the others and leave the asset package unsaved.
                if (Node->IsInstance())
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is a settings instance, edit the settings asset instead"), *Pair.Key);
                    return false;
                }

                UPCGSettings* Settings = Node->GetSettings();
                if (!Settings)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no settings"), *Pair.Key);
                    return false;
                }

                int32 Failures = 0;
                const int32 Applied = UAssetWorkbench::ApplyProperties(Settings, Properties, Failures);
                if (Failures > 0)
                {
                    return false;
                }

                PCGGraphEdit::NotifySettingsChanged(Node);
                Context.Ops += Applied;
                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Set %d propert%s on %s"), Applied, Applied == 1 ? TEXT("y") : TEXT("ies"), *Pair.Key);
            }
            return true;
        }
    };
}

TUniquePtr<IPCGGraphWriter> MakePCGGraphPropertyWriter()
{
    return MakeUnique<FPCGGraphPropertyWriter>();
}
