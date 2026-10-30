#include "Export/MaterialExportCommandlet.h"
#include "Export/MaterialGraphJson.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceBasePropertyOverrides.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "UObject/UnrealType.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    // Custom-node output type is a plain enum (not UENUM), so map it by hand.
    FString CustomOutputTypeToString(ECustomMaterialOutputType Type)
    {
        switch (Type)
        {
        case CMOT_Float1:             return TEXT("Float1");
        case CMOT_Float2:             return TEXT("Float2");
        case CMOT_Float3:             return TEXT("Float3");
        case CMOT_Float4:             return TEXT("Float4");
        case CMOT_MaterialAttributes: return TEXT("MaterialAttributes");
        default:                      return TEXT("Unknown");
        }
    }

    // Read-only and instanced properties are bookkeeping, not settings anyone writes back. Wires print as
    // Inputs[] or OutputConnections, so they stay out too.
    TSharedPtr<FJsonObject> ExportEditableDelta(const UObject* Object, const TSet<FName>& Skip, bool bFull)
    {
        TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
        const UObject* Default = Object->GetClass()->GetDefaultObject();

        for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
        {
            const FProperty* Property = *It;
            const bool bEditable = Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_Deprecated | CPF_InstancedReference);
            if (!bEditable || Skip.Contains(Property->GetFName()) || MaterialGraphJson::IsPinProperty(Property))
            {
                continue;
            }

            for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim; ++ArrayIndex)
            {
                if (!bFull && Property->Identical_InContainer(Object, Default, ArrayIndex, PPF_DeepComparison))
                {
                    continue;
                }

                // ExportText_InContainer with no delta skips a zero value as if it were the default.
                FString Value;
                Property->ExportTextItem_Direct(Value, Property->ContainerPtrToValuePtr<void>(Object, ArrayIndex), nullptr, const_cast<UObject*>(Object), PPF_None);
                const FString Key = Property->ArrayDim > 1 ? FString::Printf(TEXT("%s[%d]"), *Property->GetName(), ArrayIndex) : Property->GetName();
                Props->SetStringField(Key, Value);
            }
        }

        return Props;
    }

    TArray<TSharedPtr<FJsonValue>> ExportScalarValues(TConstArrayView<FScalarParameterValue> Values)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FScalarParameterValue& Param : Values)
        {
            TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
            ParamObj->SetStringField(TEXT("Name"), Param.ParameterInfo.Name.ToString());
            ParamObj->SetNumberField(TEXT("Value"), Param.ParameterValue);
            Array.Add(MakeShared<FJsonValueObject>(ParamObj));
        }
        return Array;
    }

    TArray<TSharedPtr<FJsonValue>> ExportVectorValues(TConstArrayView<FVectorParameterValue> Values)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FVectorParameterValue& Param : Values)
        {
            TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
            ParamObj->SetStringField(TEXT("Name"), Param.ParameterInfo.Name.ToString());
            ParamObj->SetStringField(TEXT("Value"), Param.ParameterValue.ToString());
            Array.Add(MakeShared<FJsonValueObject>(ParamObj));
        }
        return Array;
    }

    TArray<TSharedPtr<FJsonValue>> ExportTextureValues(TConstArrayView<FTextureParameterValue> Values)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FTextureParameterValue& Param : Values)
        {
            TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
            ParamObj->SetStringField(TEXT("Name"), Param.ParameterInfo.Name.ToString());
            if (Param.ParameterValue)
            {
                ParamObj->SetStringField(TEXT("Texture"), Param.ParameterValue->GetName());
                ParamObj->SetStringField(TEXT("TexturePath"), Param.ParameterValue->GetPathName());
            }
            else
            {
                ParamObj->SetStringField(TEXT("Texture"), TEXT("None"));
            }
            Array.Add(MakeShared<FJsonValueObject>(ParamObj));
        }
        return Array;
    }

    TArray<TSharedPtr<FJsonValue>> ExportStaticSwitchValues(TConstArrayView<FStaticSwitchParameter> Values)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FStaticSwitchParameter& Param : Values)
        {
            TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
            ParamObj->SetStringField(TEXT("Name"), Param.ParameterInfo.Name.ToString());
            ParamObj->SetBoolField(TEXT("Value"), Param.Value);
            ParamObj->SetBoolField(TEXT("Override"), Param.bOverride);
            Array.Add(MakeShared<FJsonValueObject>(ParamObj));
        }
        return Array;
    }

    void SetPositionFields(const TSharedPtr<FJsonObject>& Obj, const UMaterialExpression* Expression)
    {
        Obj->SetNumberField(TEXT("PositionX"), Expression->MaterialExpressionEditorX);
        Obj->SetNumberField(TEXT("PositionY"), Expression->MaterialExpressionEditorY);
    }
}

