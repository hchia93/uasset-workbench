#include "Edit/MaterialGraphWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/MaterialExpression.h"

namespace
{
    // Keyed by node, one property object each. Paths follow ApplyProperties. MaterialFunction and
    // Declaration go through their setters, see MaterialGraphEdit::ApplyNodeProperties.
    class FMaterialGraphNodePropertyWriter : public IMaterialGraphWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("NodeProperties");
        }

        virtual bool Apply(FMaterialGraphEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TSharedPtr<FJsonObject>* ByNode = nullptr;
            if (!Section->TryGetObject(ByNode))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("NodeProperties must be an object keyed by node"));
                return false;
            }

            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ByNode)->Values)
            {
                UMaterialExpression* Expression = MaterialGraphEdit::ResolveNode(Context, Pair.Key);
                if (!Expression)
                {
                    return false;
                }

                const TSharedPtr<FJsonObject> Properties = Pair.Value->AsObject();
                if (!Properties.IsValid())
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("NodeProperties for %s must be an object"), *Pair.Key);
                    return false;
                }

                int32 Applied = 0;
                if (!MaterialGraphEdit::ApplyNodeProperties(Context, Expression, Properties, Applied))
                {
                    return false;
                }

                Context.Ops += Applied;
                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Set %d propert%s on %s"), Applied, Applied == 1 ? TEXT("y") : TEXT("ies"), *Pair.Key);
            }
            return true;
        }
    };
}

TUniquePtr<IMaterialGraphWriter> MakeMaterialGraphNodePropertyWriter()
{
    return MakeUnique<FMaterialGraphNodePropertyWriter>();
}
