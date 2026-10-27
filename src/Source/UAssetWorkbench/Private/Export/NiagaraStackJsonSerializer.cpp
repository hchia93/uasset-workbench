#include "Export/NiagaraStackJsonSerializer.h"
#include "UAssetWorkbenchUtil.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"

#include "INiagaraEditorTypeUtilities.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEditorModule.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterMapHistory.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace
{
    UNiagaraGraph* GetSourceGraph(UNiagaraScriptSourceBase* SourceBase)
    {
        UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase);
        return Source ? Source->NodeGraph : nullptr;
    }

    UNiagaraNodeOutput* FindOutputNode(UNiagaraGraph* Graph, ENiagaraScriptUsage Usage, const FGuid& UsageId = FGuid())
    {
        if (!Graph)
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Node);
            if (Output && Output->GetUsage() == Usage && (!UsageId.IsValid() || Output->GetUsageId() == UsageId))
            {
                return Output;
            }
        }
        return nullptr;
    }

    void AddStage(TArray<FNiagaraStackStage>& OutStages, UNiagaraSystem* System, const FNiagaraEmitterHandle* Handle, UNiagaraGraph* Graph, const FString& Name, ENiagaraScriptUsage Usage, const FGuid& UsageId = FGuid())
    {
        UNiagaraNodeOutput* Output = FindOutputNode(Graph, Usage, UsageId);
        if (!Output)
        {
            return;
        }

        FNiagaraStackStage& Stage = OutStages.AddDefaulted_GetRef();
        Stage.Name = Name;
        Stage.Usage = Usage;
        Stage.UsageId = UsageId;
        Stage.OutputNode = Output;
        Stage.System = System;
        Stage.EmitterHandle = Handle;
        if (Handle)
        {
            Stage.Emitter = Handle->GetInstance();
        }
    }

    void CollectEmitterStages(UNiagaraSystem* System, const FNiagaraEmitterHandle& Handle, TArray<FNiagaraStackStage>& OutStages)
    {
        FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
        UNiagaraGraph* Graph = EmitterData ? GetSourceGraph(EmitterData->GraphSource) : nullptr;
        if (!Graph)
        {
            return;
        }

        AddStage(OutStages, System, &Handle, Graph, TEXT("EmitterSpawn"), ENiagaraScriptUsage::EmitterSpawnScript);
        AddStage(OutStages, System, &Handle, Graph, TEXT("EmitterUpdate"), ENiagaraScriptUsage::EmitterUpdateScript);
        AddStage(OutStages, System, &Handle, Graph, TEXT("ParticleSpawn"), ENiagaraScriptUsage::ParticleSpawnScript);
        AddStage(OutStages, System, &Handle, Graph, TEXT("ParticleUpdate"), ENiagaraScriptUsage::ParticleUpdateScript);

        for (const FNiagaraEventScriptProperties& Event : EmitterData->GetEventHandlers())
        {
            if (Event.Script)
            {
                AddStage(OutStages, System, &Handle, Graph, FString::Printf(TEXT("Event:%s"), *Event.SourceEventName.ToString()), ENiagaraScriptUsage::ParticleEventScript, Event.Script->GetUsageId());
            }
        }
        for (const UNiagaraSimulationStageBase* SimStage : EmitterData->GetSimulationStages())
        {
            if (SimStage && SimStage->Script)
            {
                AddStage(OutStages, System, &Handle, Graph, FString::Printf(TEXT("SimulationStage:%s"), *SimStage->SimulationStageName.ToString()), ENiagaraScriptUsage::ParticleSimulationStageScript, SimStage->Script->GetUsageId());
            }
        }
    }

    FCompileConstantResolver MakeResolver(const FNiagaraStackStage& Stage)
    {
        return Stage.IsSystemStage() ? FCompileConstantResolver(Stage.System, Stage.Usage) : FCompileConstantResolver(Stage.Emitter, Stage.Usage);
    }

    UEdGraphPin* FindParameterMapPin(UNiagaraNode& Node, EEdGraphPinDirection Direction)
    {
        for (UEdGraphPin* Pin : Node.Pins)
        {
            if (Pin && Pin->Direction == Direction && Node.IsParameterMapPin(Pin))
            {
                return Pin;
            }
        }
        return nullptr;
    }

    // Reads the first affected script that carries the value. The editor writes them all identically.
    bool ReadRapidIterationValue(const FNiagaraStackStage& Stage, const FNiagaraStackInput& Input, FNiagaraVariable& OutValue)
    {
        const FNiagaraVariable Parameter = NiagaraStackJson::MakeRapidIterationVariable(Stage, Input);
        TArray<UNiagaraScript*> Scripts;
        NiagaraStackJson::GetAffectedScripts(Stage.System, Stage, Scripts);
        for (UNiagaraScript* Script : Scripts)
        {
            const uint8* Data = Script ? Script->RapidIterationParameters.GetParameterData(Parameter) : nullptr;
            if (Data)
            {
                OutValue = Parameter;
                OutValue.SetData(Data);
                return true;
            }
        }
        return false;
    }

    UEdGraphPin* GetLinkedPin(UEdGraphPin* OverridePin)
    {
        return OverridePin && OverridePin->LinkedTo.Num() == 1 ? OverridePin->LinkedTo[0] : nullptr;
    }

    // Scalars, bools and enums read naturally as pin text, everything else goes through the struct text.
    bool UsesPinDefaultText(const FNiagaraTypeDefinition& Type)
    {
        return Type == FNiagaraTypeDefinition::GetFloatDef() || Type == FNiagaraTypeDefinition::GetIntDef() || Type == FNiagaraTypeDefinition::GetBoolDef() || Type.IsEnum();
    }

    // Renderer bindings carry cached resolution fields that differ from the CDO even when the bound attribute does not.
    bool IsUnchangedAttributeBinding(const FProperty* Prop, const void* Object, const void* DefaultObject)
    {
        const FStructProperty* StructProp = CastField<FStructProperty>(Prop);
        const FProperty* Bound = StructProp ? StructProp->Struct->FindPropertyByName(TEXT("ParamMapVariable")) : nullptr;
        return Bound && Bound->Identical_InContainer(StructProp->ContainerPtrToValuePtr<void>(Object), StructProp->ContainerPtrToValuePtr<void>(DefaultObject));
    }
}

