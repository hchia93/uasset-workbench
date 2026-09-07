# uasset-workbench

[English](README.md) | **中文**

![Claude Code](https://img.shields.io/badge/Claude_Code-black?style=flat&logo=anthropic&logoColor=white)
![Unreal Engine 5](https://img.shields.io/badge/Unreal_Engine-5.7-blue?logo=unrealengine&logoColor=white)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)

让脚本与 AI agent 能对 Unreal Engine 5 的 uasset 做非交互操作的编辑器插件。

## 它解决什么问题

| 问题 | 具体表现 |
| --- | --- |
| 读不了 | 逻辑与配置锁在二进制 `.uasset` 里，AI 拿到文件也无从下手。上百节点的 EventGraph 没有可读的文本形态，蓝图里的废弃变量、断掉的连线、错误的默认值靠肉眼在编辑器里审不完。Montage 的 notify 时间点、UMG 的层级与关键帧、Niagara 与材质参数、DataTable 数值、level 的 actor 摆放与 streaming 配置，全都只在编辑器界面里存在 |
| 改不了 | 要改 UMG 布局只能在编辑器里手工拖，没有可版本控制、可重放的写入路径 |
| 改不动既有的 | 给蓝图加个组件、在图里接上线、再设个默认值，是三次分开的手工编辑，一批蓝图就要重复一批次。动画状态机是同一件事再上一层，state、conduit、transition 与它们的规则图全是鼠标活 |
| 改名后修不了 | C++ 或资产改名后，CoreRedirects 只能修调用侧，修不了 Blueprint 图里的实现侧与消费侧，也修不了别的 level 对旧路径的 import |
| 审计不了 | 破损引用、level 的 streaming 拓扑、每个 level 的组件预算、贴图的压缩与 sRGB 设置、材质的 usage flag 与 Nanite 兼容，都没有批量查询的入口 |

五类问题对应五组能力。

## 五组能力

| 组 | 做什么 | 产出 | 数量 |
| --- | --- | --- | --- |
| Export | 读 uasset 结构导出 JSON | `Intermediate/UAssetExport` 下的 JSON | 11 |
| Import | 读 JSON spec 写回或创建 uasset | 被修改或新建的 uasset | 3 |
| Edit | 按意图修改既有 uasset | 被修改的 uasset | 4 |
| Migrate | C++ 或资产改名后修复引用 | 被修改的 uasset | 8 |
| Audit | 只读检查产出报告 | 报告 JSON | 4 |

组别由 run 名决定: 后缀 `Export` 是 Export 组，后缀 `Import` 与前缀 `Create` 是 Import 组，前缀 `Edit` 是 Edit 组，前缀 `Audit` 是 Audit 组，其余是 Migrate 组。命名上 `Import` 与 `Export` 是名词做后缀，其余动词在前。

## 通道与架构

所有操作收敛成同一套调用契约。heartbeat 文件 `Saved/UAssetWorkbenchTaskQueue/.alive` 按 mtime 决定路由，mtime 因编辑器同步卡顿变旧时，改查文件里记的编辑器进程 id。

| 编辑器状态 | 走哪条路 | 反馈 |
| --- | --- | --- |
| 开着 | 写 pending task，编辑器内的 subsystem 进程内执行 | 每个 run 在 Message Log 开一页，该 run 有 warning 或 error 才弹 toast |
| 关着 | 起 `UnrealEditor-Cmd` 跑 commandlet | log |

只读组 Export 与 Audit 两条路产出一致。写入组 Import、Edit、Migrate 必须在编辑器开着时跑。独立 commandlet 进程存出来的包，缺 asset registry 建依赖图要读的那几条 import。资产本身没问题，打开能用，但 Reference Viewer 里它是个孤立节点，直到有人在编辑器里再存一次。细节与补救步骤见 [Docs/Migrate.md](Docs/Migrate.md)。

这套路由的意义: 五组能力共享同一个调用入口和同一套产出约定，工作流可以按组合拼装，导出结构，离线分析，写回资产，审计验证，而不是每加一个工具就多一种调用方式。加一个 commandlet 就是加一个能力，契约不变。

## 调用契约

每个 commandlet 只返回四个码，出自同一个枚举，编辑器内的 queue 通道原样透传。

| 码 | 含义 |
| --- | --- |
| 0 | 成功 |
| 1 | 失败，参数错、资产 load 不到、值写不进去 |
| 2 | 编辑器在运行，commandlet 那条路主动让开而不是与它抢 |
| 3 | 运行本身成功，报告里有要处理的东西，Audit 组专用 |

3 不是失败，正是这一点让 audit 能当提交 gate 用。

写入按 target 全有或全无。任一 writer 失败，整个 target 在落盘前中止；编译不过的 Blueprint 永不保存。插件建出来的节点都带 `RF_Transactional`，改过的资产在编辑器里打开不会弹「需要重存」的提示。

## 快速开始

Wrapper: `src/scripts/run_commandlet.sh`，集成进项目后位于 `Plugins/UAssetWorkbench/scripts/`。

```
run_commandlet.sh <UE_PATH> <UPROJECT> <RunName> <AssetList> [IDLE_SEC] [MAX_SEC] [EXTRA_ARGS]
```

| 参数 | 说明 |
| --- | --- |
| `UE_PATH` | 引擎安装根目录 |
| `UPROJECT` | `.uproject` 的绝对路径 |
| `RunName` | commandlet 的 run 名 |
| `AssetList` | 逗号分隔的资产路径，spec 驱动的 run 传空串 |
| `IDLE_SEC` | Export 组判定完成用的输出 mtime 静默秒数，默认 10 |
| `MAX_SEC` | 总等待上限，默认 600 |
| `EXTRA_ARGS` | 透传给 commandlet 的参数 |

Git Bash 下要加 `MSYS_NO_PATHCONV=1` 前缀，否则 `/Game/...` 会被改写成 Windows 路径。

Export。

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    BlueprintEdGraphExport "/Game/Blueprints/BP_Foo"
```

Import。

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    WidgetLayoutImport "/Game/UI/WBP_Foo" 10 600 \
    '-spec="C:/temp/WBP_Foo.spec.json"'
```

Edit。

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    EditBlueprint "" 10 600 \
    '-spec="C:/temp/BP_Foo.edit.json" -apply'
```

Migrate。

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    RedirectBlueprintEvent "/Game/Blueprints/BP_Foo" 10 600 \
    '-OwnerClass="/Script/MyModule.MyActor" -OldEvent="OnPickedUp" -NewEvent="HandlePickedUp"'
```

Audit。

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    AuditTexture "" 10 600 \
    '-scandir="/Game"'
```

## 各组详解

<details>
<summary><b>Export</b>，11 个 commandlet</summary>

| RunName | 导出内容 |
| --- | --- |
| `BlueprintEdGraphExport` | Blueprint 图、节点、pin、连线、函数签名、变量、事件分发器、timeline、组件、引用资产 |
| `AnimAssetExport` | AnimSequence 与 AnimMontage 的 notify、曲线、轨道名、root motion，sequence 另有 sync marker，montage 另有 section 与 slot |
| `WidgetLayoutExport` | Widget 树、slot 布局属性、子类属性、动画关键帧、EdGraph |
| `DataAssetExport` | DataAsset 子类的全部自定义属性，数组元素展开 |
| `DataTableExport` | DataTable 行结构名与全部行数据，按 RowName 索引 |
| `NiagaraSystemExport` | Niagara emitter 列表、spawn/update script 参数、renderer 属性 |
| `MaterialExport` | Material 表达式连线链与全局设置，MaterialInstance 的参数覆写 |
| `TextureExport` | Texture 属性，压缩、sRGB、LOD group、mip、源尺寸 |
| `BehaviorTreeExport` | BT 树结构、节点参数、Blackboard key |
| `AnimBlueprintExport` | AnimBP EdGraph、状态机的状态、转换、blend 设置、入口状态、事件绑定 |
| `LevelExport` | Level 的 actor / component、与 archetype 的差异属性、碰撞与静态网格与 ISM 摘要、streaming level |

输出: `Intermediate/UAssetExport/<AssetPath>_r<revision>_<YYYYMMDD-HHMMSS>.json`，不入版本控制。

`revision` 是该资产在版本控制里的 last-changed revision。戳过的文件名在导出成功之后才落，所以同一资产的多次导出互不覆盖，过期的导出也不会被当成新鲜的，读最新那份。

每份 JSON 都带 `ExporterVersion` 与 `ExportType` 两个通用字段，其余按资产类型展开。

`BlueprintEdGraphExport`、`AnimBlueprintExport`、`WidgetLayoutExport` 共用一份 EdGraph 序列化器，图、节点、pin 三层的键在三份产物里完全相同，同一张图经两个 commandlet 导出可以直接 diff。

| 新增 | 给读者的东西 |
| --- | --- |
| 每张函数类图的 `Signature` | 入参、返回值、局部变量、access、flag，形态就是 `EditBlueprint` 能直接吃回去的 spec |
| 变量元数据、`EventDispatchers[]`、`Timelines[]` | 蓝图里住在图之外的那部分 |
| anim 节点的 `Settings` / `Bindings` / `ExposedPins` | Details 面板里的东西，不只是 pin 上的 |
| 每条 transition 的 `RuleSummary` | 一行说清这条靠什么进入，读状态机不必展开每张规则图 |
| AnimAsset 的曲线、sync marker、轨道名、root motion、montage blend | 过去只在编辑器时间轴里存在的时序数据 |
| 贴图与材质的属性块 | Edit 组与 Audit 组要动的那些构建设置 |

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

`Parameters` 改完直接喂回 `EditAnimAsset`。
</details>

<details>
<summary>AnimBlueprint 状态机</summary>

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

transition 的键就是 `EditBlueprint` 在 `StateMachines` 下读的那套，导出的一条 transition 原样贴回就是 spec。
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

MaterialInstance 导出参数覆写表。

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

导出策略: 每个 actor / component 只序列化与自身 archetype（`UObject::GetArchetype()`）不同的属性，跟 `.umap` 本身的持久化方式一致，无损且压缩率最高。蓝图生成的 actor 会正确对齐到 BPGC CDO，蓝图默认值与实例覆写因此仍然可以区分。

ISM / HISM / Foliage 组件的实例数超过 200 时只导出数量、包围盒与前 5 个采样，避免单个植被组件撑爆文件。
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
<summary><b>Import</b>，3 个 commandlet</summary>

| RunName | 做什么 |
| --- | --- |
| `WidgetLayoutImport` | 从 spec 重建 Widget Blueprint 的控件树 |
| `DataAssetImport` | 把 JSON 写进 DataAsset 的属性 |
| `CreateAsset` | 从 spec 创建任意类型的资产 |

三个都用 `-spec=` 指向 spec 的绝对路径。`CreateAsset` 还必须带 `-unattended`，引擎的 `FMessageDialog` 不检查 commandlet 模式，某些创建路径会弹窗阻塞。

**WidgetLayoutImport**

spec 顶层字段。

| 字段 | 含义 |
| --- | --- |
| `AssetPath` | 目标 Widget Blueprint 的资产路径 |
| `ClassDefaults` | 可选，写 generated class 的 CDO |
| `WidgetTree` | 根节点，往下递归 |

节点字段。

| 字段 | 含义 |
| --- | --- |
| `Class` | 裸 UMG 类名如 `HorizontalBox`，或 Blueprint 类的完整对象路径如 `/Game/UI/WBP_Bar.WBP_Bar_C` |
| `Name` | 控件名，C++ 的 `BindWidget` 靠它匹配 |
| `Properties` | 属性名到字符串值，走反射 `ImportText` |
| `Slot` | 该节点在父容器里的 slot 属性，同样走 `ImportText` |
| `Children` | 子节点数组，父节点必须是 panel 类型 |

属性值的字符串形态就是 Export 导出的那种，例 `(Value=1.000000,SizeRule=Fill)`、`(Right=48.000000)`、`HAlign_Fill`。

行为是整树替换，不是增量合并，spec 必须描述完整的树。`WidgetLayoutExport` 的产物可以直接当 Import 的输入，改布局的常规做法是先 Export 拿到当前树，改 JSON，再 Import 写回。改名是例外，重建靠同名保住 widget animation 的绑定，所以走 Import 改名会让那些绑定悬空且不报错，改名只能走 `EditBlueprint` 的 `Widgets`。增、删、挪也在那里，整树重写超过改动需要时走它。

`ClassDefaults` 落在 generated class 的 CDO 上，`EditDefaultsOnly` 那类属性住在那里，不在控件树里。它在编译之后才应用，因为编译会重建 CDO。

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

`DataAssetExport` 的反向操作。属性值两种形态。

| 形态 | 走什么 | 用在哪 |
| --- | --- | --- |
| 字符串 | 反射 `ImportText` | `DataAssetExport` 导出的那种结构字面量，导出的资产能 round-trip 回去 |
| 对象或数组 | json 转换器 | 手写 spec 时的嵌套结构与数组，可读性好得多 |

只有 spec 里点名的属性会被写，资产上其余属性保持原值。任何一个属性写失败就整体放弃保存，不会留下写了一半的资产。

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

`Assets` 数组按顺序创建。

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

两个属性块分开，是因为有些类型必须在创建前配好 factory，否则创建会失败甚至静默返回空。`FactoryProperties` 在创建前配到 factory 上，`Properties` 在创建后套到资产上，规则同 `DataAssetImport` 的两形态。

| 资产类型 | FactoryProperties 必填 | 不填的后果 |
| --- | --- | --- |
| DataTable | `Struct`，字段名是 `Struct` 不是 `RowStruct` | 返回空 |
| Blueprint | `ParentClass` 加 `bSkipClassPicker` | 弹窗阻塞 |
| Texture2D | `Width` / `Height`，且必须是 2 的幂 | 静默返回空 |
| MaterialInstanceConstant | `InitialParent`，可选 | 得到无 parent 的空实例 |

`Class` 推荐写完整路径如 `/Script/MediaAssets.MediaPlayer`，短名在引擎里是模糊查找会打警告，Blueprint 生成类这种要加载包的必须写完整路径。后面的条目可以在 `Properties` 里引用前面刚创建出来的资产路径，一条 spec 就能把互相引用的一组资产接好。同名资产已存在时跳过并报告，永不覆盖。

建不了材质的节点图。`CreateAsset` 能建出空材质，但往里加 `TextureSample` 之类的表达式并连线，走 Python 的 `unreal.MaterialEditingLibrary` 更直接。

</details>

<details>
<summary><b>Edit</b>，4 个 commandlet</summary>

Import 是照 spec 重新生成一份资产，Edit 是改动既有的那一份，一个面配一个 writer。这里是插件做得最多的地方，也是 agent 能交回一个改动而不是一段说明的地方。

| RunName | 改什么 | 默认行为 |
| --- | --- | --- |
| `EditBlueprint` | 组件、控件、变量、默认值、函数、分发器、接口、状态机、图、排版 | dry run |
| `EditAnimAsset` | AnimSequence 与 AnimMontage 的 notify 与曲线，sequence 另有 sync marker，montage 另有 section 与 slot | dry run |
| `EditTextureAsset` | Texture2D 的构建设置 | dry run |
| `EditMaterialAsset` | Material 的 usage flag 与基本设定，MaterialInstanceConstant 的 parent 与参数覆写 | dry run |

dry run 不是预览。不给 `-apply` 时每个 writer 照样对真实资产跑一遍，只跳过保存，所以 dry run 干净就是 spec 真的校验过了。改动随进程退出丢弃。

**EditBlueprint**

十一个 writer，划分对齐编辑器自己的 Blueprint diff 把蓝图拆成的那几个面。

| Spec key | 对应 diff mode | 写什么 |
| --- | --- | --- |
| `Components` | `ComponentsMode` | SimpleConstructionScript 组件树 |
| `Widgets` | `DesignerMode` | WidgetBlueprint 的控件树，增删改挪与控件或 slot 属性 |
| `WidgetAnimations` | `DesignerMode` | WidgetBlueprint 的动画曲线，改既有通道的 key |
| `Variables` | `MyBlueprintMode` | 成员变量，`Modify` 能给既有变量重定类型 |
| `Defaults` | `DefaultsMode` | CDO 与组件模板的属性值，覆盖父 BP 继承来的组件 |
| `Functions` | `MyBlueprintMode` | 函数图、签名、局部变量、access 与 flag |
| `Dispatchers` | `MyBlueprintMode` | 事件分发器与其签名图 |
| `Interfaces` | `ClassSettingsMode` | 实现的接口 |
| `StateMachines` | `GraphMode` | 状态机、state、conduit、alias、transition |
| `Graph` | `GraphMode` | 节点、节点属性、绑定、暴露 pin、pin 默认值、连线 |
| `Layout` | 无，纯外观 | 节点位置 |

一个 target 只 load 一次资产，跑完 spec 点名的所有 writer，编译保存一次。writer 的执行顺序固定，与 spec 里 key 的顺序无关，因为后一个依赖前一个。

```
Components -> Widgets -> WidgetAnimations -> Variables -> Defaults -> Functions -> Dispatchers -> Interfaces -> StateMachines -> Graph -> Layout
```

`WidgetAnimations` 按控件名寻址且排在 `Widgets` 之后，同一次运行里改完名的控件动画能跟上。`Graph` 能引用同一次运行里前面 writer 新建的组件、变量与函数入口，`Layout` 能用 `Graph` 给节点的 Id 寻址。这条依赖就是十一个面合成一个 commandlet 而不是拆成十一个的原因。

| 面 | 能到哪 |
| --- | --- |
| `Widgets` | `Add` / `Delete` / `Reparent` / `Rename` / `Modify`。不整树 Import 也能改结构。换父级不换控件对象，GUID、动画绑定、图里的引用都跟着走。面板还挂着子控件时 `Delete` 默认拒绝，要连子树删才给 `Recursive` |
| `WidgetAnimations` | `SetKeys` 整条通道替换 key，按动画名、绑定控件、轨道与通道 meta 名寻址。字段名与 `WidgetLayoutExport` 打印的一致。只改 key，轨道和通道要先在编辑器里建 |
| `Graph` 节点 | 25 种节点类型，从 `CallFunction`、`Branch` 到 `DynamicCast`、`MakeStruct` / `BreakStruct`、四种 `Switch`、`SpawnActor`、`Timeline`、`MathExpression` 与 `AnimGetter`。`Type` 以 `/` 开头当类路径解析，anim graph 节点走这条。表外的 `Type` 去 StandardMacros 里按图名查，所以 `Gate` 与 `DoOnce` 直接写就行 |
| `Graph` 的 `Bind` | anim 节点的 property access 绑定，就是 Details 面板那个 Bind 下拉框。指向一条 transition 的 Id 就绑到它的 result 上 |
| `Graph` 的 `ExposePins` | 露出或收起 anim 节点某个属性的 pin，按属性名寻址而不是数组下标 |
| `StateMachines` | 十个 op，`Add` / `AddState` / `AddConduit` / `AddAlias` / `AddTransition` / `ModifyState` / `ModifyTransition` / `RenameState` / `RemoveState` / `RemoveTransition`。键名与 `AnimBlueprintExport` 打印的一致，导出的 state 或 transition 原样贴回就是 spec |
| `Functions` | `Add` / `Rename` / `Modify` / `Remove`，`Signature` 块带入参、返回值、局部变量、纯函数、access 与分类，形态就是导出打印的那个 |
| `Layout` | `Arrange` 按图自己的拓扑铺，认三种图，K2 的 exec 链、pose 图、状态机。`Straighten` 就是编辑器里的 Q |

节点用 Id 寻址。spec 新建的节点用 spec 给的 Id，已存在的节点用 `BlueprintEdGraphExport -graphs` 打印的 32 位 NodeId，两者同一命名空间，所以新节点能直接接到旧节点上。寻址递归穿透子图，state 内部 pose 图里的节点不必先切到那一层就能点名。连线走 `UEdGraphSchema_K2::TryCreateConnection`，与在编辑器里拖线同一条路，会校验并在需要时插入转换节点。

headless 下 Slate 没量过任何东西，所以 `Layout` 的节点尺寸是从标题行数与 pin 行数估出来的，不是从 widget 上读的，并且按 widget 家族分开估，state 与 conduit、anim 节点、注释框各一套。

**EditAnimAsset**

| Spec key | 写什么 |
| --- | --- |
| `Notifies` | AnimNotify 与 AnimNotifyState 的增删改，含轨道摆放与反射出的参数 |
| `Curves` | float 曲线的增删改名与关键帧写入，走 animation data model controller |
| `SyncMarkers` | AuthoredSyncMarkers，仅 AnimSequence |
| `Sections` | montage 段落与 `NextSection` 链，仅 AnimMontage |
| `Slots` | montage 的 slot 与 segment，仅 AnimMontage |

顺序固定为 `Slots` -> `Sections` -> `Curves` -> `SyncMarkers` -> `Notifies`，因为 slot 决定 montage 长度，后面每个时间字段都按这个长度校验。

**EditTextureAsset 与 EditMaterialAsset**

两个都原样吃各自 audit 产出的 `Spec` 块，所以 `AuditTexture` 接 `EditTextureAsset`、`AuditMaterial` 接 `EditMaterialAsset` 是一对查完就修的组合，中间没有翻译步骤。

`EditTextureAsset` 写 17 项构建设置，LOD group、压缩、sRGB、mip 生成、尺寸上限、streaming、虚拟纹理、过滤与寻址。`EditMaterialAsset` 在基材质上写全部 23 个 usage flag 加 BlendMode、domain、ShadingModel、双面与 opacity mask 裁剪值，在实例上写 parent、标量 / 向量 / 贴图 / static switch 参数与 base property override。材质节点图不在范围内，那是 Python 的 `unreal.MaterialEditingLibrary` 的活。

</details>

<details>
<summary><b>Migrate</b>，8 个 commandlet</summary>

CoreRedirects 只覆盖调用侧，Blueprint 图里的实现侧与消费侧不在它的射程内。改名的 interface event 会让 BP override 退化成孤立的 custom event，事件不再触发；改名的 delegate 参数会在绑定节点上留下悬空 pin。两类都编译得过去，靠人眼在大项目里扫不出来。

| RunName | 做什么 | 默认行为 |
| --- | --- | --- |
| `RedirectBlueprintEvent` | 把退化成 custom event 的 BP override 重新接回新事件，连线一并搬过去，能识别 UE 加的 `_N` 去重后缀 | dry run |
| `RedirectBlueprintPin` | 把绑定节点的连线从旧输出 pin 移到新 pin，再重建节点丢掉旧 pin，只处理同时带有新旧两个 pin 的节点 | dry run |
| `DeleteBlueprintNode` | 按 node id 删图节点，图逻辑搬进 C++ 之后的清理步骤。连线是切断不是重接，schema 拒删的节点报出后跳过 | dry run |
| `ReparentBlueprint` | 改 Blueprint 的父类 | 直接落盘 |
| `ResaveAsset` | 强制 load、compile、save，让 load 期的 fixup 落盘，之后就能撤掉那条 CoreRedirect，支持 Blueprint 与 map | 直接落盘 |
| `SanitizeLevelReference` | 把 level 里对旧资产的每一处引用换成新资产，然后 resave 这个 level | 直接落盘，`-dryrun` 只统计 |
| `DuplicateAsset` | 把资产复制到新路径，副本独立，内部引用不做重定向，目标已存在是错误而不是覆盖 | dry run |
| `RenameAsset` | 改资产名或搬路径，硬引用与软引用一起重指。逐个资产报改名前后的 referencer 数，掉引用会报错而不是静默通过。留下的 redirector 默认 fixup 后删除 | dry run |

只扫描的那几个逐条列出命中的 Blueprint、事件或节点、以及会搬多少组连线，确认无误再补 `-apply` 编译并保存。扫描一条都没命中时会给 warning，先核对 `-OwnerClass` 与旧名拼写。`DeleteBlueprintNode` 的 node id 从 `BlueprintEdGraphExport -graphs` 的产物里原样抄，没命中的 id 会在结尾报出来，不会静默通过。

`SanitizeLevelReference` 必须在删除旧资产之前跑。换指针需要新旧两边都还在磁盘上，删完再跑就来不及了。

执行后被改动的 uasset 会出现在版本控制的工作副本里。

</details>

<details>
<summary><b>Audit</b>，4 个 commandlet 与 3 个脚本</summary>

| RunName | 检查什么 |
| --- | --- |
| `AuditLevelReference` | level 包里指向已不存在资产的引用 |
| `AuditLevelTopology` | level 之间的 streaming 关系，谁是 persistent，谁是 sublevel |
| `AuditTexture` | 贴图的构建设置与它实际被怎么采样是否对得上，规则 T1 到 T15 |
| `AuditMaterial` | Nanite 兼容与 usage flag 是否对得上实际挂载，规则 N1 到 N9 与 U1 到 U4 |

四个都只读，不保存任何包。退出码 3 表示运行成功且报告里有东西要处理，提交 gate 就看这个。

**AuditLevelReference** 走 Asset Registry 的依赖图逐个判断包是否存在，不 load world。包是否存在按已挂载的 content root 解析，所以跑之前必须让项目的插件全部启用，否则未挂载 root 下的依赖会被判成假破损。它的配对操作是 Migrate 组的 `SanitizeLevelReference`，audit 找出破损，sanitize 修。

**AuditLevelTopology** 给每个 level 分角色。

| 角色 | 判定 |
| --- | --- |
| `Standalone` | 没有 streaming levels，也不被别的 level 引用 |
| `PersistentHost` | 有 streaming levels |
| `Sublevel` | 没有 streaming levels，但被别的 level 引用 |

hosting 优先于被 hosted，一个 level 只要自己挂着 sublevel 就是 `PersistentHost`，哪怕它同时被别的 level 引用。判据是它能不能被独立打开来驱动自己的 sublevel，被复制一份塞进别的 level 里不影响这一点，嵌套关系由 `referenced_by` 照常记录。需要驱动 streaming 的工具靠这份报告找 persistent level，不必再把名字硬编码进脚本。

**AuditTexture** 跑 15 条规则，覆盖压缩、sRGB、LOD group、mip 生成、2 的幂、尺寸预算、streaming 与虚拟纹理。sampler 那几条不自己重写引擎的判断，直接调 `UMaterialExpressionTextureBase::VerifySamplerType`，跟材质编辑器给你看的是同一个结论。用途分类走 Asset Registry 反向图，贴图到材质到材质实例到 mesh / widget / Niagara，最多四跳，分成 UI / Character / Prop / World / VFX 五类，规则因此能说出「这是张 UI 贴图但不在 UI group 里」。两趟压成本，Asset Registry 的 tag 就够判的当场判完，只有必须看像素的才把贴图 load 进来。

**AuditMaterial** 跑 Nanite 规则 N1 到 N9 与 usage flag 规则 U1 到 U4，后者拿每个 `bUsedWith*` 与材质实际挂在什么上对照。每个基材质另外产出 `Cost` 块，domain、BlendMode、接线的输出、引用贴图数、表达式与函数数，以及 `Permutation` 块，usage flag 数乘 quality level 数乘 static switch 组合数。`PIEWarmup` 把这些数字排成「谁会让人在 PIE 里等」的名单，一条都没有时也明说。`-stats` 开二档，headless 逐材质编译代表性 shader 读采样器数、纹理采样数与指令数，一个材质几十秒到几分钟。

两个 audit 都在报告里产出 `Spec` 块，形状就是对应 Edit commandlet 吃的那个。只有建议值确定的规则会进去，BlendMode 这类美术决定只报不改。

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    EditTextureAsset "" 10 600 \
    '-spec="C:/temp/texture_spec.json" -apply'
```

配套脚本三个。

| 脚本 | 做什么 |
| --- | --- |
| `run_stream_metric.ps1` | stream metric 的编排入口，一条命令跑完拓扑、导出、探针、出表 |
| `stream_metric_report.py` | 把探针耗时与 `LevelExport` 的规模按 level 名合并成表，附每千组件的加载毫秒 |
| `level_budget_audit.py` | 离线读 `LevelExport` 的产物出组件预算表，每个 level 一行，标出超预算的 |

`run_stream_metric.ps1` 一条命令跑完四步。

1. 跑 `AuditLevelTopology` 出拓扑报告，这一步总是执行，目标的 sublevel 列表要从报告里拿
2. 对那些 sublevel 跑 `LevelExport` 拿规模数据，`-SkipExport` 可跳过
3. 起 unattended 编辑器逐 sublevel 测 unload / gc / load 墙钟毫秒与最差帧
4. 跑 `stream_metric_report.py` 把耗时与规模合并成表打印

没给 `-PersistentLevel` 时脚本从拓扑报告里找唯一的 `PersistentHost`，找到多个就列出候选要求显式指定。探针把 `AlwaysLoaded` 的 sublevel 在内存里换成 `LevelStreamingDynamic` 才能卸载，这一步永远不保存。跑之前编辑器必须关闭。

</details>

## 读取策略

导出的 JSON 可能非常大，一个中等复杂度的 Blueprint 就能到几千行。

1. 先 grep 定位关心的节点标题、函数名、控件名、行名
2. 拿到行号后按区间读，不要整份读进上下文

```bash
grep -n "OnHealthChanged" Intermediate/UAssetExport/Game/Blueprints/BP_Foo_r*.json
```

## 为什么不依赖官方工具链

官方给 AI 与自动化的入口，MCP、Remote Control、Python API，随引擎版本演进，能力有空窗，某些编辑器子系统在特定版本会出现回归。跨版本不能假设它们稳定，而 UE6 还有一段距离，中间的版本要照常干活。

workbench 走 commandlet 加引擎稳定 API，绕开正在演进的那一层。产出是 JSON 文件，可版本控制、可 diff、可重放。需要官方没有提供的机制时，这里就是扩展点，加一个 commandlet 就是加一个能力，调用契约不变。

在线交互式方案，也就是在编辑器运行时驱动的那一类，解决的是另一类问题，场景搭建、PIE 调试、即时参数微调。两者不互斥。

## 集成到你的项目

把 `src/` 的内容复制到项目的 `Plugins/UAssetWorkbench/`，在 `.uproject` 的 `Plugins` 数组加。

```json
{
    "Name": "UAssetWorkbench",
    "Enabled": true
}
```

重新生成工程文件并编译。

也可以放 `<UE_PATH>/Engine/Plugins/Editor/UAssetWorkbench/` 让所有项目共享。

前置: Unreal Engine 5.7，插件必须随项目编译。走 wrapper 调用不需要关编辑器，直接起 commandlet 才需要。

## 给 AI agent 用

| 文档 | 内容 |
| --- | --- |
| `Docs/AI-Guide.md` | 给 AI agent 的调用手册，覆盖全部能力的决策表加调用模板加常见坑 |
| `Docs/Export.md` | Export 组，11 个 commandlet，每个 JSON 字段 |
| `Docs/Import.md` | Import 组，3 个 commandlet，spec 格式 |
| `Docs/Edit.md` | Edit 组，4 个 commandlet，每个 spec key 与每个 layout op |
| `Docs/Migrate.md` | Migrate 组，8 个 commandlet |
| `Docs/Audit.md` | Audit 组，4 个 commandlet，完整规则表与 stream metric 工作流 |

这些文档是给 agent 读的参考。调用行为与文档描述不符时以源码为准，每个 commandlet header 顶部的块注释写了完整契约。

## 泛化

UE 只是验证场，三样可复用的东西不依赖它。

| 可复用的东西 | 是什么 | 可迁移到 |
| --- | --- | --- |
| 模式 | 不透明二进制与 AI 可读结构化文本之间的双向桥 | 任何被 GUI 锁住的专有格式，DCC / CAD / BIM / EDA / 仿真 |
| 架构 | heartbeat 路由的自适应双管线，live 进程内与 headless 两条路产出一致 | 任何同时有交互模式与无头模式的重型宿主，Houdini / Maya / Blender / Revit / MATLAB |
| 序列化纪律 | 顾及 token 成本的导出，与 archetype 求差、超量采样截断、grep 加区间读的读取契约 | 任何 LLM 数据管线的上下文工程 |

## 版本

当前版本: **2.5.3**

定义在 `src/Source/UAssetWorkbench/Public/UAssetWorkbenchVersion.h`，同时嵌进每份导出 JSON 的 `ExporterVersion` 字段。

## License

[MIT](LICENSE) - Hyrex Chia