UMaterialExportCommandlet::UMaterialExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UMaterialExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - MaterialExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);
    m_bFull = FParse::Param(*Params, TEXT("full"));

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/M_A,/Game/Path/MI_B\" [-full]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
        if (!Asset)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load: %s"), *AssetPath);
            continue;
        }

        // UMaterialInstanceConstant is a UMaterialInstance, so instances always take the instance branch.
        TSharedPtr<FJsonObject> JsonObject;
        if (UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(Asset))
        {
            JsonObject = ExportMaterialInstance(MaterialInstance);
        }
        else if (UMaterial* Material = Cast<UMaterial>(Asset))
        {
            JsonObject = ExportMaterial(Material);
        }
        else if (UMaterialFunction* Function = Cast<UMaterialFunction>(Asset))
        {
            JsonObject = ExportMaterialFunction(Function);
        }
        else if (UMaterialFunctionInstance* FunctionInstance = Cast<UMaterialFunctionInstance>(Asset))
        {
            JsonObject = ExportMaterialFunctionInstance(FunctionInstance);
        }
        else if (UMaterialParameterCollection* Collection = Cast<UMaterialParameterCollection>(Asset))
        {
            JsonObject = ExportParameterCollection(Collection);
        }
        else
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("%s is a %s, not a material, material function or parameter collection"), *AssetPath, *Asset->GetClass()->GetName());
            continue;
        }

        if (JsonObject.IsValid())
        {
            UAssetWorkbench::FExportTarget ExportTarget(AssetPath);
            if (ExportTarget.Save(JsonObject.ToSharedRef()))
            {
                UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *ExportTarget.GetPath());
                ExportedCount++;
            }
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d materials exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> UMaterialExportCommandlet::ExportMaterial(UMaterial* Material) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("Material"));
    Root->SetStringField(TEXT("MaterialName"), Material->GetName());
    Root->SetStringField(TEXT("AssetPath"), Material->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    // Global settings
    const EMaterialShadingModel ShadingModel = Material->GetShadingModels().GetFirstShadingModel();
    Root->SetStringField(TEXT("ShadingModel"), StaticEnum<EMaterialShadingModel>()->GetNameStringByValue(static_cast<int64>(ShadingModel)));
    Root->SetStringField(TEXT("BlendMode"), StaticEnum<EBlendMode>()->GetNameStringByValue(static_cast<int64>(Material->BlendMode)));
    Root->SetBoolField(TEXT("TwoSided"), Material->IsTwoSided());
    Root->SetStringField(TEXT("MaterialDomain"), StaticEnum<EMaterialDomain>()->GetNameStringByValue(static_cast<int64>(Material->MaterialDomain)));
    Root->SetNumberField(TEXT("OpacityMaskClipValue"), Material->OpacityMaskClipValue);
    Root->SetBoolField(TEXT("bAutomaticallySetUsageInEditor"), Material->bAutomaticallySetUsageInEditor != 0);

    TSet<FName> SettingsSkip = { TEXT("ShadingModel"), TEXT("ShadingModels"), TEXT("BlendMode"), TEXT("TwoSided"), TEXT("MaterialDomain"), TEXT("OpacityMaskClipValue"), TEXT("bAutomaticallySetUsageInEditor") };

    // Usage flags decide which permutations compile, and EditMaterialAsset writes them, so an export has
    // to show them. UMaterial::GetUsageName is not exported, hence the reflected property names.
    TArray<TSharedPtr<FJsonValue>> UsageFlagsArray;
    for (TFieldIterator<FBoolProperty> It(UMaterial::StaticClass()); It; ++It)
    {
        if (!It->GetName().StartsWith(TEXT("bUsedWith")) || It->HasAnyPropertyFlags(CPF_Deprecated))
        {
            continue;
        }

        SettingsSkip.Add(It->GetFName());
        if (It->GetPropertyValue_InContainer(Material))
        {
            UsageFlagsArray.Add(MakeShared<FJsonValueString>(It->GetName()));
        }
    }
    Root->SetArrayField(TEXT("UsageFlags"), UsageFlagsArray);

    TSharedPtr<FJsonObject> Settings = ExportEditableDelta(Material, SettingsSkip, m_bFull);
    if (Settings->Values.Num() > 0)
    {
        Root->SetObjectField(TEXT("Settings"), Settings);
    }

    Root->SetNumberField(TEXT("RootPositionX"), Material->EditorX);
    Root->SetNumberField(TEXT("RootPositionY"), Material->EditorY);

    // Expressions (node graph)
    TArray<TSharedPtr<FJsonValue>> ExpressionsArray;
    for (UMaterialExpression* Expression : Material->GetExpressions())
    {
        if (Expression)
        {
            TSharedPtr<FJsonObject> ExprObj = ExportExpression(Expression);
            if (ExprObj.IsValid())
            {
                ExpressionsArray.Add(MakeShared<FJsonValueObject>(ExprObj));
            }
        }
    }
    Root->SetArrayField(TEXT("Expressions"), ExpressionsArray);

    TArray<TSharedPtr<FJsonValue>> CommentsArray;
    for (const UMaterialExpressionComment* Comment : Material->GetEditorComments())
    {
        if (Comment)
        {
            CommentsArray.Add(MakeShared<FJsonValueObject>(ExportComment(Comment)));
        }
    }
    if (CommentsArray.Num() > 0)
    {
        Root->SetArrayField(TEXT("Comments"), CommentsArray);
    }

    // Expression connections (input pins on the material output node). Keys are the editor-only data
    // property names, the same names EditMaterialAsset's Edges accept.
    TSharedPtr<FJsonObject> OutputConnections = MakeShared<FJsonObject>();
    MaterialGraphJson::ForEachMaterialInput(Material, [&OutputConnections](const FString& PinName, FExpressionInput& Input)
    {
        if (Input.Expression)
        {
            TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
            ConnObj->SetStringField(TEXT("Expression"), Input.Expression->GetName());
            ConnObj->SetStringField(TEXT("ExpressionClass"), Input.Expression->GetClass()->GetName());
            ConnObj->SetNumberField(TEXT("OutputIndex"), Input.OutputIndex);
            OutputConnections->SetObjectField(PinName, ConnObj);
        }
    });

    if (OutputConnections->Values.Num() > 0)
    {
        Root->SetObjectField(TEXT("OutputConnections"), OutputConnections);
    }

    return Root;
}

