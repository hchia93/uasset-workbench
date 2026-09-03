# Edit

按意图修改既有资产。资产层面的 redirect / resave / replace 属于 [Migrate](Migrate.md)，从 spec 重新生成属于 [Import](Import.md)。

## Commandlet

| RunName | 编辑对象 |
| --- | --- |
| `EditBlueprint` | Blueprint 的组件、控件、变量、默认值、函数、分发器、接口、状态机、图、排版 |
| `EditAnimAsset` | AnimSequence / AnimMontage 的 notify 与曲线，Sequence 另有 sync marker，Montage 另有 section 与 slot |
| `EditTextureAsset` | Texture2D 的构建设置 |
| `EditMaterialAsset` | Material 的 usage flag 与基本设定，MaterialInstanceConstant 的 parent 与参数覆写 |

## EditBlueprint

### Writer

`EditBlueprint` 一个面配一个 writer，划分对齐编辑器 Blueprint diff（`SBlueprintDiff.cpp`）。一个 target 只 load 一次资产，跑完 spec 点名的所有 writer，编译保存一次。任一 writer 失败整个 target 不落盘。

| Spec key | Writer | 对应 diff mode | 写什么 |
| --- | --- | --- | --- |
| `Components` | `FBlueprintComponentWriter` | `ComponentsMode` | SimpleConstructionScript 组件树 |
| `Widgets` | `FBlueprintWidgetWriter` | `DesignerMode` | WidgetBlueprint 的控件树 |
| `Variables` | `FBlueprintVariableWriter` | `MyBlueprintMode` | 成员变量 |
| `Defaults` | `FBlueprintDefaultsWriter` | `DefaultsMode` | CDO 与组件模板的属性值 |
| `Functions` | `FBlueprintFunctionWriter` | `MyBlueprintMode` | 函数图 |
| `Dispatchers` | `FBlueprintDispatcherWriter` | `MyBlueprintMode` | 事件分发器与其签名图 |
| `Interfaces` | `FBlueprintInterfaceWriter` | `ClassSettingsMode` | 实现的接口 |
| `StateMachines` | `FBlueprintStateMachineWriter` | `GraphMode` | 状态机、state、transition |
| `Graph` | `FBlueprintGraphWriter` | `GraphMode` | 节点、节点属性、绑定、pin 默认值、连线 |
| `Layout` | `FBlueprintLayoutWriter` | 无（纯外观） | 节点位置 |

执行顺序固定，与 spec 里的 key 顺序无关：

```
Components -> Widgets -> Variables -> Defaults -> Functions -> Dispatchers -> Interfaces -> StateMachines -> Graph -> Layout
```

后一个依赖前一个的结果。`Graph` 能引用同一次运行里新建的组件和变量，也能点名 `Functions` 注册的函数入口与 result 节点，`Layout` 能用 `Graph` 给节点的 `Id` 寻址，这是十个 writer 合成一个 commandlet 的原因。

