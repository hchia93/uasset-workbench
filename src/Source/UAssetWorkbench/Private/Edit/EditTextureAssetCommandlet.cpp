#include "Edit/EditTextureAssetCommandlet.h"

// Editor-only by design: writes editor-only texture build settings. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "AssetCompilingManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TextureImportSettings.h"
#include "UObject/UnrealType.h"

#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

namespace
{
    // Build settings only. Anything that reaches into source pixels or platform data stays out of reach.
    const TCHAR* kAcceptedProperties[] =
    {
        TEXT("LODGroup"),
        TEXT("CompressionSettings"),
        TEXT("SRGB"),
        TEXT("MipGenSettings"),
        TEXT("MaxTextureSize"),
        TEXT("NeverStream"),
        TEXT("VirtualTextureStreaming"),
        TEXT("LossyCompressionAmount"),
        TEXT("bFlipGreenChannel"),
        TEXT("PowerOfTwoMode"),
        TEXT("CompressionNoAlpha"),
        TEXT("CompressionQuality"),
        TEXT("NumCinematicMipLevels"),
        TEXT("LODBias"),
        TEXT("Filter"),
        TEXT("AddressX"),
        TEXT("AddressY")
    };

    FString AcceptedPropertyList()
    {
        TArray<FString> Names;
        for (const TCHAR* Name : kAcceptedProperties)
        {
            Names.Add(Name);
        }

        return FString::Join(Names, TEXT(" "));
    }

    bool IsAcceptedProperty(const FString& Name)
    {
        for (const TCHAR* Accepted : kAcceptedProperties)
        {
            if (Name == Accepted)
            {
                return true;
            }
        }

        return false;
    }

    FString ReadPropertyText(UTexture* Texture, const FString& Name)
    {
        FProperty* Property = FindFProperty<FProperty>(Texture->GetClass(), *Name);
        if (!Property)
        {
            return FString();
        }

        FString Value;
        Property->ExportTextItem_Direct(Value, Property->ContainerPtrToValuePtr<void>(Texture), nullptr, Texture, PPF_None);
        return Value;
    }
}