TSharedPtr<FJsonObject> UMaterialExportCommandlet::ExportMaterialInstance(UMaterialInstance* MaterialInstance) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("MaterialInstance"));
    Root->SetStringField(TEXT("MaterialInstanceName"), MaterialInstance->GetName());
    Root->SetStringField(TEXT("AssetPath"), MaterialInstance->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    // Parent material
    if (MaterialInstance->Parent)
    {
        Root->SetStringField(TEXT("Parent"), MaterialInstance->Parent->GetName());
        Root->SetStringField(TEXT("ParentPath"), MaterialInstance->Parent->GetPathName());
    }

    Root->SetArrayField(TEXT("ScalarParameters"), ExportScalarValues(MaterialInstance->ScalarParameterValues));
    Root->SetArrayField(TEXT("VectorParameters"), ExportVectorValues(MaterialInstance->VectorParameterValues));
    Root->SetArrayField(TEXT("TextureParameters"), ExportTextureValues(MaterialInstance->TextureParameterValues));
    Root->SetArrayField(TEXT("StaticSwitchParameters"), ExportStaticSwitchValues(MaterialInstance->GetStaticParameters().StaticSwitchParameters));

    // Only the overrides actually flagged, an instance that overrides nothing writes an empty object.
    const FMaterialInstanceBasePropertyOverrides& Overrides = MaterialInstance->BasePropertyOverrides;
    TSharedPtr<FJsonObject> OverridesObj = MakeShared<FJsonObject>();
    if (Overrides.bOverride_BlendMode)
    {
        OverridesObj->SetStringField(TEXT("BlendMode"), StaticEnum<EBlendMode>()->GetNameStringByValue(Overrides.BlendMode.GetValue()));
    }
    if (Overrides.bOverride_ShadingModel)
    {
        OverridesObj->SetStringField(TEXT("ShadingModel"), StaticEnum<EMaterialShadingModel>()->GetNameStringByValue(Overrides.ShadingModel.GetValue()));
    }
    if (Overrides.bOverride_TwoSided)
    {
        OverridesObj->SetBoolField(TEXT("TwoSided"), Overrides.TwoSided != 0);
    }
    if (Overrides.bOverride_OpacityMaskClipValue)
    {
        OverridesObj->SetNumberField(TEXT("OpacityMaskClipValue"), Overrides.OpacityMaskClipValue);
    }
    Root->SetObjectField(TEXT("BasePropertyOverrides"), OverridesObj);

    return Root;
}