### 调用

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    EditBlueprint "" 10 600 "-spec=C:/path/spec.json -apply"
```

无 `-apply` 是 dry run，照样跑完每个 writer，只跳过编译与保存，所以校验强度与 `-apply` 一样；改动靠进程退出丢弃，编辑器开着走 queue 通道时 dry run 退 2。AssetList 传空字符串，target 写在 spec 里。

### Spec

```json
{
  "Targets": [
    {
      "AssetPath": "/Game/Path/BP_Foo",
      "Components": [
        { "Op": "Add", "Class": "/Script/Engine.PointLightComponent", "Name": "Lamp" },
        { "Op": "Rename", "Name": "Lamp", "NewName": "KeyLight" },
        { "Op": "Reparent", "Name": "Lid", "Parent": "KeyLight" },
        { "Op": "Remove", "Name": "Obsolete" }
      ],
      "Variables": [
        { "Op": "Add", "Name": "BaseIntensity", "Type": "float", "Default": "500.0" },
        { "Op": "Add", "Name": "Watchers", "Type": "object",
          "SubType": "/Script/Engine.Actor", "Container": "Array" },
        { "Op": "Modify", "Name": "BaseIntensity", "Category": "Lighting", "InstanceEditable": true },
        { "Op": "Rename", "Name": "Watchers", "NewName": "Observers" },
        { "Op": "Remove", "Name": "Legacy" }
      ],
      "Defaults": [
        { "Properties": { "InitialLifeSpan": "12.0" } },
        { "Component": "KeyLight", "Properties": { "Intensity": "2400.0" } }
      ],
      "Functions": [
        { "Op": "Add", "Name": "ComputeThing", "Id": "fn",
          "Signature": {
            "Inputs": [
              { "Name": "A", "Type": "int" },
              { "Name": "Amount", "Type": "float", "Default": "1.0" }
            ],
            "Outputs": [ { "Name": "Sum", "Type": "float" } ],
            "LocalVariables": [ { "Name": "Scratch", "Type": "int", "Default": "3" } ],
            "Pure": true, "Access": "Protected", "Category": "Gameplay"
          }
        },
        { "Op": "Modify", "Name": "ComputeThing", "Signature": { "Pure": false, "Access": "Public" } },
        { "Op": "Rename", "Name": "ComputeThing", "NewName": "ComputeThing2" },
        { "Op": "Remove", "Name": "OnStateFullyBlendedIn" }
      ],
      "Dispatchers": [
        { "Op": "Add", "Name": "OnThing", "Id": "disp", "Category": "Gameplay",
          "Signature": {
            "Inputs": [
              { "Name": "Amount", "Type": "int" },
              { "Name": "Who", "Type": "object", "SubType": "/Script/Engine.Actor" }
            ]
          }
        },
        { "Op": "Modify", "Name": "OnThing", "Signature": { "Inputs": [ { "Name": "Amount", "Type": "int" } ] } },
        { "Op": "Rename", "Name": "OnThing", "NewName": "OnThing2" },
        { "Op": "Remove", "Name": "Legacy" }
      ],
      "Interfaces": [
        { "Op": "Add", "Interface": "/Script/Engine.Interface_AssetUserData" },
        { "Op": "Remove", "Interface": "/Game/Path/BPI_Old", "PreserveFunctions": true }
      ],
      "Graph": {
        "Name": "EventGraph",
        "Nodes": [
          { "Id": "ev", "Type": "CustomEvent", "EventName": "RunProbe" },
          { "Id": "gate", "Type": "Gate" }
        ],
        "NodeProperties": [ { "Node": "gate", "Properties": { "NodeComment": "probe gate" } } ],
        "PinDefaults": [ { "Node": "gate", "Pin": "bStartClosed", "Value": "true" } ],
        "Unlink": [ { "Node": "A1B2...", "Pin": "then" } ],
        "Links": [ { "FromNode": "ev", "FromPin": "then", "ToNode": "gate", "ToPin": "Enter" } ],
        "Delete": [ { "Node": "C3D4..." } ]
      },
      "Layout": [
        { "Op": "StackHorizontal", "Nodes": ["ev", "gate"], "Spacing": 140 }
      ]
    },
    {
      "AssetPath": "/Game/Path/ABP_Foo",
      "StateMachines": [
        { "Op": "Add", "Name": "Locomotion", "Graph": "AnimGraph", "Id": "sm" },
        { "Op": "AddState", "Machine": "Locomotion", "Name": "Idle", "Entry": true },
        { "Op": "AddState", "Machine": "Locomotion", "Name": "Walk", "Id": "walk" },
        { "Op": "AddConduit", "Machine": "Locomotion", "Name": "Airborne" },
        { "Op": "AddAlias", "Machine": "Locomotion", "Name": "AnyGround", "States": ["Idle", "Walk"] },
        { "Op": "AddTransition", "Machine": "Locomotion", "From": "Idle", "To": "Walk",
          "Blend": 0.2, "Id": "tr" },
        { "Op": "ModifyState", "Machine": "Locomotion", "Name": "Walk",
          "bAlwaysResetOnEntry": true, "UpdateFunction": { "Function": "TickWalk" } },
        { "Op": "ModifyTransition", "Machine": "Locomotion", "From": "Idle", "To": "Walk",
          "LogicType": "Inertialization", "PriorityOrder": 3 },
        { "Op": "RenameState", "Machine": "Locomotion", "Name": "Walk", "NewName": "Stride" },
        { "Op": "RemoveTransition", "Machine": "Locomotion", "From": "Idle", "To": "Stride" },
        { "Op": "RemoveState", "Machine": "Locomotion", "Name": "Stride" }
      ],
      "Graph": {
        "Name": "AnimGraph",
        "Nodes": [
          { "Id": "seq", "Type": "/Script/AnimGraph.AnimGraphNode_SequencePlayer" }
        ],
        "NodeProperties": [ { "Node": "seq", "Properties": { "Node.PlayRate": "1.25" } } ],
        "Bind": [ { "Node": "seq", "Property": "PlayRate", "Path": "LocomotionState.PlayRate" } ],
        "ExposePins": [ { "Node": "seq", "Show": ["StartPosition"], "Hide": ["PlayRate"] } ],
        "Links": [ { "FromNode": "seq", "FromPin": "Pose", "ToNode": "8A7B...", "ToPin": "Result" } ]
      },
      "Layout": [
        { "Op": "Arrange", "Graph": "AnimGraph" }
      ]
    }
  ]
}
```

### Components

Op: `Add` / `Remove` / `Rename` / `Reparent`，按 spec 顺序执行，后面的能看到前面的结果。

`Add` 默认挂到场景根，`Parent` 指定则挂到该组件下。`Asset` 可选，只对吃资产的组件有意义。名字已存在是错误而不是 no-op，重跑不会悄悄多出一个带引擎后缀的副本。

### StateMachines

Op: `Add` / `AddState` / `AddConduit` / `AddAlias` / `AddTransition` / `ModifyState` / `ModifyTransition` / `RenameState` / `RemoveState` / `RemoveTransition`，按 spec 顺序执行，后面的能看到前面的结果。跑在 `Graph` 之前，所以同一个 spec 可以先建状态机再往 state 里填节点。

`Machine` 是状态机的图名，覆盖 `AnimGraph` 上挂的每一台以及嵌在 state 里的嵌套机。`From` / `To` 是 state 名，conduit 与 alias 同样按名寻址。既有资产里的和同一次运行新建的都能寻址，重名时新建的优先。一对 state 之间有多条 transition（双向、重复连线）时必须给 `Index`，缺了报错并给出可选范围。

`Add` 在 `Graph`（默认 `AnimGraph`）里放一台状态机，`Name` 是它的图名。`AddState` 的 `Entry` 为真时把入口指向它，入口只有一个，后来的替换先前的。

`AddTransition` 把 rule 图改名成 `<From>_to_<To>`，重名自动加后缀。引擎给的原名一律是 `Transition`，寻址不到，改名之后同一条 spec 的 `Graph` 就能用这个名字往 rule 图里填节点。

`AddConduit` 用 `Name` 建一个 conduit，bound graph 改名成 `Name`。

`AddAlias` 用 `Name` 建一个 alias，`States` 给被别名的 state 名单，或者 `Global` 为真表示全局别名。两者必须给且只能给一个。`States` 里认不出的名字报错并列出机器里现有的 state。

`RenameState` 用 `NewName`，走节点自己的名字校验器，重名或撞到 anim layer 名报错。state 的名字就是它 bound graph 的名字，alias 的名字存在节点上。已有 transition 的 rule 图保持原来的 `<From>_to_<To>` 名字，不跟着改。

`RemoveState` 按 `Name` 删。挂在这个 state 上的 transition 一并删掉，引擎自己不做这一步，留下的 transition 编译期看不到也报不出来。删的是入口 state 时报错，`Force` 为真才删，删完机器没有入口并给一条 warning。

`RemoveTransition` 按 `From` / `To` 删，多条时同样要 `Index`。规则图跟着走，但共享规则组里还有别的成员时那张图留着。

每个 op 都收 `Id`，注册的是这次解析到的节点，既有的与新建的一样，`Graph` 的 `NodeProperties` 与 `Links` 由此点名它。`PosX` / `PosY` 给摆放位置。

`ModifyTransition` 只动 spec 里出现的键，`AddTransition` 吃同一套键，键名与 `AnimBlueprintExport` 的 `Transitions[]` 一致，导出的一段 transition 原样贴回就能用。

| 键 | 值 | 说明 |
| --- | --- | --- |
| `Blend` / `CrossfadeDuration` | 秒 | 同一个字段的两个名字 |
| `BlendMode` | `EAlphaBlendOption` 枚举名 | `Linear` / `HermiteCubic` / `CubicInOut` 等，不认的名字报错并列出全部 |
| `CustomBlendCurve` | 曲线路径或 `None` | |
| `BlendProfile` | blend profile 路径或 `None` | 走 `BlendProfileWrapper`，收 skeleton blend profile |
| `LogicType` | `StandardBlend` / `Inertialization` / `Custom` | 也收导出打的 `TLT_` 前缀形式。切到 `Custom` 建出 `CustomTransitionGraph`，切走连图一起删，与编辑器同步 |
| `PriorityOrder` | 整数 | |
| `Bidirectional` | bool | |
| `bAutomaticRuleBasedOnSequencePlayerInState` | bool | |
| `AutomaticRuleTriggerTime` | 秒 | 负值表示在资产播完前 `Blend` 秒触发 |
| `MinTimeBeforeReentry` | 秒 | `-1` 关闭 |
| `bAllowInertializationForSelfTransitions` | bool | |
| `SyncGroupNameToRequireValidMarkersRule` | 名字或 `None` | |
| `bDisabled` | bool | |
| `SharedRules` | `{ "Name": "X" }` 或 `"None"` | 组里已有成员就加入并共用它的 rule 图，没有就开一个新组。`None` 退出，退出的一方拿回一份自己的 rule 图 |
| `SharedCrossfade` | `{ "Name": "X" }` 或 `"None"` | 同上，共用的是 crossfade 那几个字段，加入时本条的值被组里的覆盖 |
| `TransitionStart` / `TransitionEnd` / `TransitionInterrupt` | `{ "Name": "X" }` 或 `"None"` | 名字式的 custom notify，与细节面板里那三个 Notifications 字段同一个东西。`None` 清掉 |

transition 的结果 pin 绑定不在这里，走 `Graph` 的 `Bind`，`Node` 直接给本 op 注册的 `Id`。

`ModifyState` 只动 spec 里出现的键，键名与 `AnimBlueprintExport` 的 `States[]` 一致。改名走 `RenameState`，不在这里。

| 键 | 值 | 说明 |
| --- | --- | --- |
| `StateType` | `EAnimStateType` 枚举名 | 也收 `AST_` 前缀形式。只做校验，与当前类型不一致就报错，state 的类型建出来就定死了，编辑器也不给换 |
| `bAlwaysResetOnEntry` | bool | |
| `StateEntered` / `StateLeft` / `StateFullyBlended` | `{ "Name": "X" }` 或 `"None"` | 名字式的 custom notify，与细节面板里那三个 Notifications 字段同一个东西。`None` 清掉 |
| `InitialUpdateFunction` / `BecomeRelevantFunction` / `UpdateFunction` / `StateEntryFunction` / `StateFullyBlendedInFunction` / `StateExitFunction` / `StateFullyBlendedOutFunction` | `{ "Function": "X" }` 或 `"None"` | 绑在 state bound graph 里的 result 节点上，按名字当本 BP 自己的函数解析。`None` 清掉 |

绑定的函数必须是本 BP 里签名与 `Prototype_ThreadSafeAnimUpdateCall` 相容且标了 thread safe 的函数，否则编译报错。conduit 与 alias 没有这些键，写了报错。

节点建法与编辑器一致：入图、给 guid、`PostPlacedNewNode`。状态机节点和 state 都在那一步里自建子图，所以 spec 不碰子图本身。

### Widgets

Op: `Rename` / `Modify`，都用 `Name` 点名控件树里的控件，按 spec 顺序执行，后面的能看到前面的结果。

`Rename` 用 `NewName`。控件改名只能走这里：`WidgetLayoutImport` 是整树替换，靠「同名保 GUID」让动画绑定活过重建，改名恰好绕开那个前提，所以 Export 改 JSON 再 Import 那条路改不动名字。

改名连带改掉的东西：

| 落点 | 说明 |
| --- | --- |
| `WidgetVariableNameToGuidMap` | 换 key，GUID 值保留 |
| widget animation | `AnimationBindings` 的 `WidgetName`，以及非 slot 绑定对应的 MovieScene possessable |
| property binding | `Bindings` 的 `ObjectName` |
| navigation | 其他控件指向它的 navigation binding |
| desired focus | 类上记的 desired focus 控件名 |
| 图里的变量引用 | Get / Set 节点一并改指 |

新名字被占用是错误。父类上的 `BindWidget` / `BindWidgetOptional` 属性占用该名字是合法例外，那正是要对上的目标。

`Modify` 用 `Properties` 写控件自身的属性，`Slot` 写它在父容器里的 slot 属性，两个可选但至少给一个。值的形态与 `DataAssetImport` 一致，字符串走 `ImportText`，对象与数组走 json 转换器。根控件没有 slot，对它发 `Slot` 是错误。

```json
{
  "Targets": [
    {
      "AssetPath": "/Game/UI/WBP_Foo",
      "Widgets": [
        { "Op": "Rename", "Name": "OldBar", "NewName": "NewBar" },
        { "Op": "Modify", "Name": "NewBar", "Properties": { "ToolTipText": "INVTEXT(\"Health\")" }, "Slot": { "Padding": "(Left=8.000000)" } }
      ]
    }
  ]
}
```

target 不是 WidgetBlueprint 却带了 `Widgets` key 是错误。

### Variables

Op: `Add` / `Modify` / `Remove` / `Rename`。

`Type` 用编辑器类型下拉框显示的词：`bool` `byte` `enum` `int` `int64` `float` `double` `string` `name` `text` `object` `class` `softobject` `softclass` `struct`。

`SubType` 在 `enum` / `object` / `class` / `softobject` / `softclass` / `struct` 下必填，写枚举、类或 script struct 的路径。`Container` 取 `None` / `Array` / `Set` / `Map`。`Map` 的值类型写 `ValueType`，同一套类型词，值类型要子对象时再加 `ValueSubType`。

`Add` 与 `Modify` 吃同一套可选键，键名与 `BlueprintEdGraphExport` 的 `Variables[]` 一致，导出的一段变量原样贴回就能用。

| 键 | 值 | 说明 |
| --- | --- | --- |
| `Type` / `SubType` / `Container` / `ValueType` / `ValueSubType` | 见上 | `Modify` 下就是重定类型。只给 `Container` 只换容器，没出现的键保持原样。引擎不收的新类型报错退 1 |
| `Default` | 字符串 | 与导出的 `Default` 同形态 |
| `Category` | 字符串 | |
| `Tooltip` | 字符串 | |
| `InstanceEditable` | bool | 实例细节面板里可改 |
| `ReadOnly` | bool | Blueprint 图里只读 |
| `ExposeOnSpawn` | bool | |
| `Private` | bool | |
| `Transient` | bool | |
| `SaveGame` | bool | |
| `Config` | bool | |
| `AdvancedDisplay` | bool | 细节面板的 Advanced 折叠区 |
| `ExposeToCinematics` | bool | |
| `Replicated` | bool | |
| `RepNotify` | bool | `Replicated` 为假时一并清掉 |
| `RepNotifyFunc` | 字符串 | 不给则沿用已有的，都没有取 `OnRep_<Name>` |

`Modify` 只动 spec 里出现的键。`Name` 必须是本 BP 自己声明的变量，组件变量与父类继承来的属性都报错并列出可选项。

`RepNotify` 指到的函数不在这里建，同一条 spec 用 `Functions` 的 `Add` 补上即可，`Functions` 跑在 `Variables` 之后，编译在最后。

同一条 `Modify` 里既重定类型又给 `Default` 时，新值等编译才落到 CDO，此前读回的还是旧类型的值。

### Defaults

每条写一个作用域。不带 `Component` 写 actor CDO 本身，带则写该组件。

BP 自己声明的组件属性挂在 SCS node 的 template 上，继承来的 native 组件则以 CDO 子对象的形式保存 override，两条路径都覆盖。

父 BP 声明的组件走 `InheritableComponentHandler`，第一次写入时建出本 BP 的 override template，与编辑器 Components 面板改继承组件是同一条路径。

### Graph

节点类型：

| `Type` | 字段 | 说明 |
| --- | --- | --- |
| `CallFunction` | `Function`, `Target` | |
| `CallParentFunction` | `Function` | 在 BP 的父类上解析 |
| `VariableGet` / `VariableSet` | `Variable` | |
| `Event` | `EventName`, `Class` | override 父类或原生事件，`Class` 默认取 BP 的父类。同一事件已经实现是错误，报错列出可 override 的事件名 |
| `CustomEvent` | `EventName` | |
| `Branch` / `MultiGate` / `Self` / `Knot` | 无 | `Knot` 是 reroute |
| `Sequence` | `OutputCount` | |
| `Select` | `Enum` 或 `OptionCount` | |
| `DynamicCast` | `Class`, `PureCast` | |
| `MakeStruct` / `BreakStruct` | `Struct` | |
| `SwitchEnum` | `Enum` | 每个枚举条目一个 exec 出口 |
| `SwitchString` / `SwitchName` | `Cases` | 出口名就是 `Cases` 的条目 |
| `SwitchInteger` | `Cases`, `StartIndex` | 出口名是从 `StartIndex` 起的连号，`Cases` 只取长度 |
| `SpawnActor` | `Class` | |
| `Timeline` | `Name` | |
| `Comment` | `Text`, `Width`, `Height` | |
| `MathExpression` | `Expression` | |
| `MacroInstance` | `Macro` | |
| `AnimGetter` | `Getter`, `Machine`, `State`, `Transition` | 只能落在 transition rule 图或 custom blend 图 |

带 `MD_NativeMakeFunction` 的结构体（`FVector` / `FRotator` / `FTransform` 等）编译时会被引擎换成对应的 `Make X` / `Break X` 函数调用节点，pin 默认值与连线一并搬过去，导出里看到的是 `K2Node_CallFunction`。

`Timeline` 同时建出 `UTimelineTemplate`，轨道不在范围内，要加轨道另走 Details 面板。

节点注释与启用状态没有专门的 op，走 `NodeProperties` 写 `NodeComment` 与 `EnabledState`（`Enabled` / `Disabled` / `DevelopmentOnly`）。

`Type` 以 `/` 开头当类路径解析，直接按类建节点，anim graph 节点走这条：`"Type": "/Script/AnimGraph.AnimGraphNode_SequencePlayer"`。连线用的是目标图自己的 schema，所以 pose pin 与 exec pin 同一套写法。

`CallFunction` 的 `Target` 先当组件名解析，找不到再当类路径，静态函数和函数库走后者。

`AnimGetter` 就是 transition rule 里那组 `Get Relevant Anim Time Remaining` 节点。`Getter` 取 AnimBP 原生父类上带 `AnimGetter` 元数据的函数名，认不出报错并列出全部 getter 与各自的 `GetterContext`。

| 键 | 值 | 说明 |
| --- | --- | --- |
| `Getter` | 函数名 | 如 `GetRelevantAnimTimeRemainingFraction` |
| `Machine` | 状态机图名 | getter 声明 `MachineIndex` 参数时必给 |
| `State` | 状态名 | getter 声明 `StateIndex` 参数时必给 |
| `Transition` | `{ From, To, Index }` | getter 声明 `TransitionIndex` 参数时必给 |

`MachineIndex` / `StateIndex` / `TransitionIndex` 三个 pin 每次编译由引擎从 `Machine` / `State` / `Transition` 反算并覆写，spec 不写。声明 `AssetPlayerIndex` 的 getter 要点名一个 asset player 节点，这套字段点不到，报错。落错图是错误，报错带上那张图的类型。

比较节点是普通 `CallFunction`。一条完整的 rule：

```json
"Graph": {
  "Machine": "Locomotion", "Transition": { "From": "Idle", "To": "Walk" },
  "Nodes": [
    { "Id": "g1", "Type": "AnimGetter", "Getter": "GetRelevantAnimTimeRemainingFraction", "Machine": "Locomotion", "State": "Idle" },
    { "Id": "cmp", "Type": "CallFunction", "Function": "Less_DoubleDouble", "Target": "/Script/Engine.KismetMathLibrary" }
  ],
  "PinDefaults": [ { "Node": "cmp", "Pin": "B", "Value": "0.1" } ],
  "Unlink": [ { "Node": "630182DA4D53F4141AD5B792F2AA8565", "Pin": "bCanEnterTransition" } ],
  "Links": [
    { "FromNode": "g1", "FromPin": "ReturnValue", "ToNode": "cmp", "ToPin": "A" },
    { "FromNode": "cmp", "FromPin": "ReturnValue", "ToNode": "630182DA4D53F4141AD5B792F2AA8565", "ToPin": "bCanEnterTransition" }
  ]
}
```

变量驱动的 rule 不用建图，`Bind` 更省：绑到 transition 的 `Id` 上就落到它 result 节点的 `bCanEnterTransition`。

`Gate` / `DoOnce` / `ForEachLoop` / `Do N` 是 StandardMacros 里的宏图不是原生节点。上表以外的 `Type` 一律去 StandardMacros 里按图名查，所以 `Gate` 直接写就行。别的宏库走 `MacroInstance` 加完整的 `"Package.Asset:GraphName"`。

`Select` 二选一：`Enum` 给枚举路径，option pin 跟枚举条目走；`OptionCount` 给数量，2 是 bool 三元，更多是整数索引。两者都不给则 index pin 保持 wildcard，除非 spec 往里接了有类型的东西，否则编译不过。

`Sequence` 用 `OutputCount` 给出口数，`then_0` `then_1` 自带，更多的往上加。

节点用 `Id` 寻址。本次新建的节点用 spec 给的 `Id`，已存在的节点用 `BlueprintEdGraphExport -graphs` 打印的 32 位 `NodeId`，两者同一命名空间，新节点可以直接接到旧节点上。Pin 名内部名和编辑器显示的 friendly name 都认。

执行顺序：建节点 → `NodeProperties` → `Bind` → `ExposePins` → pin 默认值 → `Unlink` → `Links` → `Delete`。`Bind` 会把绑上的 pin 显示出来，`ExposePins` 排在它后面所以有最终发言权；`Unlink` 在 `Links` 之前，所以一次 pass 就能改道既有的 exec 链；`Delete` 收尾，所以同一 pass 能先把上下游接起来再把中间节点删掉。

`Delete` 按 `Id` 删节点，连线一并断开。引擎不允许用户删的节点（函数入口、result 等）报错，与编辑器里的行为一致。

`NodeProperties` 按属性路径写节点对象自己的属性，格式与 `Defaults` 一致，支持 `Struct.Field` 下钻。pin 之外的节点设定都走这里，anim graph 节点尤其如此：sequence player 的 `Node.PlayRate`、state result 的 `StateFullyBlendedInFunction`，编辑器里都在 Details 面板而不在 pin 上。清一个 function binding 写 `"<Prop>.MemberName": "None"`，空 struct 字面量 `()` 不会清掉既有字段。

被 `NodeProperties` 写过的节点一律重建一次，所以 `TargetType` / `StructType` / `Enum` 这类改拓扑的属性在 pin 默认值与 `Links` 之前就已经生成好新 pin。被 pin 默认值写过 class pin 的节点（`SpawnActor` / construct-object / `GetClassDefaults`）同样重建，exposed-on-spawn pin 随之出现。重建按 pin 名保连线。

`Bind` 写 anim graph 节点的 property access 绑定，就是 Details 面板那个 Bind 下拉框。绑上之后属性每帧由 anim instance 求值，不再看 pin 上的字面量也不看接进来的线。

| 键 | 值 | 说明 |
| --- | --- | --- |
| `Node` | `Id` 或 `NodeId` | 与其他 op 同一命名空间。给 transition 的 `Id` 就绑到它 rule 图里的 `AnimGraphNode_TransitionResult` 上，不用另外点名 result 节点 |
| `Property` | anim node 属性名 | 内部名与 Details 面板显示的 friendly name 都认，认不出报错并列出可绑的属性 |
| `Path` | 相对 AnimBP 的属性访问路径 | 点号分段，形如 `LocomotionState.bIsFalling`。末段是函数时按函数绑定写出 |
| `Function` | 本 BP 的函数名 | `Path` 的语法糖，解析到的不是函数就报错 |

`Path` 与 `Function` 二选一必给一个。值写 `"None"` 清掉绑定，pin 的显示状态不动。

路径按 `SkeletonGeneratedClass` 解析，同一条 spec 里 `Variables` / `Functions` 新加的都在，它们跑在 `Graph` 之前且各自刷过骨架。解析不到，或末端类型与目标属性不相容（bool / float / int 之间的隐式提升算相容），都是错误并列出两边的类型。

绑上之后引擎把这个 pin 显示出来并断掉它原有的连线，5.0 起绑定就是以 pin 的形式显示的。要藏起来在同一条 spec 里用 `ExposePins` 的 `Hide`，藏起来不影响绑定生效。

`ExposePins` 切 `ShowPinForProperties[i].bShowPin`，就是 Details 面板每个属性前面那个 pin 勾选框。

| 键 | 值 | 说明 |
| --- | --- | --- |
| `Node` | `Id` 或 `NodeId` | 与 `Bind` 同一套解析，transition 同样落到它的 result 节点 |
| `Show` | 属性名数组 | 露出 pin |
| `Hide` | 属性名数组 | 收起 pin |

同一条 entry 里 `Show` 先于 `Hide`。认不出的属性名报错并列出可选项。`NodeProperties` 写 `ShowPinForProperties[i].bShowPin` 是同一个字段，但要数出下标，anim node 结构体加一个属性下标就漂了。

pin 默认值的 `Value` 必填。对象 / 类 / 接口 pin 加载不到、软对象 / 软类 pin 的路径在资产注册表里查无此物、枚举 pin 给了不存在的条目、schema 判定值非法，四者都是错误并回滚整个 target，不再静默保持原值。软路径只查存在不加载，软类路径末尾的 `_C` 会回退到同名 Blueprint 上查。结构体 pin 按字面量写，形如 `(X=1.0,Y=2.0,Z=0.0)`。

节点寻址递归穿透子图，所以 state machine 里的 state、state 内部 anim graph 的节点，都能用导出打印的 `NodeId` 直接点名，不必先切到那一层图。

`Graph` 的 `Name` 也认子图，所以状态机内部的 state 图直接写它的名字就能进去改。

transition 的 rule 图引擎一律叫 `Transition`，只在自己那个 transition 节点内唯一，按名字会点到别的那张。改 rule 图改用 `Machine` 加 `Transition`，与 `Name` 二选一：`"Graph": { "Machine": "Locomotion", "Transition": { "From": "Idle", "To": "Walk", "Index": 0 } }`。

`Variables` 的 `Remove` 会连带删掉引用该变量的 Get / Set 节点，所以同一个 spec 的 `Delete` 里不要再点名它们，否则轮到 `Graph` 时节点已经不在，报 `no node ... to delete` 并回滚整个 target。写法顺序上 `Variables` 早于 `Graph`。

连线走 `UEdGraphSchema_K2::TryCreateConnection`，与编辑器里拖线同路，会校验并在需要时插转换节点。schema 拒绝的连线是错误。

### Functions

Op: `Add` / `Rename` / `Modify` / `Remove`，按 spec 顺序执行，后面的能看到前面的结果。所有 op 都用 `Name` 点名函数图。

`Add` 建一张新函数图，可以内联带 `Signature`。名字被函数图、宏图、事件图、变量或父类函数占用是错误，报错会列出占用方与现有函数名单。

`Rename` 用 `NewName`，调用点一并改指，新名字同样过名字校验。

`Modify` 必须带 `Signature`。

`Remove` 按函数图名删。被删函数的调用点会变成错误节点，同一个 spec 要顺手用 `Graph` 的 `Delete` 把调用点一并清掉。

`Id`（`Add` / `Modify` 可选）注册函数入口节点，result 节点注册成 `<Id>.Result`，同一个 spec 的 `Graph` 就能点名它们。`Graph` 的 `Name` 也能直接写新建的函数图名。

`Signature` 字段：

| 字段 | 说明 |
| --- | --- |
| `Inputs` | 入口节点的参数，元素 `{ Name, Type, SubType, Container, IsReference, Default }` |
| `Outputs` | result 节点的返回值，同样的元素形状。没有 result 节点且列表非空时按编辑器的做法建一个 |
| `LocalVariables` | 函数局部变量，元素 `{ Name, Type, SubType, Container, Default }` |
| `Pure` / `Const` / `Static` | bool |
| `Access` | `Public` / `Protected` / `Private` |
| `CallInEditor` | bool |
| `Category` / `Keywords` / `Tooltip` | 字符串 |

`Type` / `SubType` / `Container` 与 `Variables` 同一套词，见上面的 `Variables` 一节。

`Inputs` / `Outputs` / `LocalVariables` 是全集语义：spec 里有节点上没有的就建，类型变了就改类型并清掉旧默认值，节点上有 spec 里没有的就删。key 不写就整块不动，写成空数组 `[]` 就清空。

pin 顺序按 spec 顺序，那也是编译出来的参数顺序。

`Signature` 里只出现的 key 才会被写，其余保持原样。

override 与接口实现函数的签名归父类 / 接口所有，对它们发 `Signature` 是错误。`Remove` 与 `Rename` 不受限。

形状与 `BlueprintEdGraphExport -graphs` 打印的 `Signature` 一致，导出的那段可以直接贴回来当 spec。

### Dispatchers

Op: `Add` / `Rename` / `Modify` / `Remove`，按 spec 顺序执行。所有 op 都用 `Name` 点名分发器。

一个分发器是一个 multicast delegate 成员变量加一张同名签名图，每个 op 两半一起改。

`Add` 建变量与签名图，`Category` 与 `Signature` 可选。名字被变量、函数图、宏图或事件图占用是错误。

`Rename` 用 `NewName`，签名图与已绑定的事件节点一并改指。

`Modify` 必须带 `Signature`，语义与 `Functions` 的 `Inputs` 一致：给了就是完整集合。

`Remove` 删变量与签名图。留下的 Bind / Call / Assign 节点会变成错误节点，同一个 spec 要顺手用 `Graph` 的 `Delete` 清掉。

`Id`（`Add` / `Modify` 可选）注册签名图的入口节点，与 `Functions` 同一套约定。

`Signature` 只收 `Inputs`，元素形状与 `Functions` 一节相同。multicast delegate 没有返回值，写 `Outputs` 是错误。

形状与 `BlueprintEdGraphExport -graphs` 打印的 `EventDispatchers[].Signature` 一致，导出的那段可以直接贴回来当 spec。

### Interfaces

Op: `Add` / `Remove`，用 `Interface` 点名接口，不用 `Name`。

`Interface` 收 native 接口的类路径 `/Script/Module.MyInterface`，即 `UINTERFACE` 生成的那个类去掉前缀，不是 `IMyInterface`。也收 Blueprint 接口的资产路径 `/Game/Path/BPI_Foo`。解析不到、或解析到的不是接口类是错误，报错会列出当前已实现的接口。

`Add` 建接口要求的函数图。带返回值的函数进 `InterfaceFunction` 图，纯事件签名的函数直接在事件图里当事件放，不建图。

`Remove` 的 `PreserveFunctions` 为真时把接口函数转成普通函数留下，默认为假连图一起删。

### Layout

Op: `Arrange` `Straighten` `AlignLeft` `AlignRight` `AlignTop` `AlignBottom` `AlignCenterX` `AlignCenterY` `StackHorizontal` `StackVertical` `Move`。

#### 图寻址

op 三种寻址方式，都不写就用 `Graph` writer 用的那张。

| 键 | 目标 |
| --- | --- |
| `Graph` | 按图名，EventGraph / 函数图 / AnimGraph / state 内部图 |
| `Machine` | 状态机图 |
| `Machine` + `Transition { From, To, Index }` | 该 transition 的 rule 图，引擎把 rule 图全叫 Transition，按名找不准 |

一个 op 只动目标图自己的节点，不伸进子图。transition 节点是画在两个 state 之间线上的箭头，任何 op 都不排它，点名就退 1。

#### Arrange

按拓扑铺整张图，一行坐标都不用手写。

```json
{ "Op": "Arrange", "Nodes": ["ev"], "Spacing": 80, "RowSpacing": 48, "PosX": 0, "PosY": 0 }
```

`Nodes` 在这个 op 里是**入口点**不是操作对象。铺法按图的种类分三套：

| 图 | Arrange 做什么 | 缺省入口 |
| --- | --- | --- |
| K2（EventGraph / 函数 / 宏） | exec 链从入口往右平铺 | 无，`Nodes` 必填 |
| pose（AnimGraph / state 内部图 / rule 图 / custom blend） | result 节点靠右边，pose 输入逐列往左，同级竖着堆 | result 节点 |
| 状态机 | entry 在最左，沿 transition BFS 分层，同层竖着堆，transition 不排 | entry 节点 |

三套共用的规则：

| | |
| --- | --- |
| 一个节点的第二条及以后的 exec 出口 | 在已排内容下方另起一行，Branch 与 Sequence 都走这条 |
| 数据 feeder | 摆在消费者左边，Y 取「让这条线拉平」的值 |
| 同一个节点的多个 feeder | 依次往左错开，跨过前一个的整棵子树，谁都不用让出自己的水平线 |
| 入口走不到的节点 | 兜底停在结果下方，Save cached pose 与新建 BP 自带的禁用事件节点都在这里 |

`Spacing` 是列间距，`RowSpacing` 是行间距。

pose 图与状态机图的 `Nodes` 可以整个省掉，走缺省入口。

```json
{ "Op": "Arrange", "Graph": "AnimGraph" }
{ "Op": "Arrange", "Machine": "Locomotion" }
{ "Op": "Arrange", "Machine": "Locomotion", "Transition": { "From": "Idle", "To": "Walk" } }
```

#### Straighten

对应编辑器里选中节点按 Q。

```json
{ "Op": "Straighten", "Nodes": ["setInt", "sel", "math", "getBase"] }
```

最左边的节点当锚点不动，其余顺连线滑动到两端 pin 同一水平线。一个节点有多条连线时落在平均值上，因为一个 Y 满足不了全部。pose 连线同理。

#### 尺寸估算

`NodeWidth` / `NodeHeight` 要 Slate 画过一次才有值，headless 下新建的节点全是 0，靠尺寸和 pin 位置的 op 都得估：高度是 `标题栏 48 + 可见 pin 行数 × 24`，宽度按标题长度撑、下限 200，pin 的纵向位置是 `48 + 同向 pin 序号 × 24 + 半行`。只有一个可见 pin 的紧凑节点（`Get` 那种）没有标题行，pin 走中线。

固定常量兜底不行：`Set Collision Profile Name` 有 4 行 pin 实际约 144 高，按 100 排必然重叠。

不同 widget 家族一套估法：state / conduit / alias / entry 是圆角框，宽度按名字撑、下限 160，高 60；anim 节点在 pin 行之外，有绑定加一行、有隐藏可暴露属性再加一行；注释框用作者画的尺寸；transition 不排也不占地。

#### Stack / Align

Stack 保持第一个节点不动，其余按 spec 顺序排在它后面。

## EditAnimAsset

### Writer

划分与 `EditBlueprint` 同形，一个面配一个 writer。一个 target 只 load 一次资产，跑完 spec 点名的所有 writer，保存一次。任一 writer 失败整个 target 不落盘。

| Spec key | Writer | 写什么 |
| --- | --- | --- |
| `Notifies` | `FAnimNotifyWriter` | AnimNotify 与 AnimNotifyState 的增删改 |
| `Curves` | `FAnimCurveWriter` | float curve 的增删改名与关键帧写入 |
| `SyncMarkers` | `FAnimSyncMarkerWriter` | AuthoredSyncMarkers 的增删改，仅 AnimSequence |
| `Sections` | `FAnimMontageSectionWriter` | 段落的增删改名与 NextSection 链，仅 AnimMontage |
| `Slots` | `FAnimMontageSlotWriter` | slot 与 segment 的增删改，仅 AnimMontage |

执行顺序固定为 `Slots` → `Sections` → `Curves` → `SyncMarkers` → `Notifies`，与 spec 里的书写顺序无关。slot 决定 montage 长度，后面几个 writer 的时间字段都按这个长度校验。

`RefreshCacheData` 在 target 跑完后统一做一次，它会按时间重排 notify 与 sync marker 数组并重建面板的轨道列表。排序只在 target 收尾时做一次，不发生在 op 之间，所以同一个 target 内按 `Index` 寻址看到的是本次 spec 之前的顺序。

### 调用

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    EditAnimAsset "" 10 600 "-spec=C:/path/spec.json -apply"
```