void NiagaraStackJson::CollectStages(UNiagaraSystem* System, const FNiagaraEmitterHandle* EmitterHandle, TArray<FNiagaraStackStage>& OutStages)
{
    if (!System)
    {
        return;
    }

    if (!EmitterHandle)
    {
        UNiagaraScript* SpawnScript = System->GetSystemSpawnScript();
        UNiagaraGraph* Graph = SpawnScript ? GetSourceGraph(SpawnScript->GetLatestSource()) : nullptr;
        AddStage(OutStages, System, nullptr, Graph, TEXT("SystemSpawn"), ENiagaraScriptUsage::SystemSpawnScript);
        AddStage(OutStages, System, nullptr, Graph, TEXT("SystemUpdate"), ENiagaraScriptUsage::SystemUpdateScript);
        return;
    }

    CollectEmitterStages(System, *EmitterHandle, OutStages);
}

UEdGraphPin* NiagaraStackJson::GetParameterMapInputPin(UNiagaraNode& Node)
{
    return FindParameterMapPin(Node, EGPD_Input);
}

UEdGraphPin* NiagaraStackJson::GetParameterMapOutputPin(UNiagaraNode& Node)
{
    return FindParameterMapPin(Node, EGPD_Output);
}

void NiagaraStackJson::GetOrderedModules(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModules)
{
    UNiagaraNode* Current = &OutputNode;
    while (Current)
    {
        UEdGraphPin* InputPin = GetParameterMapInputPin(*Current);
        if (!InputPin || InputPin->LinkedTo.Num() != 1)
        {
            break;
        }

        Current = Cast<UNiagaraNode>(InputPin->LinkedTo[0]->GetOwningNode());
        if (UNiagaraNodeFunctionCall* Module = Cast<UNiagaraNodeFunctionCall>(Current))
        {
            OutModules.Insert(Module, 0);
        }
    }
}

UNiagaraNode* NiagaraStackJson::FindStageStartNode(UNiagaraNodeOutput& OutputNode)
{
    UNiagaraNode* Current = &OutputNode;
    while (true)
    {
        UEdGraphPin* InputPin = GetParameterMapInputPin(*Current);
        UNiagaraNode* Previous = InputPin && InputPin->LinkedTo.Num() == 1 ? Cast<UNiagaraNode>(InputPin->LinkedTo[0]->GetOwningNode()) : nullptr;
        if (!Previous)
        {
            return Current;
        }
        Current = Previous;
    }
}