TSharedPtr<FJsonObject> UMaterialExportCommandlet::ExportMaterialFunction(UMaterialFunction* Function) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("MaterialFunction"));
    Root->SetStringField(TEXT("MaterialFunctionName"), Function->GetName());
    Root->SetStringField(TEXT("AssetPath"), Function->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    // Layer and layer blend are subclasses that share this graph, the class says which one.
    Root->SetStringField(TEXT("Class"), Function->GetClass()->GetName());
    Root->SetStringField(TEXT("Description"), Function->Description);
    Root->SetBoolField(TEXT("bExposeToLibrary"), Function->bExposeToLibrary != 0);
    Root->SetStringField(TEXT("MaterialFunctionUsage"), StaticEnum<EMaterialFunctionUsage>()->GetNameStringByValue(static_cast<int64>(Function->GetMaterialFunctionUsage())));

    const TSet<FName> SettingsSkip = { TEXT("Description"), TEXT("bExposeToLibrary"), TEXT("MaterialFunctionUsage") };
    TSharedPtr<FJsonObject> Settings = ExportEditableDelta(Function, SettingsSkip, m_bFull);
    if (Settings->Values.Num() > 0)
    {
        Root->SetObjectField(TEXT("Settings"), Settings);
    }

    // The signature a caller sees, in pin order. SortPriority decides it, not the order nodes were added.
    TArray<FFunctionExpressionInput> FunctionInputs;
    TArray<FFunctionExpressionOutput> FunctionOutputs;
    Function->GetInputsAndOutputs(FunctionInputs, FunctionOutputs);

    TArray<TSharedPtr<FJsonValue>> InputsArray;
    for (const FFunctionExpressionInput& Entry : FunctionInputs)
    {
        const UMaterialExpressionFunctionInput* Input = Entry.ExpressionInput;
        if (!Input)
        {
            continue;
        }

        TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
        InputObj->SetStringField(TEXT("Name"), Input->InputName.ToString());
        InputObj->SetStringField(TEXT("Expression"), Input->GetName());
        InputObj->SetStringField(TEXT("Type"), StaticEnum<EFunctionInputType>()->GetNameStringByValue(static_cast<int64>(Input->InputType.GetValue())));
        InputObj->SetStringField(TEXT("Description"), Input->Description);
        InputObj->SetNumberField(TEXT("SortPriority"), Input->SortPriority);
        InputObj->SetBoolField(TEXT("bUsePreviewValueAsDefault"), Input->bUsePreviewValueAsDefault != 0);
        InputObj->SetStringField(TEXT("PreviewValue"), Input->PreviewValue.ToString());
        InputsArray.Add(MakeShared<FJsonValueObject>(InputObj));
    }
    Root->SetArrayField(TEXT("Inputs"), InputsArray);

    TArray<TSharedPtr<FJsonValue>> OutputsArray;
    for (const FFunctionExpressionOutput& Entry : FunctionOutputs)
    {
        const UMaterialExpressionFunctionOutput* Output = Entry.ExpressionOutput;
        if (!Output)
        {
            continue;
        }

        TSharedPtr<FJsonObject> OutputObj = MakeShared<FJsonObject>();
        OutputObj->SetStringField(TEXT("Name"), Output->OutputName.ToString());
        OutputObj->SetStringField(TEXT("Expression"), Output->GetName());
        OutputObj->SetStringField(TEXT("Description"), Output->Description);
        OutputObj->SetNumberField(TEXT("SortPriority"), Output->SortPriority);
        OutputsArray.Add(MakeShared<FJsonValueObject>(OutputObj));
    }
    Root->SetArrayField(TEXT("Outputs"), OutputsArray);

    TArray<TSharedPtr<FJsonValue>> ExpressionsArray;
    for (UMaterialExpression* Expression : Function->GetExpressions())
    {
        if (Expression)
        {
            ExpressionsArray.Add(MakeShared<FJsonValueObject>(ExportExpression(Expression)));
        }
    }
    Root->SetArrayField(TEXT("Expressions"), ExpressionsArray);

    TArray<TSharedPtr<FJsonValue>> CommentsArray;
    for (const UMaterialExpressionComment* Comment : Function->GetEditorComments())
    {
        if (Comment)
        {
            CommentsArray.Add(MakeShared<FJsonValueObject>(ExportComment(Comment)));
        }
    }
    if (CommentsArray.Num() > 0)
    {
        Root->SetArrayField(TEXT("Comments"), CommentsArray);
    }

    return Root;
}