无 `-apply` 是 dry run，照样跑完每个 writer，只跳过保存，所以校验强度与 `-apply` 一样；改动靠进程退出丢弃，编辑器开着走 queue 通道时 dry run 退 2。AssetList 传空字符串，target 写在 spec 里。

### Spec

```json
{
  "Targets": [
    {
      "AssetPath": "/Game/Path/AM_Foo",
      "Notifies": [
        { "Op": "Add", "Class": "/Script/Engine.AnimNotify_PlaySound", "TriggerTime": 1.52,
          "Track": 10,
          "Parameters": { "Sound": "/Game/Path/S_Bar.S_Bar", "bFollow": "True",
                          "AttachName": "root" } },
        { "Op": "Add", "Class": "/Game/Path/ANS_Telegraph", "TriggerTime": 0.4,
          "Duration": 0.447 },
        { "Op": "Modify", "Name": "PlaySound", "At": 0.106,
          "Parameters": { "AttachName": "Socket_NeckHead" } },
        { "Op": "Remove", "Name": "PlaySound", "At": 0.106 }
      ],
      "Curves": [
        { "Op": "Add", "Name": "Windup", "Flags": ["Editable"],
          "Keys": [ { "Time": 0.0, "Value": 0.0 },
                    { "Time": 0.5, "Value": 1.0, "InterpMode": "RCIM_Linear" } ] },
        { "Op": "Rename", "Name": "distance", "NewName": "Travel" },
        { "Op": "SetKeys", "Name": "Travel", "Keys": [ { "Time": 0.0, "Value": 0.0 } ] },
        { "Op": "Modify", "Name": "Travel", "Flags": ["Editable", "Metadata"] },
        { "Op": "Remove", "Name": "Windup" }
      ],
      "SyncMarkers": [
        { "Op": "Add", "Name": "L", "Time": 0.25, "Track": "FootSyncMarkers" },
        { "Op": "Modify", "Name": "L", "At": 0.25, "Time": 0.26 },
        { "Op": "Remove", "Name": "L", "At": 0.26 }
      ],
      "Sections": [
        { "Op": "Add", "Name": "Recover", "Time": 0.3, "NextSection": "Default" },
        { "Op": "Modify", "Name": "Default", "NextSection": "Recover" },
        { "Op": "Rename", "Name": "Recover", "NewName": "Recovery" },
        { "Op": "Remove", "Name": "Recovery" }
      ],
      "Slots": [
        { "Op": "Add", "SlotName": "UpperBody" },
        { "Op": "AddSegment", "SlotName": "UpperBody",
          "Sequence": "/Game/Path/AS_Swing.AS_Swing", "StartPos": 0.0, "PlayRate": 1.0 },
        { "Op": "ModifySegment", "SlotName": "UpperBody", "Index": 0, "PlayRate": 1.5 },
        { "Op": "RemoveSegment", "SlotName": "UpperBody", "Index": 0 },
        { "Op": "Remove", "SlotName": "UpperBody" }
      ]
    }
  ]
}
```