void NiagaraStackJson::CollectStageNodes(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNode*>& OutNodes)
{
    TArray<UNiagaraNode*> Pending;
    Pending.Add(&OutputNode);
    while (Pending.Num() > 0)
    {
        UNiagaraNode* Node = Pending.Pop(EAllowShrinking::No);
        if (!Node || OutNodes.Contains(Node))
        {
            continue;
        }
        OutNodes.Add(Node);
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input)
            {
                for (UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    Pending.Add(Linked ? Cast<UNiagaraNode>(Linked->GetOwningNode()) : nullptr);
                }
            }
        }
    }
}

void NiagaraStackJson::CollectInputs(const FNiagaraStackStage& Stage, UNiagaraNodeFunctionCall& FunctionCall, TArray<FNiagaraStackInput>& OutInputs)
{
    TArray<FNiagaraVariable> Variables;
    FNiagaraStackGraphUtilities::GetStackFunctionInputs(FunctionCall, Variables, MakeResolver(Stage), FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);

    for (const FNiagaraVariable& Variable : Variables)
    {
        const FNiagaraParameterHandle Handle(Variable.GetName());
        const FNiagaraTypeDefinition& Type = Variable.GetType();

        FNiagaraStackInput& Input = OutInputs.AddDefaulted_GetRef();
        Input.Variable = Variable;
        Input.AliasedName = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(Handle, &FunctionCall).GetParameterHandleString();
        Input.DisplayName = Handle.GetName().ToString();
        // Mirrors FNiagaraStackGraphUtilities::IsRapidIterationType, which is not exported. Bools and enums live on override pins.
        Input.bRapidIteration = Type.IsStatic() || (Type != FNiagaraTypeDefinition::GetBoolDef() && !Type.IsEnum() && Type != FNiagaraTypeDefinition::GetParameterMapDef() && !Type.IsUObject());
    }
}

void NiagaraStackJson::CollectStaticSwitchPins(const FNiagaraStackStage& Stage, UNiagaraNodeFunctionCall& FunctionCall, TArray<UEdGraphPin*>& OutPins)
{
    TArray<UEdGraphPin*> Pins;
    TSet<UEdGraphPin*> HiddenPins;
    FCompileConstantResolver Resolver = MakeResolver(Stage);
    FNiagaraStackGraphUtilities::GetStackFunctionStaticSwitchPins(FunctionCall, Pins, HiddenPins, Resolver);
    for (UEdGraphPin* Pin : Pins)
    {
        if (Pin && !HiddenPins.Contains(Pin))
        {
            OutPins.Add(Pin);
        }
    }
}

UNiagaraNode* NiagaraStackJson::FindOverrideNode(UNiagaraNodeFunctionCall& FunctionCall)
{
    UEdGraphPin* InputPin = GetParameterMapInputPin(FunctionCall);
    UNiagaraNode* Linked = InputPin && InputPin->LinkedTo.Num() == 1 ? Cast<UNiagaraNode>(InputPin->LinkedTo[0]->GetOwningNode()) : nullptr;
    return IsNodeOfClass(Linked, TEXT("NiagaraNodeParameterMapSet")) ? Linked : nullptr;
}

UEdGraphPin* NiagaraStackJson::FindOverridePin(UNiagaraNodeFunctionCall& FunctionCall, FName AliasedName)
{
    UNiagaraNode* OverrideNode = FindOverrideNode(FunctionCall);
    if (!OverrideNode)
    {
        return nullptr;
    }
    for (UEdGraphPin* Pin : OverrideNode->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Input && Pin->GetFName() == AliasedName)
        {
            return Pin;
        }
    }
    return nullptr;
}

FNiagaraVariable NiagaraStackJson::MakeRapidIterationVariable(const FNiagaraStackStage& Stage, const FNiagaraStackInput& Input)
{
    const FNiagaraVariable Aliased(Input.Variable.GetType(), Input.AliasedName);
    if (Stage.IsSystemStage())
    {
        return FNiagaraUtilities::ConvertVariableToRapidIterationConstantName(Aliased, nullptr, Stage.Usage);
    }
    const FString EmitterName = Stage.EmitterHandle->GetUniqueInstanceName();
    return FNiagaraUtilities::ConvertVariableToRapidIterationConstantName(Aliased, *EmitterName, Stage.Usage);
}

