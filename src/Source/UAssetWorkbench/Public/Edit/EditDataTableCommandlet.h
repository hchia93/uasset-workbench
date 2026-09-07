#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "EditDataTableCommandlet.generated.h"

class FJsonObject;
class UDataTable;

// Writes property values into existing DataTable rows. Row paths take the same "Field.Sub[2].Leaf" syntax
// the Blueprint writers use, so a DataTableExport value can be edited and fed straight back. Adding or
// removing rows is out of scope, that stays with unreal.DataTableFunctionLibrary.
//   UnrealEditor-Cmd.exe Project.uproject -run=EditDataTable -spec="C:/path/spec.json" [-apply]
// Contract: Docs/Edit.md
UCLASS()
class UEditDataTableCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UEditDataTableCommandlet();

    virtual int32 Main(const FString& Params) override;

private:
    // Returns false on the first failed row, leaving the table unsaved.
    bool ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TArray<UDataTable*>& OutTouched, int32& OutOps) const;
};