### Notifies

Op: `Add` / `Modify` / `Remove`，按 spec 顺序执行，后面的能看到前面的结果。

#### Add

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `Class` | named notify 以外必填 | 类路径、Blueprint 资产路径或裸类名，解析成 AnimNotify 或 AnimNotifyState |
| `TriggerTime` | 是 | 秒，必须落在资产长度内 |
| `Duration` | state notify 必填 | 给非 state notify 是错误，`TriggerTime + Duration` 也必须落在长度内 |
| `Track` | 否 | 默认 0，资产还没声明的轨道会补出来，notify 就落在 spec 指的那一行。写 `"auto"` 则挑第一条在这个时间点前后 0.05 秒内没东西的轨，都占了就追一条新的 |
| `Name` | 否 | 存下来的 NotifyName |
| `Parameters` | 否 | 见下 |

Blueprint notify 写资产路径就行，generated class 由它找出来。`Class` 省略则是 named notify，AnimBP 用 `AnimNotify_<Name>` 接的那种，这时 `Name` 就是它的全部内容。

`Name` 默认取类名去掉 `AnimNotify_` / `AnimNotifyState_` 前缀，与 notify 面板新建时写下的一致。不是 `GetNotifyName()`，那个被 PlaySound 一类重写成显示参数值，拿它寻址会落空。

