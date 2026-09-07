# uasset-workbench

**English** | [中文](README_CN.md)

![Claude Code](https://img.shields.io/badge/Claude_Code-black?style=flat&logo=anthropic&logoColor=white)
![Unreal Engine 5](https://img.shields.io/badge/Unreal_Engine-5.7-blue?logo=unrealengine&logoColor=white)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)

An editor plugin that lets scripts and AI agents operate on Unreal Engine 5 uassets non-interactively.

## What problem it solves

| Problem | How it shows up |
| --- | --- |
| Can't read | Logic and configuration are locked inside binary `.uasset` files, and an AI handed the file has nowhere to start. An EventGraph with hundreds of nodes has no readable text form, and abandoned variables, broken connections, wrong defaults are impossible to fully audit by eye in the editor. Montage notify timing, UMG hierarchy and keyframes, Niagara and material parameters, DataTable values, level actor placement and streaming config all exist only in the editor UI |
| Can't write | Changing a UMG layout means dragging by hand in the editor, there is no version-controllable, replayable write path |
| Can't change what exists | Adding a component to a Blueprint, wiring it into the graph and setting its default are three separate hand edits, and doing that across many Blueprints means doing it many times. An animation state machine is the same story one level up, states, conduits, transitions and their rule graphs are all mouse work |
| Can't fix after a rename | After a C++ or asset rename, CoreRedirects only fixes the call side. It cannot fix the implementation side or the consumer side inside Blueprint graphs, nor another level's import of the old path |
| Can't audit | Broken references, level streaming topology, per-level component budget, texture compression and sRGB settings, material usage flags and Nanite compatibility have no batch query entry point |

Five problems, five capability groups.

## The five groups

| Group | What it does | Output | Count |
| --- | --- | --- | --- |
| Export | Read uasset structure, write JSON | JSON under `Intermediate/UAssetExport` | 11 |
| Import | Read a JSON spec, write back to or create uassets | modified or new uassets | 3 |
| Edit | Change an existing uasset to match an intent | modified uassets | 4 |
| Migrate | Fix references after a C++ or asset rename | modified uassets | 8 |
| Audit | Read-only checks producing a report | report JSON | 4 |

The group is decided by the run name: suffix `Export` is the Export group, suffix `Import` and prefix `Create` are the Import group, prefix `Edit` is the Edit group, prefix `Audit` is the Audit group, everything else is Migrate. Naming-wise `Import` and `Export` are nouns used as suffixes, every other verb leads.

## Routing and architecture

Every operation converges on the same calling contract. The heartbeat file `Saved/UAssetWorkbenchTaskQueue/.alive` decides the route by mtime, falling back to the editor process id it carries when a synchronous stall leaves the mtime stale.

| Editor state | Which path | Feedback |
| --- | --- | --- |
| Open | Write a pending task, the in-editor subsystem runs it in-process | One Message Log page per run, a toast when that run reports warnings or errors |
| Closed | Launch `UnrealEditor-Cmd` to run the commandlet | log |

The read-only groups, Export and Audit, produce identical output on either path. The write groups, Import, Edit and Migrate, must run with the editor open. A package saved from a standalone commandlet process lands without the imports the asset registry builds its dependency graph from. The asset itself is fine, it opens and runs, but Reference Viewer shows it isolated until something saves it again from inside the editor. [Docs/Migrate.md](Docs/Migrate.md) carries the detail and the recovery step.

What the routing buys: the five groups share one call entry and one output convention, so workflows compose, export the structure, analyze offline, write back to the asset, audit to verify, instead of every added tool bringing its own way of being called. Adding a commandlet adds a capability, the contract does not change.

## Calling contract

Every commandlet returns one of four codes, from a single enum, and the in-editor queue passes the code through unchanged.

| Code | Meaning |
| --- | --- |
| 0 | Success |
| 1 | Failure, bad arguments, an asset that would not load, a value that would not write |
| 2 | The editor is running, the commandlet path steps aside rather than competing with it |
| 3 | The run itself succeeded and the report has findings, Audit group only |

3 is not a failure, which is what makes an audit usable as a commit gate.

Writes are all-or-nothing per target. A writer that fails aborts its whole target before anything reaches disk, and a Blueprint that fails to compile is never saved. Nodes the plugin creates carry `RF_Transactional`, so an edited asset opens in the editor without the "requires resave" toast.

## Quick start

Wrapper: `src/scripts/run_commandlet.sh`, which sits at `Plugins/UAssetWorkbench/scripts/` once integrated into a project.

```
run_commandlet.sh <UE_PATH> <UPROJECT> <RunName> <AssetList> [IDLE_SEC] [MAX_SEC] [EXTRA_ARGS]
```

| Argument | Meaning |
| --- | --- |
| `UE_PATH` | Engine install root |
| `UPROJECT` | Absolute path to the `.uproject` |
| `RunName` | The commandlet's run name |
| `AssetList` | Comma-separated asset paths, spec-driven runs pass an empty string |
| `IDLE_SEC` | Output mtime quiet seconds the Export group uses to decide completion, default 10 |
| `MAX_SEC` | Total wait cap, default 600 |
| `EXTRA_ARGS` | Passed through to the commandlet |

Under Git Bash prefix with `MSYS_NO_PATHCONV=1`, otherwise `/Game/...` gets rewritten into a Windows path.

Export.

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    BlueprintEdGraphExport "/Game/Blueprints/BP_Foo"
```

Import.

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    WidgetLayoutImport "/Game/UI/WBP_Foo" 10 600 \
    '-spec="C:/temp/WBP_Foo.spec.json"'
```

Edit.

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    EditBlueprint "" 10 600 \
    '-spec="C:/temp/BP_Foo.edit.json" -apply'
```

Migrate.

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    RedirectBlueprintEvent "/Game/Blueprints/BP_Foo" 10 600 \
    '-OwnerClass="/Script/MyModule.MyActor" -OldEvent="OnPickedUp" -NewEvent="HandlePickedUp"'
```

Audit.

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    AuditTexture "" 10 600 \
    '-scandir="/Game"'
```

## Groups in detail

<details>
<summary><b>Export</b>, 11 commandlets</summary>

| RunName | What it exports |
| --- | --- |
| `BlueprintEdGraphExport` | Blueprint graphs, nodes, pins, connections, function signatures, variables, dispatchers, timelines, components, referenced assets |
| `AnimAssetExport` | AnimSequence and AnimMontage notifies, curves, track names, root motion, sync markers on a sequence, sections and slots on a montage |
| `WidgetLayoutExport` | Widget tree, slot layout properties, subclass properties, animation keyframes, EdGraph |
| `DataAssetExport` | All custom properties of DataAsset subclasses, array elements expanded |
| `DataTableExport` | DataTable row struct name and all row data, indexed by RowName |
| `NiagaraSystemExport` | Niagara emitter list, spawn/update script parameters, renderer properties |
| `MaterialExport` | Material expression connection chain and global settings, MaterialInstance parameter overrides |
| `TextureExport` | Texture properties, compression, sRGB, LOD group, mips, source dimensions |
| `BehaviorTreeExport` | BT tree structure, node parameters, Blackboard keys |
| `AnimBlueprintExport` | AnimBP EdGraph, state machines, states, transitions, blend settings, entry state, event bindings |
| `LevelExport` | Level actors / components, delta-from-archetype properties, collision / static mesh / ISM summary, streaming levels |

Output: `Intermediate/UAssetExport/<AssetPath>_r<revision>_<YYYYMMDD-HHMMSS>.json`, out of version control.

`revision` is the asset's last-changed revision in version control. The stamped name is written only after the export succeeds, so two exports of the same asset never overwrite each other and a stale export is never mistaken for a fresh one. Read the newest.

Every JSON carries the two common fields `ExporterVersion` and `ExportType`, the rest expands per asset type.

One EdGraph serializer backs `BlueprintEdGraphExport`, `AnimBlueprintExport` and `WidgetLayoutExport`, so the graph, node and pin keys are identical across all three and the same graph exported through two commandlets diffs clean.

| Addition | What it gives the reader |
| --- | --- |
| `Signature` on every function-class graph | Inputs, outputs, local variables, access, flags, in the exact shape `EditBlueprint` takes back as a spec |
| Variable metadata, `EventDispatchers[]`, `Timelines[]` | The parts of a Blueprint that live outside the graphs |
| Anim node `Settings` / `Bindings` / `ExposedPins` | What the Details panel shows, not only what the pins show |
| `RuleSummary` per transition | One line saying what lets that transition fire, so a state machine reads without expanding every rule graph |
| AnimAsset curves, sync markers, track names, root motion, montage blend | Timing data that used to exist only in the editor timeline |
| Texture and material property blocks | The build settings the Edit and Audit groups act on |

<details>
<summary>Blueprint EdGraph</summary>

```json
{
    "ExporterVersion": "2.5.3",
    "ExportType": "BlueprintEdGraph",
    "Blueprint": "BP_Foo",
    "ParentClass": "PlayerController",
    "Variables": [
        { "Name": "DebugTimerHandle", "Type": "FTimerHandle" }
    ],
    "Graphs": [
        {
            "GraphType": "EventGraph",
            "Nodes": [
                {
                    "NodeId": "630182DA4D53F4141AD5B792F2AA8565",
                    "Class": "K2Node_CallFunction",
                    "Title": "Open Level (by Object Reference)",
                    "FunctionName": "OpenLevelBySoftObjectPtr",
                    "Pins": [ ... ]
                }
            ]
        }
    ]
}
```
</details>

<details>
<summary>AnimAsset</summary>

```json
{
    "ExporterVersion": "2.5.3",
    "ExportType": "AnimMontage",
    "AssetName": "AM_Foo_Attack_01",
    "SequenceLength": 0.543,
    "Sections": [
        { "Name": "Default", "StartTime": 0 }
    ],
    "SlotTracks": [
        {
            "SlotName": "DefaultSlot",
            "Segments": [
                { "AnimSequence": "AS_Foo_Attack_01", "AnimPlayRate": 2 }
            ]
        }
    ],
    "Curves": [
        {
            "Name": "Windup",
            "Flags": [ "Editable" ],
            "KeyCount": 2,
            "Keys": [ { "Time": 0.0, "Value": 0.0 }, { "Time": 0.5, "Value": 1.0 } ]
        }
    ],
    "Notifies": [
        {
            "NotifyName": "ANS_Example",
            "TriggerTime": 0.0001,
            "Duration": 0.122,
            "TrackIndex": 0,
            "IsState": true,
            "NotifyClass": "AnimNotifyState_Example",
            "Parameters": {
                "Speed": "2000.000000",
                "Curve": "/Script/Engine.CurveFloat'.../Falloff.Falloff'"
            }
        }
    ]
}
```

`Parameters` goes straight back into `EditAnimAsset` once edited.
</details>

<details>
<summary>AnimBlueprint state machine</summary>

```json
{
    "ExporterVersion": "2.5.3",
    "ExportType": "AnimBlueprint",
    "StateMachines": [
        {
            "StateMachineName": "Locomotion",
            "EntryState": "Idle",
            "States": [
                {
                    "StateName": "Idle",
                    "StateType": "AST_SingleState",
                    "Events": { "UpdateFunction": "TickIdle" }
                },
                { "StateName": "Airborne", "StateType": "Conduit" }
            ],
            "Transitions": [
                {
                    "FromState": "Idle",
                    "ToState": "Walk",
                    "CrossfadeDuration": 0.2,
                    "LogicType": "TLT_Inertialization",
                    "RuleSummary": {
                        "Getter": "GetRelevantAnimTimeRemainingFraction",
                        "State": "Idle",
                        "Compare": "Less",
                        "Threshold": "0.1"
                    }
                }
            ]
        }
    ]
}
```

The transition keys are the ones `EditBlueprint` reads under `StateMachines`, so an exported transition pastes back as a spec.
</details>

<details>
<summary>Widget Layout</summary>

```json
{
    "ExporterVersion": "2.5.3",
    "ExportType": "WidgetLayout",
    "WidgetBlueprint": "WBP_Foo",
    "WidgetTree": {
        "Name": "CanvasPanel_36",
        "Class": "CanvasPanel",
        "Visibility": "SelfHitTestInvisible",
        "Children": [
            {
                "Name": "Tint",
                "Class": "Image",
                "Properties": {
                    "Brush": "(TintColor=...)"
                },
                "Slot": {
                    "SlotClass": "CanvasPanelSlot",
                    "LayoutData": "(Anchors=(Minimum=(X=0,Y=0),Maximum=(X=1,Y=1)))"
                }
            }
        ]
    },
    "Animations": [ ... ],
    "Graphs": [ ... ]
}
```
</details>

<details>
<summary>DataTable</summary>

```json
{
    "ExporterVersion": "2.5.3",
    "ExportType": "DataTable",
    "DataTableName": "DT_Foo",
    "RowStruct": "AttributeMetaData",
    "RowCount": 5,
    "Rows": {
        "AttributeSet.Health": {
            "BaseValue": "100.000000",
            "MinValue": "0.000000",
            "MaxValue": "1.000000",
            "bCanStack": "False"
        },
        "AttributeSet.Mana": {
            "BaseValue": "0.000000",
            "MinValue": "0.000000",
            "MaxValue": "1.000000"
        }
    }
}
```
</details>

<details>
<summary>Material</summary>

```json
{
    "ExporterVersion": "2.5.3",
    "ExportType": "Material",
    "MaterialName": "M_Foo",
    "ShadingModel": "MSM_DefaultLit",
    "BlendMode": "BLEND_Translucent",
    "TwoSided": false,
    "MaterialDomain": "MD_Surface",
    "bAutomaticallySetUsageInEditor": true,
    "UsageFlags": [ "bUsedWithSkeletalMesh", "bUsedWithNanite" ],
    "Expressions": [
        {
            "Class": "MaterialExpressionMaterialFunctionCall",
            "Description": "MaterialFunctionCall (FlipBook)",
            "Inputs": [
                {
                    "InputName": "Animation Phase (0-1) (S)",
                    "ConnectedTo": "MaterialExpressionFrac_0"
                }
            ]
        }
    ],
    "OutputConnections": {
        "EmissiveColor": { "Expression": "MaterialExpressionMultiply_0" },
        "Opacity": { "Expression": "MaterialExpressionMultiply_1" }
    }
}
```

MaterialInstance exports the parameter override table.

```json
{
    "ExportType": "MaterialInstance",
    "Parent": "M_Base",
    "ScalarParameters": [
        { "Name": "Roughness", "Value": 0.8 }
    ],
    "VectorParameters": [
        { "Name": "BaseColor", "Value": "(R=0.5,G=0.1,B=0.1,A=1.0)" }
    ],
    "TextureParameters": [
        { "Name": "Albedo", "Texture": "T_Stone_D", "TexturePath": "/Game/Textures/T_Stone_D" }
    ]
}
```
</details>

<details>
<summary>Level</summary>

```json
{
    "ExporterVersion": "2.5.3",
    "ExportType": "Level",
    "LevelName": "L_Foo",
    "WorldSettings": {
        "Class": "WorldSettings",
        "DeltaProperties": {
            "DefaultGameMode": "/Script/Engine.BlueprintGeneratedClass'/Game/Core/BP_GameMode.BP_GameMode_C'"
        }
    },
    "StreamingLevels": [
        {
            "PackageName": "/Game/Maps/L_FooProps",
            "Class": "LevelStreamingAlwaysLoaded",
            "ShouldBeLoaded": true,
            "ShouldBeVisible": true
        }
    ],
    "ActorCount": 181,
    "Actors": [
        {
            "Name": "StaticMeshActor_2",
            "Label": "SM_Cube",
            "Class": "/Script/Engine.StaticMeshActor",
            "Transform": { "Loc": "(-9740.000,-4450.000,-0.100)", "Scale": "(184.750,210.000,1.000)" },
            "Components": [
                {
                    "Name": "StaticMeshComponent0",
                    "Class": "/Script/Engine.StaticMeshComponent",
                    "Mobility": "Static",
                    "StaticMesh": "/Game/Meshes/SM_Cube.SM_Cube",
                    "CollisionProfile": "BlockAll",
                    "CollisionEnabled": "QueryAndPhysics",
                    "DeltaProperties": { "bUseDefaultCollision": "False" }
                }
            ]
        }
    ]
}
```

Export strategy: every actor / component serializes only the properties that differ from its own archetype (`UObject::GetArchetype()`), mirroring how `.umap` itself persists, lossless and at the highest compression rate. Blueprint-spawned actors align correctly to the BPGC CDO, so blueprint defaults and instance overrides remain distinguishable.

ISM / HISM / Foliage components with more than 200 instances export only the count, the bounds, and the first 5 samples, so a single foliage component cannot blow up the file.
</details>

<details>
<summary>Niagara System</summary>

```json
{
    "ExporterVersion": "2.5.3",
    "ExportType": "NiagaraSystem",
    "SystemName": "NS_Foo",
    "ExposedParameters": [],
    "Emitters": [
        {
            "Name": "DirectionalBurst",
            "Enabled": true,
            "SimTarget": "CPU",
            "SpawnScript": {
                "Parameters": [
                    { "Name": "DirectionalBurst.SpawnRate", "Type": "NiagaraFloat" }
                ]
            },
            "Renderers": [
                {
                    "RendererClass": "NiagaraSpriteRendererProperties",
                    "Properties": { ... }
                }
            ]
        }
    ]
}
```
</details>

</details>

<details>
<summary><b>Import</b>, 3 commandlets</summary>

| RunName | What it does |
| --- | --- |
| `WidgetLayoutImport` | Rebuilds a Widget Blueprint's widget tree from a spec |
| `DataAssetImport` | Writes JSON into a DataAsset's properties |
| `CreateAsset` | Creates assets of any type from a spec |

All three take `-spec=` pointing at the spec's absolute path. `CreateAsset` additionally requires `-unattended`, because `FMessageDialog` does not check for commandlet mode and some creation paths would raise a blocking dialog.

**WidgetLayoutImport**

Top-level spec fields.

| Field | Meaning |
| --- | --- |
| `AssetPath` | Asset path of the target Widget Blueprint |
| `ClassDefaults` | Optional, lands on the generated class CDO |
| `WidgetTree` | Root node, recursive from there |

Node fields.

| Field | Meaning |
| --- | --- |
| `Class` | Bare UMG class name such as `HorizontalBox`, or the full object path of a Blueprint class such as `/Game/UI/WBP_Bar.WBP_Bar_C` |
| `Name` | Widget name, C++ `BindWidget` matches on it |
| `Properties` | Property name to string value, applied through reflection `ImportText` |
| `Slot` | The node's slot properties inside its parent container, also through `ImportText` |
| `Children` | Child node array, the parent must be a panel type |

Property value strings take the same form Export emits, e.g. `(Value=1.000000,SizeRule=Fill)`, `(Right=48.000000)`, `HAlign_Fill`.

The behavior is whole-tree replacement, not incremental merge, so the spec must describe the complete tree. A `WidgetLayoutExport` output feeds straight back in as Import input, and the usual way to change a layout is Export the current tree, edit the JSON, Import it back. Renaming is the exception, the rebuild keeps widget animation bindings alive by matching names, so a rename through Import silently orphans them and has to go through `EditBlueprint`'s `Widgets` instead. Add, delete and reparent live there too, for when a whole-tree rewrite is more than the change needs.

`ClassDefaults` lands on the generated class CDO, which is where `EditDefaultsOnly` properties live rather than in the widget tree. It is applied after the compile, because the compile rebuilds the CDO.

```json
{
  "AssetPath": "/Game/UI/WBP_Foo",
  "WidgetTree": {
    "Class": "HorizontalBox",
    "Name": "RootBox",
    "Children": [
      {
        "Class": "SizeBox",
        "Name": "IconBox",
        "Properties": {
          "bOverride_WidthOverride": "True",
          "WidthOverride": "64.000000"
        },
        "Slot": {
          "Padding": "(Right=48.000000)",
          "VerticalAlignment": "VAlign_Center"
        },
        "Children": [
          {
            "Class": "Image",
            "Name": "Icon",
            "Properties": {
              "ColorAndOpacity": "(R=1.000000,G=1.000000,B=1.000000,A=1.000000)"
            }
          }
        ]
      },
      {
        "Class": "TextBlock",
        "Name": "Label",
        "Properties": {
          "Text": "INVTEXT(\"Label\")"
        },
        "Slot": {
          "Size": "(Value=1.000000,SizeRule=Fill)",
          "HorizontalAlignment": "HAlign_Fill"
        }
      }
    ]
  }
}
```

**DataAssetImport**

The inverse of `DataAssetExport`. Property values take either of two forms.

| Form | Goes through | Used for |
| --- | --- | --- |
| String | reflection `ImportText` | the struct literal form `DataAssetExport` emits, so an exported asset round-trips back in |
| Object or array | the json converter | nested structs and arrays in a hand-written spec, far more readable |

Only the named properties are written, everything else on the asset keeps its current value. If any single property fails to write, the whole save is abandoned, so no half-written asset is left behind.

```json
{
  "AssetPath": "/Game/Path/DA_Foo",
  "Properties": {
    "Scalar": "12.0",
    "Nested": [ { "Name": "A" }, { "Name": "B" } ]
  }
}
```

**CreateAsset**

The `Assets` array is created in order.

```json
{
  "Assets": [
    {
      "PackagePath": "/Game/Path",
      "AssetName": "DT_Foo",
      "Class": "/Script/Engine.DataTable",
      "FactoryProperties": { "Struct": "/Script/MyModule.MyRow" },
      "Properties": {}
    }
  ]
}
```

The two property blocks are separate because some types are only valid if their factory was configured first, otherwise creation fails or silently yields nothing. `FactoryProperties` is applied to the factory before creation, `Properties` is applied to the asset after creation and follows the same two forms as `DataAssetImport`.

| Asset type | Required in FactoryProperties | Consequence of omitting |
| --- | --- | --- |
| DataTable | `Struct`, the field name is `Struct`, not `RowStruct` | yields nothing |
| Blueprint | `ParentClass` plus `bSkipClassPicker` | blocking dialog |
| Texture2D | `Width` / `Height`, both must be powers of two | silently yields nothing |
| MaterialInstanceConstant | `InitialParent`, optional | an empty instance with no parent |

`Class` is best written as a full path such as `/Script/MediaAssets.MediaPlayer`, a short name resolves through a fuzzy engine lookup and warns, and a Blueprint generated class, which needs its package loaded, requires the full path. A later entry can reference in its `Properties` an asset path an earlier entry just created, so one spec wires up a group of mutually referencing assets. An asset already existing at the same path is skipped and reported, this never overwrites.

Material node graphs are out of reach. `CreateAsset` produces an empty material, but adding expressions such as `TextureSample` and wiring them up goes more directly through Python's `unreal.MaterialEditingLibrary`.

</details>

<details>
<summary><b>Edit</b>, 4 commandlets</summary>

Import regenerates an asset from a spec. Edit changes one that already exists, one writer per facet. This is where the plugin does the most, and what lets an agent hand back a change rather than a description of one.

| RunName | What it edits | Default |
| --- | --- | --- |
| `EditBlueprint` | Components, widgets, variables, defaults, functions, dispatchers, interfaces, state machines, graph, layout | dry run |
| `EditAnimAsset` | AnimSequence and AnimMontage notifies and curves, sync markers on a sequence, sections and slots on a montage | dry run |
| `EditTextureAsset` | Texture2D build settings | dry run |
| `EditMaterialAsset` | Material usage flags and base settings, MaterialInstanceConstant parent and parameter overrides | dry run |

Dry run is not a preview. Without `-apply` every writer still runs against the real asset and only the save is skipped, so a clean dry run means the spec validated for real. The changes die with the process.

**EditBlueprint**

Eleven writers, split along the same facets the editor's own Blueprint diff splits a Blueprint into.

| Spec key | Diff mode it mirrors | What it writes |
| --- | --- | --- |
| `Components` | `ComponentsMode` | SimpleConstructionScript component tree |
| `Widgets` | `DesignerMode` | WidgetBlueprint widget tree, add / delete / reparent / rename, widget or slot properties |
| `WidgetAnimations` | `DesignerMode` | WidgetBlueprint animation curves, keys on an existing channel |
| `Variables` | `MyBlueprintMode` | member variables, `Modify` retypes an existing one |
| `Defaults` | `DefaultsMode` | CDO and component template values, reaching components inherited from a parent Blueprint |
| `Functions` | `MyBlueprintMode` | function graphs, signature, local variables, access and flags |
| `Dispatchers` | `MyBlueprintMode` | event dispatchers and their signature graph |
| `Interfaces` | `ClassSettingsMode` | implemented interfaces |
| `StateMachines` | `GraphMode` | state machines, states, conduits, aliases, transitions |
| `Graph` | `GraphMode` | nodes, node properties, bindings, exposed pins, pin defaults, connections |
| `Layout` | none, purely cosmetic | node positions |

One target loads the asset once, runs every writer the spec names, then compiles and saves once. Writers run in a fixed order regardless of key order in the spec, because each depends on the last.

```
Components -> Widgets -> WidgetAnimations -> Variables -> Defaults -> Functions -> Dispatchers -> Interfaces -> StateMachines -> Graph -> Layout
```

`WidgetAnimations` addresses widgets by name and runs after `Widgets`, so an animation follows a widget renamed in the same run. `Graph` can reference the components, variables and function entry points the earlier writers made, and `Layout` addresses nodes by the Id `Graph` gave them. That dependency is why these are one commandlet rather than eleven.

| Facet | What it reaches |
| --- | --- |
| `Widgets` | `Add` / `Delete` / `Reparent` / `Rename` / `Modify`. Structural edits without a whole-tree Import. A reparent keeps the widget object, so its GUID, animation bindings and graph references follow it. Deleting a panel that still holds children is refused unless `Recursive` |
| `WidgetAnimations` | `SetKeys` replaces one channel's keys whole, addressed by animation, bound widget, track and channel meta name. Field names match what `WidgetLayoutExport` prints. Keys only, tracks and channels are made in the editor |
| `Graph` nodes | 25 node types, from `CallFunction` and `Branch` through `DynamicCast`, `MakeStruct` / `BreakStruct`, the four `Switch` kinds, `SpawnActor`, `Timeline`, `MathExpression` and `AnimGetter`. A `Type` starting with `/` is read as a class path, which is how anim graph nodes get built. A `Type` outside the table resolves as a StandardMacros graph name, so `Gate` and `DoOnce` need no ceremony |
| `Graph` `Bind` | Property-access bindings on anim nodes, the Bind dropdown in the Details panel. Aimed at a transition's Id it binds that transition's result |
| `Graph` `ExposePins` | Shows or hides the pin for an anim node property, addressed by name rather than by array index |
| `StateMachines` | Ten ops, `Add` / `AddState` / `AddConduit` / `AddAlias` / `AddTransition` / `ModifyState` / `ModifyTransition` / `RenameState` / `RemoveState` / `RemoveTransition`. Keys match what `AnimBlueprintExport` prints, so an exported state or transition pastes back as a spec |
| `Functions` | `Add` / `Rename` / `Modify` / `Remove`, with a `Signature` block carrying inputs, outputs, local variables, purity, access and category, in the shape the export prints |
| `Layout` | `Arrange` reads the graph's own topology and knows three kinds, K2 exec chains, pose graphs, state machines. `Straighten` is the editor's Q |

Nodes are addressed by Id. A node the spec creates takes whatever Id the spec gave it, a node that already exists takes the 32-hex NodeId `BlueprintEdGraphExport -graphs` prints, and both live in one namespace, so a new node can be wired straight onto an existing one. Addressing recurses into subgraphs, so a node inside a state's pose graph is reachable without switching graphs first. Connections go through `UEdGraphSchema_K2::TryCreateConnection`, which validates and inserts conversion nodes exactly as dragging a wire in the editor does.

Slate has measured nothing in a headless run, so `Layout` estimates node extents from title lines and pin rows rather than reading them off a widget, with a separate estimate per widget family, states and conduits, anim nodes, comment boxes.

**EditAnimAsset**

| Spec key | What it writes |
| --- | --- |
| `Notifies` | AnimNotify and AnimNotifyState add / modify / remove, including track placement and reflected parameters |
| `Curves` | Float curve add / rename / remove and keyframe writes, through the animation data model controller |
| `SyncMarkers` | Authored sync markers, AnimSequence only |
| `Sections` | Montage sections and the `NextSection` chain, AnimMontage only |
| `Slots` | Montage slots and their segments, AnimMontage only |

Order is fixed at `Slots` -> `Sections` -> `Curves` -> `SyncMarkers` -> `Notifies`, because slots decide the montage length every later time field is validated against.

**EditTextureAsset and EditMaterialAsset**

Both consume the `Spec` block their audit emits verbatim, so `AuditTexture` into `EditTextureAsset` and `AuditMaterial` into `EditMaterialAsset` are a find-then-fix pair with no translation step in between.

`EditTextureAsset` writes 17 build settings, LOD group, compression, sRGB, mip generation, size cap, streaming, virtual texturing, filtering and addressing. `EditMaterialAsset` writes all 23 usage flags plus blend mode, domain, shading model, two-sided and opacity mask clip value on a base material, and parent, scalar / vector / texture / static switch parameters and base property overrides on an instance. Material node graphs stay out of scope, Python's `unreal.MaterialEditingLibrary` covers those.

</details>

<details>
<summary><b>Migrate</b>, 8 commandlets</summary>

CoreRedirects covers the call side only, the implementation side and the consumer side inside Blueprint graphs are out of its range. A renamed interface event degrades a BP override into an orphaned custom event and the event stops firing, a renamed delegate parameter leaves a dangling pin on the binding node. Both still compile, and neither is findable by eye in a large project.

| RunName | What it does | Default |
| --- | --- | --- |
| `RedirectBlueprintEvent` | Reconnects a BP override that degraded into a custom event back to the new event, carrying its wiring across, and recognizes the `_N` dedup suffix UE appends | dry run |
| `RedirectBlueprintPin` | Moves a binding node's wiring from the old output pin to the new pin, then reconstructs the node to drop the old pin, touching only nodes that carry both the old and the new pin | dry run |
| `DeleteBlueprintNode` | Deletes graph nodes by node id, the cleanup step after graph logic moves into C++. Connections are cut, not rerouted, and nodes the schema refuses to delete are reported and skipped | dry run |
| `ReparentBlueprint` | Changes a Blueprint's parent class | writes directly |
| `ResaveAsset` | Forces load, compile, save so load-time fixups land on disk, after which that CoreRedirect can be dropped, supports Blueprint and map | writes directly |
| `SanitizeLevelReference` | Repoints every reference to an old asset inside a level at the new asset, then resaves that level | writes directly, `-dryrun` only counts |
| `DuplicateAsset` | Copies assets to new paths, the copy is independent and nothing is retargeted, and an existing destination is an error rather than an overwrite | dry run |
| `RenameAsset` | Renames or moves assets and repoints every referencer, hard and soft. Reports the referencer count per asset before and after, so a rename that dropped a reference surfaces instead of passing silently. Redirectors the rename leaves behind are fixed up and deleted unless `-keepredirectors` is passed | dry run |

The scanning commandlets list each Blueprint, event or node hit and how many wire groups would move, then take `-apply` to compile and save once it looks right. A scan with zero hits warns, check the `-OwnerClass` and the old name spelling first. Node ids for `DeleteBlueprintNode` are copied verbatim out of a `BlueprintEdGraphExport -graphs` output, and ids that matched nothing are reported at the end rather than passing silently.

`SanitizeLevelReference` must run before the old asset is deleted. Repointing needs both the old and the new on disk, running it after the delete is too late.

Modified uassets show up in the version control working copy afterwards.

</details>

<details>
<summary><b>Audit</b>, 4 commandlets and 3 scripts</summary>

| RunName | What it checks |
| --- | --- |
| `AuditLevelReference` | References inside level packages pointing at assets that no longer exist |
| `AuditLevelTopology` | Streaming relationships between levels, which one is persistent, which one is a sublevel |
| `AuditTexture` | Whether a texture's build settings match how it is actually sampled, rules T1 to T15 |
| `AuditMaterial` | Nanite compatibility and usage flags against actual application, rules N1 to N9 and U1 to U4 |

All four are read-only and save no package. Exit code 3 means the run worked and the report has findings, which is what a commit gate keys on.

**AuditLevelReference** walks the Asset Registry dependency graph and checks package existence one by one, it does not load worlds. Existence resolves against the mounted content roots, so every plugin of the project must be enabled before the run, otherwise dependencies under an unmounted root come back as false breakage. Its paired operation is `SanitizeLevelReference` in the Migrate group, audit finds the breakage, sanitize fixes it.

**AuditLevelTopology** assigns every level a role.

| Role | Test |
| --- | --- |
| `Standalone` | No streaming levels, and not referenced by another level |
| `PersistentHost` | Has streaming levels |
| `Sublevel` | No streaming levels, but referenced by another level |

Hosting outranks being hosted, a level that carries sublevels of its own is `PersistentHost` even when something else streams it in. The test is whether the level can be opened on its own to drive its sublevels, a copy of it living inside another level does not take that away, and `referenced_by` still records the nesting. Tools that need to drive streaming find the persistent level from this report instead of hardcoding a name into the script.

**AuditTexture** runs 15 rules over compression, sRGB, LOD group, mip generation, power-of-two, size budget, streaming and virtual texturing. The sampler rules do not reimplement the engine's opinion, they call `UMaterialExpressionTextureBase::VerifySamplerType`, the same check the material editor shows. Use is classified by walking the Asset Registry reverse graph from texture to material to instance to mesh / widget / Niagara, up to four hops, into UI / Character / Prop / World / VFX, which is what lets a rule say a UI texture is not in the UI group. Two passes keep it cheap, Asset Registry tags answer most rules and only the ones that need pixels load the texture.

**AuditMaterial** runs the Nanite rules N1 to N9 and the usage-flag rules U1 to U4, the latter comparing every `bUsedWith*` against what the material is actually applied to. Each base material also gets a `Cost` block, domain, blend mode, connected outputs, referenced texture count, expression and function counts, and a `Permutation` block, usage flag count multiplied by quality levels multiplied by static switch combinations. `PIEWarmup` ranks the materials those numbers say will make someone wait, and says so plainly when nothing qualifies. `-stats` adds a second tier, compiling representative shaders headless for sampler counts, texture sample counts and instruction counts, at tens of seconds to minutes per material.

Both audits emit a `Spec` block in the report, already in the shape their Edit counterpart consumes. Only rules with a determinate expected value go in, so an art decision such as a blend mode is reported and never auto-changed.

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    EditTextureAsset "" 10 600 \
    '-spec="C:/temp/texture_spec.json" -apply'
```

Three companion scripts.

| Script | What it does |
| --- | --- |
| `run_stream_metric.ps1` | Orchestration entry point for stream metric, one command covering topology, export, probe, report |
| `stream_metric_report.py` | Joins probe timings with `LevelExport` scale by level name into one table, including load milliseconds per thousand components |
| `level_budget_audit.py` | Reads `LevelExport` output offline into a component budget table, one row per level, flagging the ones over budget |

`run_stream_metric.ps1` covers four steps in one command.

1. Run `AuditLevelTopology` for the topology report, always executed, the target's sublevel list comes from it
2. Run `LevelExport` on those sublevels for scale data, `-SkipExport` skips it
3. Start an unattended editor measuring unload / gc / load wall-clock milliseconds and worst frame per sublevel
4. Run `stream_metric_report.py` to join timings with scale into one printed table

Without `-PersistentLevel` the script picks the single `PersistentHost` out of the topology report, and lists the candidates for an explicit pick when it finds more than one. The probe swaps `AlwaysLoaded` sublevels to `LevelStreamingDynamic` in memory so they can unload, and that step never saves. The editor must be closed before the run.

</details>

## Reading strategy

The exported JSON can be very large, a Blueprint of moderate complexity already reaches several thousand lines.

1. Grep first to locate the node titles, function names, widget names, or row names you care about
2. Read by line range once you have the line numbers, do not pull the whole file into context

```bash
grep -n "OnHealthChanged" Intermediate/UAssetExport/Game/Blueprints/BP_Foo_r*.json
```

## Why not the official toolchain

The official entry points for AI and automation, MCP, Remote Control, the Python API, evolve along with the engine version, capability gaps open up, and some editor subsystems regress in particular versions. They cannot be assumed stable across versions, and UE6 is still some way off, the versions in between have to keep working.

The workbench goes through commandlets plus stable engine APIs, routing around the layer that is still moving. The output is JSON files, version-controllable, diffable, replayable. When a mechanism the official path does not offer is needed, this is the extension point, adding a commandlet adds a capability and the calling contract does not change.

Online interactive approaches, the kind driven at editor runtime, solve a different class of problem, scene building, PIE debugging, ad hoc parameter tweaks. The two are not mutually exclusive.

## Integrating into your project

Copy the contents of `src/` into the project's `Plugins/UAssetWorkbench/`, and add to the `Plugins` array in `.uproject`.

```json
{
    "Name": "UAssetWorkbench",
    "Enabled": true
}
```

Regenerate project files and build.

It can also live at `<UE_PATH>/Engine/Plugins/Editor/UAssetWorkbench/` to be shared by every project.

Prerequisites: Unreal Engine 5.7, and the plugin must be compiled with the project. Calling through the wrapper does not require closing the editor, launching the commandlet directly does.

## For AI agents

| Doc | Contents |
| --- | --- |
| `Docs/AI-Guide.md` | Call manual for AI agents, a decision table covering every capability, call templates, common pitfalls |
| `Docs/Export.md` | Export group, 11 commandlets, every JSON field |
| `Docs/Import.md` | Import group, 3 commandlets, spec format |
| `Docs/Edit.md` | Edit group, 4 commandlets, every spec key and every layout op |
| `Docs/Migrate.md` | Migrate group, 8 commandlets |
| `Docs/Audit.md` | Audit group, 4 commandlets, full rule tables, stream metric workflow |

These docs are reference material for agents to read. Where behavior and documentation disagree, the source wins, the block comment at the top of each commandlet header carries the full contract.

## How it generalizes

UE is only the proving ground, the three reusable parts do not depend on it.

| Reusable asset | What it is | Transfers to |
| --- | --- | --- |
| Pattern | bidirectional bridge between opaque binary and AI-readable structured text | any GUI-locked proprietary format, DCC / CAD / BIM / EDA / simulation |
| Architecture | heartbeat-routed adaptive dual pipeline, live in-process and headless producing identical output | any heavyweight host with both an interactive and a headless mode, Houdini / Maya / Blender / Revit / MATLAB |
| Serialization discipline | token-cost-aware export, delta-from-archetype, cap-and-sample on overflow, a grep plus range-read contract | context engineering for any LLM data pipeline |

## Version

Current version: **2.5.3**

Defined in `src/Source/UAssetWorkbench/Public/UAssetWorkbenchVersion.h`, and embedded in the `ExporterVersion` field of every exported JSON.

## License

[MIT](LICENSE) - Hyrex Chia