TSharedPtr<FJsonObject> UMaterialExportCommandlet::ExportMaterialFunctionInstance(UMaterialFunctionInstance* Instance) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("MaterialFunctionInstance"));
    Root->SetStringField(TEXT("MaterialFunctionInstanceName"), Instance->GetName());
    Root->SetStringField(TEXT("AssetPath"), Instance->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());
    Root->SetStringField(TEXT("Class"), Instance->GetClass()->GetName());

    if (Instance->Parent)
    {
        Root->SetStringField(TEXT("Parent"), Instance->Parent->GetName());
        Root->SetStringField(TEXT("ParentPath"), Instance->Parent->GetPathName());
    }
    if (Instance->Base)
    {
        Root->SetStringField(TEXT("Base"), Instance->Base->GetName());
        Root->SetStringField(TEXT("BasePath"), Instance->Base->GetPathName());
    }

    Root->SetArrayField(TEXT("ScalarParameters"), ExportScalarValues(Instance->ScalarParameterValues));
    Root->SetArrayField(TEXT("VectorParameters"), ExportVectorValues(Instance->VectorParameterValues));
    Root->SetArrayField(TEXT("TextureParameters"), ExportTextureValues(Instance->TextureParameterValues));
    Root->SetArrayField(TEXT("StaticSwitchParameters"), ExportStaticSwitchValues(Instance->StaticSwitchParameterValues));

    return Root;
}

TSharedPtr<FJsonObject> UMaterialExportCommandlet::ExportParameterCollection(UMaterialParameterCollection* Collection) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("MaterialParameterCollection"));
    Root->SetStringField(TEXT("MaterialParameterCollectionName"), Collection->GetName());
    Root->SetStringField(TEXT("AssetPath"), Collection->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    if (const UMaterialParameterCollection* Base = Collection->GetBaseParameterCollection())
    {
        Root->SetStringField(TEXT("BasePath"), Base->GetPathName());
    }

    TArray<TSharedPtr<FJsonValue>> ScalarsArray;
    for (const FCollectionScalarParameter& Param : Collection->ScalarParameters)
    {
        TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
        ParamObj->SetStringField(TEXT("Name"), Param.ParameterName.ToString());
        ParamObj->SetNumberField(TEXT("DefaultValue"), Param.DefaultValue);
        ScalarsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
    }
    Root->SetArrayField(TEXT("ScalarParameters"), ScalarsArray);

    TArray<TSharedPtr<FJsonValue>> VectorsArray;
    for (const FCollectionVectorParameter& Param : Collection->VectorParameters)
    {
        TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
        ParamObj->SetStringField(TEXT("Name"), Param.ParameterName.ToString());
        ParamObj->SetStringField(TEXT("DefaultValue"), Param.DefaultValue.ToString());
        VectorsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
    }
    Root->SetArrayField(TEXT("VectorParameters"), VectorsArray);

    return Root;
}