#### Modify / Remove

两个 op 都寻址一个既有 notify，二选一：

| 方式 | 字段 | 说明 |
| --- | --- | --- |
| 位置 | `Index` | `AnimAssetExport` 打印的 `Notifies` 数组下标 |
| 名字 | `Name`，可加 `At` / `Track` 收窄 | `At` 是 trigger 时间 |

匹配到 0 个或 2 个以上都是错误，重跑不会悄悄改到另一个 notify 上。

`At` 按毫秒容差比。trigger 时间带 snap 偏移，从导出里抄回来的值与 spec 里写下的值不会精确相等。

Modify 写 `NewName` / `TriggerTime` / `Duration` / `Track` / `Parameters`，至少给一个。清掉一个 notify 用 `Remove`，不是写一个把它掏空的 Modify。

`Track` 在 Modify 里同时是寻址条件，用 `Name` 寻址时它被当成「现在在哪一行」。换行用 `Index` 寻址。

#### Parameters

按属性路径写到 notify 对象上，格式与 `AnimAssetExport` 打印的 `Parameters` 一致，导出的 notify 可以原样喂回来当 spec。named notify 没有对象，给它 `Parameters` 是错误。

### Curves

适用 AnimSequence 与 AnimMontage（montage 自己的 float curve）。写入走 animation data model controller，与曲线面板同一条路径。寻址一律按 `Name`。