void NiagaraStackJson::GetAffectedScripts(UNiagaraSystem* System, const FNiagaraStackStage& Stage, TArray<UNiagaraScript*>& OutScripts)
{
    if (System)
    {
        OutScripts.Add(System->GetSystemSpawnScript());
        OutScripts.Add(System->GetSystemUpdateScript());
    }

    FVersionedNiagaraEmitterData* EmitterData = Stage.EmitterHandle ? Stage.EmitterHandle->GetEmitterData() : nullptr;
    if (!EmitterData)
    {
        return;
    }

    TArray<UNiagaraScript*> Scripts;
    EmitterData->GetScripts(Scripts, false);
    for (UNiagaraScript* Script : Scripts)
    {
        if (!Script)
        {
            continue;
        }
        if (Stage.Usage == ENiagaraScriptUsage::ParticleEventScript)
        {
            if (Script->GetUsage() == ENiagaraScriptUsage::ParticleEventScript && Script->GetUsageId() == Stage.UsageId)
            {
                OutScripts.Add(Script);
            }
        }
        else if (Script->ContainsUsage(Stage.Usage))
        {
            OutScripts.Add(Script);
        }
    }
}

bool NiagaraStackJson::ReadLocalValue(const FNiagaraStackStage& Stage, UNiagaraNodeFunctionCall& FunctionCall, const FNiagaraStackInput& Input, FString& OutText)
{
    if (UEdGraphPin* OverridePin = FindOverridePin(FunctionCall, Input.AliasedName))
    {
        if (OverridePin->LinkedTo.Num() > 0)
        {
            return false;
        }
        OutText = OverridePin->DefaultValue;
        return true;
    }

    FNiagaraVariable Value;
    if (Input.bRapidIteration && ReadRapidIterationValue(Stage, Input, Value))
    {
        OutText = ValueToString(Value);
        return true;
    }
    return false;
}

ENiagaraStackInputMode NiagaraStackJson::ResolveInputMode(const FNiagaraStackStage& Stage, UNiagaraNodeFunctionCall& FunctionCall, const FNiagaraStackInput& Input)
{
    UEdGraphPin* OverridePin = FindOverridePin(FunctionCall, Input.AliasedName);
    if (OverridePin)
    {
        UEdGraphPin* Linked = GetLinkedPin(OverridePin);
        if (!Linked)
        {
            return ENiagaraStackInputMode::Local;
        }

        UEdGraphNode* LinkedNode = Linked->GetOwningNode();
        if (Cast<UNiagaraNodeCustomHlsl>(LinkedNode))
        {
            return ENiagaraStackInputMode::Expression;
        }
        if (Cast<UNiagaraNodeFunctionCall>(LinkedNode))
        {
            return ENiagaraStackInputMode::DynamicInput;
        }
        if (const UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(LinkedNode))
        {
            if (GetDataInterface(InputNode))
            {
                return ENiagaraStackInputMode::DataInterface;
            }
            if (GetObjectAsset(InputNode))
            {
                return ENiagaraStackInputMode::ObjectAsset;
            }
        }
        return ENiagaraStackInputMode::Linked;
    }

    FNiagaraVariable Value;
    if (Input.bRapidIteration && ReadRapidIterationValue(Stage, Input, Value))
    {
        return ENiagaraStackInputMode::Local;
    }
    return ENiagaraStackInputMode::Default;
}

const TCHAR* NiagaraStackJson::InputModeName(ENiagaraStackInputMode Mode)
{
    switch (Mode)
    {
    case ENiagaraStackInputMode::Local:         return TEXT("Local");
    case ENiagaraStackInputMode::Linked:        return TEXT("Linked");
    case ENiagaraStackInputMode::DynamicInput:  return TEXT("DynamicInput");
    case ENiagaraStackInputMode::DataInterface: return TEXT("DataInterface");
    case ENiagaraStackInputMode::ObjectAsset:   return TEXT("ObjectAsset");
    case ENiagaraStackInputMode::Expression:    return TEXT("Expression");
    default:                                    return TEXT("Default");
    }
}