UEditTextureAssetCommandlet::UEditTextureAssetCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UEditTextureAssetCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("UAssetWorkbench v%s - EditTextureAsset"), UASSET_WORKBENCH_VERSION_STRING);

    FString SpecPath;
    if (!FParse::Value(*Params, TEXT("spec="), SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("No spec specified. Usage: -spec=\"C:/path/spec.json\" [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    SpecPath = SpecPath.TrimQuotes();
    const bool bApply = FParse::Param(*Params, TEXT("apply"));

    // Writes happen either way, only save reads bApply. In-editor there is no process exit to discard them.
    if (!bApply && !IsRunningCommandlet())
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Dry run needs its own process to discard the in-memory edit. Close the editor and run the commandlet, or pass -apply."));
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    FString SpecText;
    if (!FFileHelper::LoadFileToString(SpecText, *SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to read spec: %s"), *SpecPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    TSharedPtr<FJsonObject> Spec;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SpecText);
    if (!FJsonSerializer::Deserialize(Reader, Spec) || !Spec.IsValid())
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Spec is not valid JSON: %s"), *SpecPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
    if (!Spec->TryGetArrayField(TEXT("Targets"), Targets))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Spec has no Targets array"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("EditTextureAsset: %d target(s) %s"), Targets->Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    TSet<UTexture*> Touched;
    int32 Ops = 0;
    for (const TSharedPtr<FJsonValue>& Value : *Targets)
    {
        const TSharedPtr<FJsonObject>& Entry = Value->AsObject();
        UAssetWorkbench::FFailureReasonCapture FailureReason;
        if (!Entry.IsValid() || !ApplyTarget(Entry, bApply, Touched, Ops))
        {
            UAssetWorkbench::ReportTargetFailure(TEXT("EditTextureAsset"), UAssetWorkbench::DescribeSpecTarget(Entry), FailureReason.GetReason());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d property write(s) staged across %d asset(s) (dry run, not saved)."), Ops, Touched.Num());
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    for (UTexture* Texture : Touched)
    {
        UAssetWorkbench::FFailureReasonCapture FailureReason;
        if (!UAssetWorkbench::CompileAndSavePackage(Texture, false))
        {
            UAssetWorkbench::ReportTargetFailure(TEXT("EditTextureAsset"), Texture->GetPathName(), FailureReason.GetReason());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d property write(s) across %d asset(s) (saved)."), Ops, Touched.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

bool UEditTextureAssetCommandlet::ApplyTarget(const TSharedPtr<FJsonObject>& Entry, bool bApply, TSet<UTexture*>& OutTouched, int32& OutOps) const
{
    FString AssetPath;
    if (!Entry->TryGetStringField(TEXT("AssetPath"), AssetPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Target has no AssetPath field"));
        return false;
    }

    const TSharedPtr<FJsonObject>* PropertiesField = nullptr;
    if (!Entry->TryGetObjectField(TEXT("Properties"), PropertiesField))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s writes nothing. Expected Properties"), *AssetPath);
        return false;
    }

    const TSharedPtr<FJsonObject>& Properties = *PropertiesField;
    if (Properties->Values.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s writes nothing. Expected Properties"), *AssetPath);
        return false;
    }

    UTexture* Texture = LoadObject<UTexture>(nullptr, *AssetPath);
    if (!Texture)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load texture: %s"), *AssetPath);
        return false;
    }

    TArray<FString> Names;
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
    {
        if (!IsAcceptedProperty(Pair.Key))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not an editable texture property. Accepted: %s"), *Pair.Key, *AcceptedPropertyList());
            return false;
        }

        if (!FindFProperty<FProperty>(Texture->GetClass(), *Pair.Key))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no property %s"), *Texture->GetClass()->GetName(), *Pair.Key);
            return false;
        }

        Names.Add(Pair.Key);
    }

    TMap<FString, FString> Before;
    for (const FString& Name : Names)
    {
        Before.Add(Name, ReadPropertyText(Texture, Name));
    }

    Texture->PreEditChange(nullptr);

    int32 Failures = 0;
    const int32 Written = UAssetWorkbench::ApplyProperties(Texture, Properties, Failures);
    if (Failures > 0)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%d propert(ies) rejected on %s, nothing saved"), Failures, *AssetPath);
        return false;
    }

    TMap<FString, FString> Applied;
    for (const FString& Name : Names)
    {
        Applied.Add(Name, ReadPropertyText(Texture, Name));
    }

    // The reimport pass reverts NoMips + Default back to group defaults on a power of two source, so any
    // value it takes back is written a second time and reported. Spec wins over engine import defaults.
    UE::TextureUtilitiesCommon::ApplyDefaultsForNewlyImportedTextures(Texture, true);

    for (const FString& Name : Names)
    {
        const FString Current = ReadPropertyText(Texture, Name);
        if (Current == Applied[Name])
        {
            continue;
        }

        TSharedRef<FJsonObject> Retry = MakeShared<FJsonObject>();
        Retry->SetField(Name, Properties->Values[Name]);

        int32 RetryFailures = 0;
        UAssetWorkbench::ApplyProperties(Texture, Retry, RetryFailures);
        UE_LOG(LogUAssetWorkbenchEditor, Warning, TEXT("%s: engine import defaults took %s back to %s, spec re-applied"), *Name, *Applied[Name], *Current);
    }

    Texture->PostEditChange();
    FAssetCompilingManager::Get().FinishAllCompilation();

    for (const FString& Name : Names)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s %s: %s -> %s"), *AssetPath, *Name, *Before[Name], *ReadPropertyText(Texture, Name));
    }

    OutOps += Written;
    OutTouched.Add(Texture);
    return true;
}