| Op | 字段 | 说明 |
| --- | --- | --- |
| `Add` | `Name` 必填，`Flags` / `Keys` 选填 | 同名曲线已存在是错误。`Flags` 省略取 `Editable` |
| `Remove` | `Name` | |
| `Rename` | `Name` + `NewName` | `NewName` 已存在是错误 |
| `SetKeys` | `Name` + `Keys` | 整条替换，不是追加。清空曲线用 `Remove` |
| `Modify` | `Name` + `Flags` | 只写 flags，`Flags` 缺了就是空写 |

`Flags` 取 `Editable` / `Metadata` / `DriveTrack` / `Disabled` / `Material` / `Morph`，与 `AnimAssetExport` 打印的一致，导出的 `Flags` 数组可以原样喂回来。`Material` 与 `Morph` 是老资产上的遗留位。

`Keys` 每项要 `Time` 与 `Value`，`InterpMode` 选填，取 `RCIM_Linear` 一类的枚举名，也接受去掉 `RCIM_` 前缀的写法，省略是 `RCIM_Linear`。5.3 起曲线名是每资产的 `FName`，不需要先在 skeleton 上注册。

### SyncMarkers

仅 AnimSequence，喂一个 montage 是错误。

| Op | 字段 | 说明 |
| --- | --- | --- |
| `Add` | `Name` + `Time`，`Track` 选填 | |
| `Modify` | 寻址 + `NewName` / `Time` / `Track`，至少一个 | |
| `Remove` | 寻址 | |