FString NiagaraStackJson::ValueToString(const FNiagaraVariable& AllocatedVariable)
{
    const FNiagaraTypeDefinition Unstatic = AllocatedVariable.GetType().RemoveStaticDef();

    // Vector pin defaults round to three decimals and matrices have none, the struct text keeps full precision.
    UScriptStruct* Struct = Unstatic.GetScriptStruct();
    if (Struct && !UsesPinDefaultText(Unstatic))
    {
        FString Text;
        Struct->ExportText(Text, AllocatedVariable.GetData(), nullptr, nullptr, PPF_None, nullptr);
        return Text;
    }

    TSharedPtr<INiagaraEditorTypeUtilities, ESPMode::ThreadSafe> TypeUtilities = FNiagaraEditorModule::Get().GetTypeUtilities(Unstatic);
    if (TypeUtilities.IsValid() && TypeUtilities->CanHandlePinDefaults())
    {
        FNiagaraVariable UnstaticVariable(Unstatic, AllocatedVariable.GetName());
        UnstaticVariable.SetData(AllocatedVariable.GetData());
        return TypeUtilities->GetPinDefaultStringFromValue(UnstaticVariable);
    }

    FString PinDefault;
    GetDefault<UEdGraphSchema_Niagara>()->TryGetPinDefaultValueFromNiagaraVariable(AllocatedVariable, PinDefault);
    return PinDefault;
}

bool NiagaraStackJson::StringToValue(const FString& Text, FNiagaraVariable& InOutVariable)
{
    const FNiagaraTypeDefinition Unstatic = InOutVariable.GetType().RemoveStaticDef();

    // Struct text first, pin default text as the fallback, so both the export form and the editor's pin form parse.
    UScriptStruct* Struct = Unstatic.GetScriptStruct();
    if (Struct && !UsesPinDefaultText(Unstatic))
    {
        TArray<uint8> Buffer;
        Buffer.SetNumZeroed(Struct->GetStructureSize());
        Struct->InitializeStruct(Buffer.GetData());
        const TCHAR* End = Struct->ImportText(*Text, Buffer.GetData(), nullptr, PPF_None, GWarn, Struct->GetName());
        const bool bParsed = End != nullptr;
        if (bParsed)
        {
            InOutVariable.SetData(Buffer.GetData());
        }
        Struct->DestroyStruct(Buffer.GetData());
        if (bParsed)
        {
            return true;
        }
    }

    TSharedPtr<INiagaraEditorTypeUtilities, ESPMode::ThreadSafe> TypeUtilities = FNiagaraEditorModule::Get().GetTypeUtilities(Unstatic);
    if (!TypeUtilities.IsValid() || !TypeUtilities->CanHandlePinDefaults())
    {
        return false;
    }

    FNiagaraVariable Parsed(Unstatic, InOutVariable.GetName());
    Parsed.AllocateData();
    if (!TypeUtilities->SetValueFromPinDefaultString(Text, Parsed))
    {
        return false;
    }
    InOutVariable.SetData(Parsed.GetData());
    return true;
}

UNiagaraDataInterface* NiagaraStackJson::GetDataInterface(const UNiagaraNode* InputNode)
{
    // UNiagaraNodeInput::GetDataInterface is not exported from NiagaraEditor, read the UPROPERTY via reflection.
    static FObjectPropertyBase* Property = CastField<FObjectPropertyBase>(UNiagaraNodeInput::StaticClass()->FindPropertyByName(TEXT("DataInterface")));
    return Property && Cast<UNiagaraNodeInput>(InputNode) ? Cast<UNiagaraDataInterface>(Property->GetObjectPropertyValue_InContainer(InputNode)) : nullptr;
}

UObject* NiagaraStackJson::GetObjectAsset(const UNiagaraNode* InputNode)
{
    static FObjectPropertyBase* Property = CastField<FObjectPropertyBase>(UNiagaraNodeInput::StaticClass()->FindPropertyByName(TEXT("ObjectAsset")));
    return Property && Cast<UNiagaraNodeInput>(InputNode) ? Property->GetObjectPropertyValue_InContainer(InputNode) : nullptr;
}

