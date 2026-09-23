#include "Edit/PCGGraphWriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "PCGGraph.h"

namespace
{
    // Graph-level fields, the same names PCGGraphExport prints under GraphSettings.
    class FPCGGraphSettingsWriter : public IPCGGraphWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("GraphSettings");
        }

        virtual bool Apply(FPCGGraphEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TSharedPtr<FJsonObject>* Properties = nullptr;
            if (!Section->TryGetObject(Properties))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("GraphSettings must be an object"));
                return false;
            }

            int32 Failures = 0;
            const int32 Applied = UAssetWorkbench::ApplyProperties(Context.Graph, *Properties, Failures);
            if (Failures > 0)
            {
                return false;
            }

            Context.Ops += Applied;
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Set %d graph setting(s)"), Applied);
            return true;
        }
    };
}

TUniquePtr<IPCGGraphWriter> MakePCGGraphSettingsWriter()
{
    return MakeUnique<FPCGGraphSettingsWriter>();
}