寻址与 Notifies 同规则：`Index` 是 `AnimAssetExport` 打印的 `SyncMarkers` 数组下标，或者 `Name` 加 `At` 收窄，`At` 按毫秒容差比。匹配到 0 个或 2 个以上都是错误。

`Track` 取行号、`NotifyTracks` 里的行名，或 `"auto"`（挑第一条在这个时间点前后 0.05 秒内没 marker 的行，都占了就追一条）。sync marker 与 notify 共用同一批行。

### Sections

仅 AnimMontage，喂一个 sequence 是错误。寻址按 `Name`。

| Op | 字段 | 说明 |
| --- | --- | --- |
| `Add` | `Name` + `Time`，`NextSection` 选填 | 同名段落已存在是错误 |
| `Remove` | `Name` | 指向它的 `NextSection` 一并断开 |
| `Rename` | `Name` + `NewName` | 指向它的 `NextSection` 一并改名 |
| `Modify` | `Name` + `Time` / `NextSection`，至少一个 | |

`NextSection` 写一个既有段落名，或写 `"None"` 断链。指一个本次 spec 还没建出来的段落是错误。

段落在 target 收尾时按时间重排，并保证第一段落在 0，与 montage 编辑器同一条不变式。

### Slots

仅 AnimMontage，喂一个 sequence 是错误。slot 按 `SlotName` 寻址，同名多个 slot 无法寻址。

| Op | 字段 | 说明 |
| --- | --- | --- |
| `Add` | `SlotName` | 同名 slot 已存在是错误 |
| `Remove` | `SlotName` | montage 不能丢掉最后一个 slot |
| `AddSegment` | `SlotName` + `Sequence` + `StartPos`，`AnimStartTime` / `AnimEndTime` / `PlayRate` / `LoopingCount` 选填 | |
| `RemoveSegment` | `SlotName` + `Index` 或 `StartPos` | |
| `ModifySegment` | `SlotName` + `Index` 或 `StartPos`，再加要写的字段 | |

`Sequence` 是资产路径，必须与 montage 同 skeleton，且能进 composition（另一个 montage 不行）。省略的播放字段取被引资产的自然值：`AnimStartTime` 0、`AnimEndTime` 资产长度、`PlayRate` 1、`LoopingCount` 1。

`StartPos` 在 `ModifySegment` 里同时是寻址条件，要挪一个 segment 就用 `Index` 寻址。

slot 改完后在 target 收尾时按编辑器那条路走一遍：segment 按 `StartPos` 排序、`UpdateLinkableElements`、按 composition 重算长度、notify 与段落重新贴回 segment。

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 成功 |
| 1 | 参数错误、寻址不到、属性写不进去 |
| 2 | 编辑器在运行 |

## EditTextureAsset

用途: 按 spec 改贴图的构建设置。spec 形状与 `AuditTexture` 报告的 `Spec` 块一致，审计结果可以直接喂回。

### 调用

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    EditTextureAsset "" 10 600 "-spec=C:/path/spec.json"
```

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    EditTextureAsset "" 10 600 "-spec=C:/path/spec.json -apply"
```

无 `-apply` 是 dry run，属性照写只跳过保存，`.uasset` 不动。AssetList 传空字符串，target 写在 spec 里。

### Spec

```json
{
  "Targets": [
    {
      "AssetPath": "/Game/Path/T_Foo.T_Foo",
      "Properties": {
        "LODGroup": "TEXTUREGROUP_CharacterNormalMap",
        "CompressionSettings": "TC_Normalmap",
        "SRGB": "false",
        "MipGenSettings": "TMGS_NoMipmaps",
        "MaxTextureSize": "2048"
      }
    }
  ]
}
```

枚举写名字，数值写数字或数字字符串，两种都收。

### 可写属性

| 属性 | 说明 |
| --- | --- |
| `LODGroup` | `TextureGroup` 枚举名 |
| `CompressionSettings` | `TextureCompressionSettings` 枚举名 |
| `SRGB` | bool |
| `MipGenSettings` | `TextureMipGenSettings` 枚举名 |
| `MaxTextureSize` | 整数，0 = 不限 |
| `NeverStream` | bool |
| `VirtualTextureStreaming` | bool |
| `LossyCompressionAmount` | `ETextureLossyCompressionAmount` 枚举名，Oodle RDO 档位 |
| `bFlipGreenChannel` | bool |
| `PowerOfTwoMode` | `ETexturePowerOfTwoSetting` 枚举名 |
| `CompressionNoAlpha` | bool |
| `CompressionQuality` | `ETextureCompressionQuality` 枚举名 |
| `NumCinematicMipLevels` | 整数 |
| `LODBias` | 整数 |
| `Filter` | `TextureFilter` 枚举名 |
| `AddressX` | `TextureAddress` 枚举名 |
| `AddressY` | `TextureAddress` 枚举名 |