bool NiagaraStackJson::IsNodeOfClass(const UObject* Node, const TCHAR* ClassName)
{
    for (const UClass* Class = Node ? Node->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
    {
        if (Class->GetFName() == ClassName)
        {
            return true;
        }
    }
    return false;
}

FString NiagaraStackJson::DescribeStaticSwitchValue(const UEdGraphPin* Pin)
{
    FString Value = Pin->DefaultValue;
    const UEnum* Enum = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin).GetEnum();
    if (!Enum)
    {
        return Value;
    }

    // User-defined enums store the enumerator's internal name ("NewEnumerator2"), native enums an integer.
    int32 Index = Value.IsNumeric() ? Enum->GetIndexByValue(FCString::Atoi64(*Value)) : Enum->GetIndexByNameString(Value);
    return Index != INDEX_NONE ? Enum->GetDisplayNameTextByIndex(Index).ToString() : Value;
}

bool NiagaraStackJson::ResolveStaticSwitchValue(const UEdGraphPin* Pin, const FString& Text, FString& OutPinValue)
{
    const UEnum* Enum = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin).GetEnum();
    if (!Enum)
    {
        OutPinValue = Text;
        return true;
    }

    const bool bNumericStorage = Pin->DefaultValue.IsNumeric();
    for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
    {
        const bool bNameHit = Enum->GetNameStringByIndex(Index) == Text || Enum->GetAuthoredNameStringByIndex(Index) == Text;
        const bool bDisplayHit = Enum->GetDisplayNameTextByIndex(Index).ToString() == Text;
        if (bNameHit || bDisplayHit)
        {
            OutPinValue = bNumericStorage ? FString::Printf(TEXT("%lld"), Enum->GetValueByIndex(Index)) : Enum->GetNameStringByIndex(Index);
            return true;
        }
    }
    return false;
}

TSharedPtr<FJsonObject> NiagaraStackJson::ExportPropertyDelta(UObject* Object, UClass* StopAtClass, const TSet<FName>& SkippedProperties)
{
    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
    const UObject* DefaultObject = Object->GetClass()->GetDefaultObject();

    for (UClass* Class = Object->GetClass(); Class && Class != StopAtClass; Class = Class->GetSuperClass())
    {
        for (TFieldIterator<FProperty> PropIt(Class, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            const bool bSkipped = SkippedProperties.Contains(Prop->GetFName()) || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_InstancedReference | CPF_ContainsInstancedReference);
            if (bSkipped || Prop->Identical_InContainer(Object, DefaultObject) || IsUnchangedAttributeBinding(Prop, Object, DefaultObject))
            {
                continue;
            }

            FString Value;
            Prop->ExportTextItem_Direct(Value, Prop->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
            Props->SetStringField(Prop->GetName(), Value);
        }
    }
    return Props;
}

TSharedPtr<FJsonObject> NiagaraStackJson::ExportStructDelta(const UScriptStruct* Struct, const void* Data, UObject* Owner, const TSet<FName>& SkippedProperties)
{
    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();

    TArray<uint8> DefaultData;
    DefaultData.SetNumZeroed(Struct->GetStructureSize());
    Struct->InitializeDefaultValue(DefaultData.GetData());

    for (TFieldIterator<FProperty> PropIt(Struct); PropIt; ++PropIt)
    {
        FProperty* Prop = *PropIt;
        const bool bSkipped = SkippedProperties.Contains(Prop->GetFName()) || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_InstancedReference | CPF_ContainsInstancedReference);
        if (bSkipped || Prop->Identical_InContainer(Data, DefaultData.GetData()))
        {
            continue;
        }

        FString Value;
        Prop->ExportTextItem_Direct(Value, Prop->ContainerPtrToValuePtr<void>(Data), nullptr, Owner, PPF_None);
        Props->SetStringField(Prop->GetName(), Value);
    }

    Struct->DestroyStruct(DefaultData.GetData());
    return Props;
}

TSharedPtr<FJsonObject> NiagaraStackJson::SerializeDataInterface(UNiagaraDataInterface* DataInterface)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("Class"), DataInterface->GetClass()->GetPathName());
    // The curve lookup table is derived from the keys and rebuilt on load.
    static const TSet<FName> Derived = { TEXT("ShaderLUT"), TEXT("LUTMinTime"), TEXT("LUTMaxTime"), TEXT("LUTInvTimeRange"), TEXT("LUTNumSamplesMinusOne") };
    TSharedPtr<FJsonObject> Properties = ExportPropertyDelta(DataInterface, UNiagaraDataInterface::StaticClass(), Derived);
    if (Properties.IsValid() && Properties->Values.Num() > 0)
    {
        Obj->SetObjectField(TEXT("Properties"), Properties);
    }
    return Obj;
}