TSharedPtr<FJsonObject> UMaterialExportCommandlet::ExportExpression(UMaterialExpression* Expression) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    Obj->SetStringField(TEXT("Name"), Expression->GetName());
    Obj->SetStringField(TEXT("Class"), Expression->GetClass()->GetName());
    Obj->SetStringField(TEXT("Description"), Expression->GetDescription());

    const FString Label = MaterialGraphJson::NodeLabel(Expression);
    if (!Label.IsEmpty())
    {
        Obj->SetStringField(TEXT("Label"), Label);
    }

    SetPositionFields(Obj, Expression);
    if (!Expression->Desc.IsEmpty())
    {
        Obj->SetStringField(TEXT("Comment"), Expression->Desc);
    }

    // Fields printed on their own stay out of Properties, so one value never shows twice.
    TSet<FName> PropertySkip = { TEXT("Desc"), TEXT("MaterialFunction") };

    // Parameter-specific data
    if (UMaterialExpressionScalarParameter* ScalarParam = Cast<UMaterialExpressionScalarParameter>(Expression))
    {
        Obj->SetStringField(TEXT("ParameterName"), ScalarParam->ParameterName.ToString());
        Obj->SetNumberField(TEXT("DefaultValue"), ScalarParam->DefaultValue);
        Obj->SetStringField(TEXT("Group"), ScalarParam->Group.ToString());
        PropertySkip.Append({ TEXT("ParameterName"), TEXT("DefaultValue"), TEXT("Group") });
    }
    else if (UMaterialExpressionVectorParameter* VectorParam = Cast<UMaterialExpressionVectorParameter>(Expression))
    {
        Obj->SetStringField(TEXT("ParameterName"), VectorParam->ParameterName.ToString());
        Obj->SetStringField(TEXT("DefaultValue"), VectorParam->DefaultValue.ToString());
        Obj->SetStringField(TEXT("Group"), VectorParam->Group.ToString());
        PropertySkip.Append({ TEXT("ParameterName"), TEXT("DefaultValue"), TEXT("Group") });
    }
    else if (UMaterialExpressionTextureSampleParameter* TextureParam = Cast<UMaterialExpressionTextureSampleParameter>(Expression))
    {
        Obj->SetStringField(TEXT("ParameterName"), TextureParam->ParameterName.ToString());
        if (TextureParam->Texture)
        {
            Obj->SetStringField(TEXT("DefaultTexture"), TextureParam->Texture->GetPathName());
        }
        Obj->SetStringField(TEXT("Group"), TextureParam->Group.ToString());
        PropertySkip.Append({ TEXT("ParameterName"), TEXT("Texture"), TEXT("Group") });
    }
    else if (UMaterialExpressionCustom* CustomExpr = Cast<UMaterialExpressionCustom>(Expression))
    {
        // The HLSL body, the whole reason this branch exists. Node-graph export alone hides it.
        Obj->SetStringField(TEXT("Code"), CustomExpr->Code);
        Obj->SetStringField(TEXT("OutputType"), CustomOutputTypeToString(CustomExpr->OutputType));
        PropertySkip.Append({ TEXT("Code"), TEXT("OutputType"), TEXT("AdditionalDefines"), TEXT("IncludeFilePaths"), TEXT("AdditionalOutputs"), TEXT("Inputs") });

        if (CustomExpr->AdditionalDefines.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> DefinesArray;
            for (const FCustomDefine& Define : CustomExpr->AdditionalDefines)
            {
                TSharedPtr<FJsonObject> DefineObj = MakeShared<FJsonObject>();
                DefineObj->SetStringField(TEXT("Name"), Define.DefineName);
                DefineObj->SetStringField(TEXT("Value"), Define.DefineValue);
                DefinesArray.Add(MakeShared<FJsonValueObject>(DefineObj));
            }
            Obj->SetArrayField(TEXT("AdditionalDefines"), DefinesArray);
        }

        if (CustomExpr->IncludeFilePaths.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> IncludesArray;
            for (const FString& IncludePath : CustomExpr->IncludeFilePaths)
            {
                IncludesArray.Add(MakeShared<FJsonValueString>(IncludePath));
            }
            Obj->SetArrayField(TEXT("IncludeFilePaths"), IncludesArray);
        }

        if (CustomExpr->AdditionalOutputs.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> OutputsArray;
            for (const FCustomOutput& Output : CustomExpr->AdditionalOutputs)
            {
                TSharedPtr<FJsonObject> OutputObj = MakeShared<FJsonObject>();
                OutputObj->SetStringField(TEXT("Name"), Output.OutputName.ToString());
                OutputObj->SetStringField(TEXT("OutputType"), CustomOutputTypeToString(Output.OutputType));
                OutputsArray.Add(MakeShared<FJsonValueObject>(OutputObj));
            }
            Obj->SetArrayField(TEXT("AdditionalOutputs"), OutputsArray);
        }
    }

    if (const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
    {
        if (Call->MaterialFunction)
        {
            Obj->SetStringField(TEXT("Function"), Call->MaterialFunction->GetPathName());
        }
    }

    // A usage node carries no wire to its declaration, this field is the only link.
    if (const UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Expression))
    {
        if (Usage->Declaration)
        {
            Obj->SetStringField(TEXT("Declaration"), Usage->Declaration->GetName());
        }
    }

    TSharedPtr<FJsonObject> Properties = ExportEditableDelta(Expression, PropertySkip, m_bFull);
    if (Properties->Values.Num() > 0)
    {
        Obj->SetObjectField(TEXT("Properties"), Properties);
    }

    // OutputIndex in Inputs[] counts into this list. A single output needs no name to be reached.
    const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
    if (Outputs.Num() > 1)
    {
        TArray<TSharedPtr<FJsonValue>> OutputPins;
        for (const FExpressionOutput& Output : Outputs)
        {
            OutputPins.Add(MakeShared<FJsonValueString>(Output.OutputName.ToString()));
        }
        Obj->SetArrayField(TEXT("OutputPins"), OutputPins);
    }

    if (m_bFull)
    {
        TArray<TSharedPtr<FJsonValue>> InputPins;
        for (FExpressionInputIterator It(Expression); It; ++It)
        {
            InputPins.Add(MakeShared<FJsonValueString>(Expression->GetInputName(It.Index).ToString()));
        }
        if (InputPins.Num() > 0)
        {
            Obj->SetArrayField(TEXT("InputPins"), InputPins);
        }
    }

    // Input connections
    {
        TArray<TSharedPtr<FJsonValue>> InputsArray;
        for (FExpressionInputIterator It(Expression); It; ++It)
        {
            FExpressionInput* Input = It.Input;
            if (Input && Input->Expression)
            {
                TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
                InputObj->SetStringField(TEXT("InputName"), Expression->GetInputName(It.Index).ToString());
                InputObj->SetStringField(TEXT("ConnectedTo"), Input->Expression->GetName());
                InputObj->SetStringField(TEXT("ConnectedClass"), Input->Expression->GetClass()->GetName());
                InputObj->SetNumberField(TEXT("OutputIndex"), Input->OutputIndex);
                InputsArray.Add(MakeShared<FJsonValueObject>(InputObj));
            }
        }
        if (InputsArray.Num() > 0)
        {
            Obj->SetArrayField(TEXT("Inputs"), InputsArray);
        }
    }

    return Obj;
}

TSharedPtr<FJsonObject> UMaterialExportCommandlet::ExportComment(const UMaterialExpressionComment* Comment) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("Name"), Comment->GetName());
    Obj->SetStringField(TEXT("Text"), Comment->Text);
    SetPositionFields(Obj, Comment);
    Obj->SetNumberField(TEXT("SizeX"), Comment->SizeX);
    Obj->SetNumberField(TEXT("SizeY"), Comment->SizeY);
    Obj->SetStringField(TEXT("Color"), Comment->CommentColor.ToString());
    return Obj;
}