表外的属性名退 1，报错里列出整张表。属性名在这个 texture 类上不存在也退 1（`AddressX` / `AddressY` 只在 `Texture2D` 上）。

### 写入顺序

1. `PreEditChange(nullptr)`
2. 按 spec 写属性
3. `ApplyDefaultsForNewlyImportedTextures(Texture, true)`
4. `PostEditChange()`，随后 `FinishAllCompilation()`
5. `-apply` 时保存包

dry run 做完前四步，只跳过第 5 步。

第 3 步会把 pow2 且 `TMGS_NoMipmaps` + `TC_Default` / `TC_EditorIcon` 的非 UI 贴图退回 group 默认值。commandlet 检出这种回退后按 spec 再写一次并打 Warning，spec 优先。

### LODGroup 8BitData / 16BitData

引擎那套「切到 `TEXTUREGROUP_8BitData` / `TEXTUREGROUP_16BitData` 就强制改写 `CompressionSettings` / `SRGB` / `Filter` / `MipGenSettings`」只挂在带具体属性的 `PostEditChangeProperty` 上。本 commandlet 走无属性的 `PostEditChange()`，那套改写不触发，要那些值就在 spec 里显式写出来。

### 日志

每个目标每个属性打一行 `资产 属性: 改前 -> 改后`，值是 `PostEditChange` 之后回读出来的生效值。

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 成功 |
| 1 | 参数错误、spec 读不了、资产 load 不到、属性名不认识、枚举名写错 |
| 2 | 编辑器在运行 |

## EditMaterialAsset

用途: 按 spec 改材质。`UMaterial` 改 usage flag 与基本设定，`UMaterialInstanceConstant` 改 parent 与参数覆写。spec 形状与 `AuditMaterial` 报告的 `Spec` 块一致，审计结果可以直接喂回。

节点图不在范围内。增删连表达式走 Python 的 `unreal.MaterialEditingLibrary`，那边有完整脚本接口。

### 调用

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    EditMaterialAsset "" 10 600 "-spec=C:/path/spec.json"
```

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    EditMaterialAsset "" 10 600 "-spec=C:/path/spec.json -apply"
```

无 `-apply` 是 dry run，写入照做只跳过保存，`.uasset` 不动。AssetList 传空字符串，target 写在 spec 里。

### Spec

一个 target 是材质还是材质实例，由 `AssetPath` 指向的类决定，两边收的 key 不同。

```json
{
  "Targets": [
    {
      "AssetPath": "/Game/Path/M_Foo.M_Foo",
      "Properties": {
        "bUsedWithSkeletalMesh": "true",
        "bUsedWithNanite": "true",
        "TwoSided": "true",
        "BlendMode": "BLEND_Masked",
        "OpacityMaskClipValue": "0.5"
      }
    },
    {
      "AssetPath": "/Game/Path/MI_Foo.MI_Foo",
      "Properties": {
        "Parent": "/Game/Path/M_Foo.M_Foo",
        "ScalarParameters": { "Opacity": 0.75 },
        "VectorParameters": { "Tint": "(R=1.0,G=0.2,B=0.2,A=1.0)" },
        "TextureParameters": { "BaseColor": "/Game/Path/T_Bar.T_Bar" },
        "StaticSwitchParameters": { "UseDetail": true },
        "BasePropertyOverrides": { "TwoSided": true }
      }
    }
  ]
}
```

枚举写名字，bool 收 `true` / `false` 与字符串形式，数值收数字与数字字符串。

### Material 可写属性

| 属性 | 说明 |
| --- | --- |
| `bUsedWith*` | 23 个 usage flag 全收，名字与引擎的属性名一致，例如 `bUsedWithSkeletalMesh` / `bUsedWithNanite` / `bUsedWithNiagaraSprites` |
| `bAutomaticallySetUsageInEditor` | bool |
| `BlendMode` | `EBlendMode` 枚举名 |
| `MaterialDomain` | `EMaterialDomain` 枚举名 |
| `ShadingModel` | `EMaterialShadingModel` 枚举名，走 `SetShadingModel`，成员本身是 private |
| `TwoSided` | bool |
| `OpacityMaskClipValue` | 浮点 |

表外的属性名退 1，报错里列出整张表。

### bAutomaticallySetUsageInEditor 关着时的写法

`SetMaterialUsage` 在 `bAutomaticallySetUsageInEditor` 为 false 时是空转，这时只能直接写 `bUsedWith*` 位域，引擎自己在 `FbxSkeletalMeshImport` 里也是这么干的。commandlet 两条路都走：先按 `SetMaterialUsage` 试，读回来还不是目标值就用反射直接写。

同一个 spec 里既关 `bAutomaticallySetUsageInEditor` 又开某个 flag 时，前者先落，后者自动走直写那条路。

### MaterialInstanceConstant 可写 section

| Section | 说明 |
| --- | --- |
| `Parent` | 新 parent 的资产路径，走 `SetParentEditorOnly` |
| `ScalarParameters` | `{ 参数名: 数值 }` |
| `VectorParameters` | `{ 参数名: "(R=,G=,B=,A=)" }` |
| `TextureParameters` | `{ 参数名: 贴图资产路径 }` |
| `StaticSwitchParameters` | `{ 参数名: bool }` |
| `BasePropertyOverrides` | `BlendMode` / `TwoSided` / `ShadingModel` / `OpacityMaskClipValue`，写了哪个就打开哪个 `bOverride_` |

参数名以 parent 为准。parent 上没有这个名字就退 1，报错里列出 parent 实际有的同类参数名。

### 写入顺序

Material。

1. `PreEditChange(nullptr)`
2. `bAutomaticallySetUsageInEditor`
3. usage flag
4. 其余基本设定
5. `PostEditChange()`，加进整轮共用的一个 `FMaterialUpdateContext`
6. `-apply` 时保存包

MaterialInstanceConstant。

1. `Parent`
2. 参数与 `BasePropertyOverrides` 全部暂存
3. 一个 `FMaterialInstanceParameterUpdateContext` 一次性落，析构时只跑一次 `UpdateStaticPermutation`
4. `MarkPackageDirty`
5. `-apply` 时保存包

dry run 做完除保存外的全部步骤。整轮共用一个 `FMaterialUpdateContext`，spec 里几个材质就只有一次 renderer 更新。

### 日志

每个目标每个写入打一行 `资产 属性: 改前 -> 改后`，usage flag 那行的改后值是回读出来的生效值，能看出走的是 `SetMaterialUsage` 还是直写。

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 成功 |
| 1 | 参数错误、spec 读不了、资产 load 不到、key 不认识、枚举名写错、参数名不在 parent 上 |
| 2 | 编辑器在运行 |

任一 target 失败整轮不落盘。

## 约束

commandlet 没有 UI，没有 selection 状态。任何依赖"当前选中"的编辑器操作在这里不成立，对齐一类必须由 spec 显式点名节点。

Python 侧对 EdGraph 暴露极少（`UbergraphPages` / `parent_class` / `Notifies` 均不可读），这一组只能是 C++ commandlet。