TSharedPtr<FJsonObject> NiagaraStackJson::SerializeFunctionCall(const FNiagaraStackStage& Stage, UNiagaraNodeFunctionCall& FunctionCall)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("Name"), FunctionCall.GetFunctionName());
    if (FunctionCall.FunctionScript)
    {
        Obj->SetStringField(TEXT("Script"), FunctionCall.FunctionScript->GetPathName());
    }
    Obj->SetBoolField(TEXT("Enabled"), FunctionCall.IsNodeEnabled());

    TArray<UEdGraphPin*> SwitchPins;
    CollectStaticSwitchPins(Stage, FunctionCall, SwitchPins);
    if (SwitchPins.Num() > 0)
    {
        TSharedPtr<FJsonObject> Switches = MakeShared<FJsonObject>();
        for (const UEdGraphPin* Pin : SwitchPins)
        {
            Switches->SetStringField(Pin->GetName(), DescribeStaticSwitchValue(Pin));
        }
        Obj->SetObjectField(TEXT("StaticSwitches"), Switches);
    }

    TArray<FNiagaraStackInput> Inputs;
    CollectInputs(Stage, FunctionCall, Inputs);

    TSharedPtr<FJsonObject> InputsObj = MakeShared<FJsonObject>();
    for (const FNiagaraStackInput& Input : Inputs)
    {
        const ENiagaraStackInputMode Mode = ResolveInputMode(Stage, FunctionCall, Input);

        TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
        InputObj->SetStringField(TEXT("Type"), Input.Variable.GetType().GetName());
        InputObj->SetStringField(TEXT("Mode"), InputModeName(Mode));

        UEdGraphPin* OverridePin = FindOverridePin(FunctionCall, Input.AliasedName);
        UEdGraphPin* Linked = GetLinkedPin(OverridePin);
        UEdGraphNode* LinkedNode = Linked ? Linked->GetOwningNode() : nullptr;

        switch (Mode)
        {
        case ENiagaraStackInputMode::Local:
        {
            FString Value;
            if (ReadLocalValue(Stage, FunctionCall, Input, Value))
            {
                InputObj->SetStringField(TEXT("Value"), Value);
            }
            break;
        }
        case ENiagaraStackInputMode::Linked:
            InputObj->SetStringField(TEXT("Link"), Linked->GetName());
            break;
        case ENiagaraStackInputMode::DynamicInput:
            InputObj->SetObjectField(TEXT("DynamicInput"), SerializeFunctionCall(Stage, *CastChecked<UNiagaraNodeFunctionCall>(LinkedNode)));
            break;
        case ENiagaraStackInputMode::DataInterface:
            InputObj->SetObjectField(TEXT("DataInterface"), SerializeDataInterface(GetDataInterface(Cast<UNiagaraNode>(LinkedNode))));
            break;
        case ENiagaraStackInputMode::ObjectAsset:
            InputObj->SetStringField(TEXT("Object"), GetObjectAsset(Cast<UNiagaraNode>(LinkedNode))->GetPathName());
            break;
        case ENiagaraStackInputMode::Expression:
            InputObj->SetStringField(TEXT("Expression"), UAssetWorkbench::ReadPropertyPathText(LinkedNode, TEXT("CustomHlsl")));
            break;
        default:
            break;
        }

        InputsObj->SetObjectField(Input.DisplayName, InputObj);
    }
    if (InputsObj->Values.Num() > 0)
    {
        Obj->SetObjectField(TEXT("Inputs"), InputsObj);
    }

    return Obj;
}

TSharedPtr<FJsonObject> NiagaraStackJson::SerializeStage(const FNiagaraStackStage& Stage)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("Name"), Stage.Name);

    TArray<UNiagaraNodeFunctionCall*> Modules;
    GetOrderedModules(*Stage.OutputNode, Modules);

    TArray<TSharedPtr<FJsonValue>> ModulesArray;
    for (UNiagaraNodeFunctionCall* Module : Modules)
    {
        ModulesArray.Add(MakeShared<FJsonValueObject>(SerializeFunctionCall(Stage, *Module)));
    }
    Obj->SetArrayField(TEXT("Modules"), ModulesArray);
    return Obj;
}
