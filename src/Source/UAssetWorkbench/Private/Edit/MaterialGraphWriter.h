#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UMaterial;
class UMaterialExpression;
class UMaterialFunction;

// One graph owner, carried across every writer of a single target. Exactly one of Material and Function
// is set. NodesById holds existing expressions and comments by object name and nodes the Nodes writer
// added under their spec Id, so NodeProperties, Edges and Layout can name either.
struct FMaterialGraphEditContext
{
    UMaterial* Material = nullptr;
    UMaterialFunction* Function = nullptr;
    FString AssetPath;

    TMap<FString, UMaterialExpression*> NodesById;
    int32 Ops = 0;
};

// Same split as IPCGGraphWriter: the commandlet owns the asset, each writer owns one facet.
class IMaterialGraphWriter
{
public:
    virtual ~IMaterialGraphWriter() = default;

    // Spec key this writer answers to. A target without the key skips the writer entirely.
    virtual const TCHAR* GetSpecKey() const = 0;

    // Returns false once anything fails, which aborts the whole target before it is saved.
    virtual bool Apply(FMaterialGraphEditContext& Context, const TSharedPtr<FJsonValue>& Section) = 0;
};

TUniquePtr<IMaterialGraphWriter> MakeMaterialGraphNodeWriter();
TUniquePtr<IMaterialGraphWriter> MakeMaterialGraphNodePropertyWriter();
TUniquePtr<IMaterialGraphWriter> MakeMaterialGraphEdgeWriter();
TUniquePtr<IMaterialGraphWriter> MakeMaterialGraphLayoutWriter();

namespace MaterialGraphEdit
{
    // Edges address the material's own output node by this key, its pins by the OutputConnections names.
    extern const TCHAR* const kMaterialNodeKey;

    UObject* GetOwner(const FMaterialGraphEditContext& Context);

    // Existing nodes answer to their object name, the same Name MaterialExport prints.
    void RegisterExistingNodes(FMaterialGraphEditContext& Context);

    // Falls back to a unique Label. A miss lists what the graph actually has.
    UMaterialExpression* ResolveNode(const FMaterialGraphEditContext& Context, const FString& NodeKey);

    FString DescribeNodes(const FMaterialGraphEditContext& Context);

    // Writes through ApplyProperties, except two fields whose setter does more than store: a function call
    // rebuilds its pins from MaterialFunction, a reroute usage binds Declaration by node key.
    bool ApplyNodeProperties(FMaterialGraphEditContext& Context, UMaterialExpression* Expression, const TSharedPtr<FJsonObject>& Properties, int32& OutApplied);
}
