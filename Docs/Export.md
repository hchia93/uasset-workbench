# Export

读 uasset 结构，导出成可 grep 的 JSON。

## Commandlet

| RunName | 导出内容 |
| --- | --- |
| `BlueprintEdGraphExport` | Blueprint 图、节点、pin、连线 |
| `AnimAssetExport` | AnimSequence / Montage 的 notify，Montage 另有 section、slot |
| `WidgetLayoutExport` | Widget 树、布局、动画、EdGraph |
| `DataAssetExport` | DataAsset 子类属性 |
| `DataTableExport` | DataTable 行结构与全部行数据 |
| `NiagaraSystemExport` | Niagara emitter、script、renderer |
| `MaterialExport` | Material 表达式与连线，MI 的参数覆写 |
| `TextureExport` | Texture 属性，压缩、sRGB、LOD group、mip、源尺寸 |
| `BehaviorTreeExport` | BT 树结构、节点参数、Blackboard key |
| `AnimBlueprintExport` | AnimBP EdGraph、状态机的状态、转换、blend 设置、入口状态与 event 绑定 |
| `LevelExport` | Level 的 actor / component、与 archetype 的差异属性、碰撞与静态网格与 ISM 摘要、streaming level |
| `PCGGraphExport` | PCG graph 的节点、pin、连线、settings delta、参数、子图引用、comment；graph instance 出参数覆写 |
| `PCGCatalogExport` | 可用 PCG 节点类的字典：pin 表、属性表、预配置项，加 `-scandir` 下的 graph、settings 资产、Blueprint element、data asset |

## 调用

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    WidgetLayoutExport \
    "/Game/UI/WBP_Bar,/Game/UI/WBP_Baz"
```

一次可以传多个资产，逗号分隔，同一次运行共用一个 RunName。

`PCGCatalogExport` 不吃 `AssetList`，位置参数留空串，`-filter=` 走 `EXTRA_ARGS`。输出固定落 `Intermediate/UAssetExport/PCGCatalogExport_rNA_<YYYYMMDD-HHMMSS>.json`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    PCGCatalogExport \
    "" \
    10 600 \
    '-filter=Spawner'
```

## 详略开关

| 参数 | 适用 | 效果 |
| --- | --- | --- |
| `-graphs` | `BlueprintEdGraphExport` | `Graphs[]` 展开到 `Nodes` 与 pin |
| `-full` | `PCGGraphExport` | `Nodes[].Settings` 出全量属性，默认只出相对 CDO 的 delta |
| `-filter=` | `PCGCatalogExport` | 按类名、标题、分类、别名的子串过滤 `Native[]`，必给 |
| `-scandir=` | `PCGCatalogExport` | 资产段扫描范围，默认 `/Game` |

不给 `-graphs` 时 `Graphs[]` 只有图级字段与 `Signature`，一个中等 Blueprint 从几千行降到几十行。`DeleteBlueprintNode` 要的 `NodeId` 只在展开态里有。

其余 commandlet 没有开关，一律全量。`AnimBlueprintExport` 与 `WidgetLayoutExport` 的图恒定展开。

## 输出

路径: `Intermediate/UAssetExport/<AssetPath>_r<revision>_<YYYYMMDD-HHMMSS>.json`

`revision` 是该资产在版本控制里的 last-changed revision，取不到时写 `rNA`。戳在写入成功后才落，失败的导出不占用文件名。同一资产的多次导出互不覆盖，读的时候取最新那份。
版本控制: 不入库

每份 JSON 都带这两个通用字段，其余按资产类型展开。

| 字段 | 含义 |
| --- | --- |
| `ExporterVersion` | 产出这份 JSON 的插件版本 |
| `ExportType` | 资产类型标识，决定后面的结构怎么读 |

完成判定看输出文件: 全部存在，且 mtime 稳定 `IDLE_SEC` 秒。

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 全部输出就位 |
| 1 | 输出缺失或执行失败，wrapper 打印 log 尾部 |
| 2 | 编辑器在运行 |

Export 组不会返回 3，3 是 Audit 组专用。

## 读取策略

导出的 JSON 可能非常大，一个中等复杂度的 Blueprint 就能到几千行。

1. 先 grep 定位关心的节点标题、函数名、控件名、行名
2. 拿到行号后按区间读，不要整份读进上下文

```bash
grep -n "OnHealthChanged" Intermediate/UAssetExport/Game/Blueprints/BP_Foo_r*.json
```

## EdGraph 通用形状

`BlueprintEdGraphExport` / `AnimBlueprintExport` / `WidgetLayoutExport` 共用一份 EdGraph writer，图、节点、pin 三层的键在三份产物里逐字相同。同一张图经不同 commandlet 导出的结果可以直接 diff。

约定: 默认值一律不发。布尔键出现即为 `true`，看不到该键就是 false，不要按 `== false` 判。

### 图对象

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `Name` | 恒发 | `UEdGraph` 对象名，函数图即函数名 |
| `GraphType` | 恒发 | 图分类，取值见下表 |
| `NodeCount` | 恒发 | 节点总数，含被过滤掉的隐藏节点 |
| `HasLogic` | 恒发 | 图内是否存在任意一条连线，用来在精简态区分空壳图与真逻辑图 |
| `Nodes` | 仅展开态 | 节点数组 |
| `Signature` | 函数类图 | 函数签名块，精简态也发 |
| `Interface` | 接口实现图 / AnimLayer | 所实现接口名 |

`GraphType` 全部取值。

| 值 | 来源 |
| --- | --- |
| `EventGraph` | 三个 commandlet 的 Ubergraph |
| `Function` / `Macro` / `DelegateSignature` / `InterfaceFunction` | `BlueprintEdGraphExport` |
| `AnimGraph` / `AnimLayer` / `State` / `Conduit` / `TransitionRule` / `CustomTransition` | `AnimBlueprintExport` |
| `Composite` / `StateMachine` / `BlendSpace` / `SubGraph` | 递归进 `SubGraphs` 时按 owning node 类判 |

### 节点对象

通用键，每个节点都有。

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `NodeId` | 恒发 | 节点 GUID，`LinkedTo` 与 `DeleteBlueprintNode` 引用的就是它 |
| `Class` | 恒发 | 节点 UClass 短名，如 `K2Node_CallFunction` |
| `Title` | 恒发 | 编辑器里的完整标题，可能含换行 |
| `PosX` / `PosY` | 恒发 | 图上坐标 |
| `Pins` | 恒发 | pin 数组，隐藏 pin 已剔除 |
| `Comment` | 非空 | 节点气泡注释 |
| `Enabled` | 非 Enabled | 恒为 `false`，正常节点不发此键 |
| `EnabledState` | 与 `Enabled` 成对 | `Disabled` / `DevelopmentOnly` |

K2 节点专属键，按节点类追加，一个节点可命中多组。

| 字段 | 节点类 | 含义 |
| --- | --- | --- |
| `VariableName` / `VariableOwner` | `K2Node_Variable` | 变量名与所属类短名 |
| `MacroName` / `MacroPackage` | `K2Node_MacroInstance` | 宏图名与所在 package |
| `FunctionName` / `FunctionOwner` | `K2Node_CallFunction` | 被调函数名与所属类短名 |
| `Pure` / `Const` / `Interface` / `SelfContext` | `K2Node_CallFunction` | 各自为 true 时才发 |
| `CastTarget` / `PureCast` | `K2Node_DynamicCast` | 目标类完整路径，纯 Cast 标记 |
| `EventName` | `K2Node_Event` / `K2Node_CustomEvent` | 事件名，custom event 读 `CustomFunctionName` |
| `CallInEditor` / `NetFlags` | `K2Node_CustomEvent` | `NetFlags` 取 `Server` / `Client` / `Multicast` / `Reliable` |
| `ComponentPropertyName` / `DelegatePropertyName` | `K2Node_ComponentBoundEvent` | 绑定的组件与委托属性名 |
| `DelegatePropertyName` / `EventOwner` | `K2Node_ActorBoundEvent` | `EventOwner` 是完整路径 |
| `TimelineName` | `K2Node_Timeline` | Timeline 名 |
| `SelectedFunctionName` | `K2Node_CreateDelegate` | 委托绑到的函数名 |
| `SpawnClass` | `K2Node_SpawnActorFromClass` | 生成类完整路径，只在 Class pin 是字面量时发 |
| `IndexPinType` | `K2Node_Select` | 对象，`Type` 加可选 `SubType` |
| `SwitchType` / `Enum` | `K2Node_Switch*` | `Enum` / `Integer` / `String` / `Name`，枚举资产完整路径 |
| `InputAction` / `InputKey` | `K2Node_InputAction` / `K2Node_InputKey` | 旧输入系统的 Action 名与按键名 |
| `Reroute` | `K2Node_Knot` | 恒为 `true` |

Anim 节点专属键。

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `AnimationAsset` | 该 anim node 引用了资产 | 完整路径 |
| `Settings` | 反射出的字段非空 | `FAnimNode` 结构体全量字段，不是 delta，默认值也在里面 |
| `Bindings` | 该节点有 property binding | 属性名到绑定对象的映射 |
| `ExposedPins` / `HiddenOptionalPins` | 非空 | 可选属性里暴露成 pin 与没暴露的两组名字 |
| `PropertyPath` / `PropertyPathSegments` / `ResolvedPath` | `K2Node_PropertyAccess` | 显示文本、分段、`.` join 后的解析路径 |

`Bindings` 每项。

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `Path` | 恒发 | `.` 连接的属性路径 |
| `PathAsText` | 恒发 | 显示文本 |
| `Type` | 恒发 | `Property` / `Function` / `None` |
| `bIsBound` / `bIsPromotion` | 恒发 | 布尔，false 也发 |
| `ContextId` | 非空 | 上下文 ID |
| `CompiledContext` | 非空 | 编译后上下文，`Thread Safe` 说明过了 anim 编译器 |
| `ArrayIndex` | 非 `INDEX_NONE` | 数组下标 |

调用参数与子图。

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `Args` | `K2Node_CallFunction` / `K2Node_MacroInstance` 有生效值 | 平铺对象，pin 名到生效值。只收未连线、非隐藏、非 exec 的输入 pin，值优先取 `DefaultObject` 路径。`PlaySound2D` 放的是哪个 sound 从这里一眼读出 |
| `SubGraphs` | 展开态且该节点有子图 | 嵌套图对象数组，结构与顶层图对象一致，最深 8 层，已访问过的图不重复展开 |

### pin 对象

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `Name` | 恒发 | pin 名 |
| `Direction` | 恒发 | `Input` / `Output` 两值 |
| `Type` | 恒发 | pin 类别，如 `exec` / `object` / `int` |
| `SubType` | `PinSubCategoryObject` 有效 | 子类型对象短名 |
| `SubCategory` | `PinSubCategory` 非 None | 子类别 FName，与 `SubType` 各自独立，可同时出现 |
| `Container` | 非 None | `Array` / `Set` / `Map` |
| `ValueType` | `Container` 是 `Map` | Map 的 value 端类型对象 |
| `Default` | 非空 | 字面默认值 |
| `DefaultText` | 非空 | FText 默认值 |
| `DefaultObject` | 非空 | 默认对象完整路径 |
| `AutogeneratedDefault` | 与 `Default` 不一致 | 只在两者有差时发，这个差就是用户改过值的标志 |
| `Advanced` / `Reference` / `Orphaned` | 为 true | 折叠在高级区 / by-ref / 签名变更后残留 |
| `LinkedTo` | 有连线 | 连线数组，每项 `NodeId` / `NodeTitle` / `PinName` |

### Signature

函数类图恒发，`Inputs` 与 `Outputs` 恒发即使为空数组。签名可以原样贴进 `EditBlueprint` 的 `Functions` spec。

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `Inputs` / `Outputs` | 恒发 | 入参与返回值数组 |
| `SignatureSource` | 签名读自节点 pin | 恒为 `Inherited`。override 与接口实现函数的 `UserDefinedPins` 为空，参数来自继承的 `UFunction`，此时回退读 entry / result 节点 pin |
| `LocalVariables` | 非空 | 局部变量数组 |
| `Pure` / `Const` / `Static` / `CallInEditor` / `Override` | 为 true | 各自的函数 flag |
| `Access` | 有 entry 节点 | `Public` / `Protected` / `Private` |
| `Category` / `Keywords` / `Tooltip` | 非空 | 函数元数据 |

`Inputs` / `Outputs` / `LocalVariables` 每项。

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `Name` | 恒发 | 参数名 |
| `Type` | 恒发 | pin 类别 |
| `SubType` / `SubTypePath` | `PinSubCategoryObject` 有效，成对出现 | `SubType` 是读者认得的裸名，`SubTypePath` 是 `EditBlueprint` spec 吃的完整路径 |
| `Container` | 恒发 | 非容器也发 `None`，与 pin 对象的规则相反 |
| `IsReference` | `Inputs` / `Outputs` 恒发 | `LocalVariables` 不发此键 |
| `Default` | 非空 | 默认值。`SignatureSource` 是 `Inherited` 时不发 |

## BlueprintEdGraphExport

`ExportType`: `BlueprintEdGraph`

### 顶层字段

| 字段 | 含义 |
| --- | --- |
| `BlueprintType` | `Normal` / `Const` / `MacroLibrary` / `Interface` / `FunctionLibrary` |
| `ParentClass` / `ParentClassPath` / `GeneratedClassPath` | 类身份 |
| `ImplementedInterfaces[]` / `ImplementedInterfaceCount` | BP 的 Implements Interface 列表，每项 `Name` / `Path` |
| `Variables[]` / `VariableCount` / `UserVariableCount` | 生成类的成员属性，并入 `NewVariables` 的元数据: `Category` / `PinType` / `SubType` / `Container` / `ValueType` / `Guid` 与各 flag |
| `EventDispatchers[]` / `EventDispatcherCount` | `Name` / `Category` / `Signature`，同时也出现在 `Variables[]` 里 |
| `Timelines[]` / `TimelineCount` | 长度、播放 flag、float / vector / color / event 轨道与曲线路径 |
| `Components[]` | SCS 组件树 |
| `InheritedComponents[]` | 父 BP 的 SCS 组件，不在 `Components[]` 里但 `Defaults` spec 的 `Component` 可以点名 |
| `InheritedComponentOverrides[]` | 本 BP 没声明的组件上的 delta |
| `ActorCDOProperties[]` / `ActorCDOOverrides[]` | BP 类 CDO 的 actor 级字段，全量与相对父类的 delta |
| `Graphs[]` | 见下 |
| `MacroGraphCount` / `DelegateSignatureGraphCount` / `InterfaceGraphCount` | 按图种类的计数 |
| `Referencers_Levels` / `Referencers_Other` | 反向依赖，按 level 与非 level 分开 |
| `ReferencedAssets` | 正向依赖 |

`Components[]` 每项。

| 字段 | 含义 |
| --- | --- |
| `Name` / `Class` | 组件名与类 |
| `IsRoot` / `ParentName` / `AttachSocket` / `IsEditorOnly` | 树上的位置 |
| `PropertyOverrides` | 相对组件类 CDO 的 delta |
| `ResolvedProperties` | 全量解析后的字段，读者逐字段比对不必从 delta 反推 |

`InheritedComponentOverrides[]` 有两种来源，看 `Source`。

| `Source` | 含义 |
| --- | --- |
| 缺省 | C++ native 默认子对象在 BP CDO 上的 delta |
| `ParentBlueprint` | 父 BP 组件的 override，另带 `ParentBlueprint` 指出属于哪个类 |

### Graphs 分类

| `GraphType` | 来源 |
| --- | --- |
| `EventGraph` | `UbergraphPages` |
| `Function` | 函数图 |
| `Macro` | 宏图 |
| `DelegateSignature` | 事件分发器的签名图 |
| `InterfaceFunction` | 接口实现图，带 `Interface` 字段 |

`Function` / `Macro` / `DelegateSignature` / `InterfaceFunction` 四类恒带 `Signature`。

## AnimBlueprintExport

`ExportType`: `AnimBlueprint`

### 顶层字段

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `AnimBlueprintName` / `AssetPath` / `ExportTimestamp` | 恒发 | 资产身份 |
| `ParentClass` / `ParentClassPath` | 有父类 | 成对出现 |
| `TargetSkeleton` | 非空 | 骨架资产路径 |
| `ImplementedInterfaces[]` | 恒发 | 每项 `Name` / `Path` / `IsAnimLayerInterface` |
| `Graphs[]` | 恒发 | Ubergraph 加 FunctionGraphs，分类见下 |
| `StateMachines[]` | 恒发 | 全部状态机，含嵌套，打平成一层 |

状态机自己那张图不进 `Graphs[]`，它的内容已经在 `StateMachines[]` 里展开。

### FunctionGraphs 分类

| `GraphType` | 判定 |
| --- | --- |
| `EventGraph` | Ubergraph |
| `AnimGraph` | 图名等于 `AnimGraph` |
| `AnimLayer` | 命中某个 `UAnimLayerInterface` 子接口的同名函数，或函数带 `MD_AnimBlueprintFunction` 元数据，或图类是 `UAnimationGraph` |
| `Function` | 以上都不是的普通 K2 函数图，唯一带 `Signature` 的一档 |

`AnimLayer` 只在走接口那条判定时带 `Interface` 字段，自有 layer 没有接口可指，缺这个键是合法的。图里第一个 root 节点的 group 非空时另发 `LayerGroup`。

### StateMachines

嵌套机器不嵌套输出，全部打平进顶层数组，层级靠 `Parent` 回指。

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `StateMachineName` | 恒发 | 状态机节点标题 |
| `EntryState` | 入口有连线 | 入口指向的状态名 |
| `States` | 恒发 | 普通 state、conduit、alias 混在同一数组 |
| `Transitions` | 恒发 | 该机器内全部 transition |
| `StateCount` | 恒发 | `States` 长度，已含 conduit 与 alias |
| `TransitionCount` / `ConduitCount` / `AliasCount` | 恒发 | 分类计数 |
| `Parent` | 嵌套机器 | 对象，`Machine` 是父状态机节点标题，`State` 是承载本机器的那个 state 名 |

### States

三种 `StateType` 走三套字段。

| 字段 | 适用 | 含义 |
| --- | --- | --- |
| `StateName` / `NodeId` / `StateType` | 三种都有 | 普通 state 的 `StateType` 是枚举名 `AST_SingleState`，conduit 与 alias 是字面量 `Conduit` / `Alias` |
| `Comment` | 三种都有，非空才发 | 节点注释 |
| `bAlwaysResetOnEntry` | 普通 state，为 true 才发 | 进入时重置 |
| `Events` | 普通 state | 事件与函数绑定，见下 |
| `BoundGraph` | 普通 state 与 conduit | state 装 pose 图，conduit 装 transition rule |
| `bGlobalAlias` | alias，为 true 才发 | 全局别名，此时不发 `AliasedStates` |
| `AliasedStates` | alias 且非全局 | 被别名的状态名数组，已排序保证两次导出可比对 |

`Events` 的键名全部由反射得出，不是固定表，UE 改名或加 hook 时导出跟着变。值一律是扁平字符串。

| 来源 | 典型键名 |
| --- | --- |
| state 节点自身的 notify | `StateEntered` / `StateLeft` / `StateFullyBlended` |
| bound graph 里 result 节点的成员引用 | `InitialUpdateFunction` / `BecomeRelevantFunction` / `UpdateFunction` / `StateEntryFunction` / `StateFullyBlendedInFunction` / `StateExitFunction` / `StateFullyBlendedOutFunction` |

这批名字与 `EditBlueprint` 的 `StateMachines` `ModifyState` 输入键一一对应，见 [Edit.md](Edit.md)。

### Transitions

默认值一律不发，避免把唯一一条非默认设置埋进噪音里。

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `NodeId` | 恒发 | transition 节点 GUID |
| `FromState` / `ToState` | 端点存在 | 端点节点的完整标题 |
| `CrossfadeDuration` / `BlendMode` / `PriorityOrder` / `LogicType` | 恒发 | blend 与优先级 |
| `bAutomaticRuleBasedOnSequencePlayerInState` / `AutomaticRuleTriggerTime` | 前者为 true 时成对发 | 自动规则 |
| `bDisabled` / `Bidirectional` / `bAllowInertializationForSelfTransitions` | 为 true | 三个独立开关 |
| `MinTimeBeforeReentry` | 非负 | 负值是关闭态，不发 |
| `SyncGroupNameToRequireValidMarkersRule` | 非 None | 同步组名 |
| `bSharedRules` / `SharedRulesName` / `SharedRulesGuid` | 共享规则组 | 三个一起发 |
| `SharedCrossfade` / `SharedCrossfadeName` / `SharedCrossfadeGuid` | 共享 crossfade 组 | 三个一起发。键名没有 `b` 前缀，与 `bSharedRules` 不对称 |
| `BlendProfile` / `BlendProfileMode` | 有 blend profile | 资产路径与模式 |
| `CustomBlendCurve` | 非空 | 曲线资产路径 |
| `CustomTransitionGraph` | `LogicType` 是 Custom | 嵌套图对象 |
| `Events` | 非空 | 反射块，节点自身的 notify 属性名即 `TransitionStart` / `TransitionEnd` / `TransitionInterrupt`，另加 result 节点的成员引用 |
| `TransitionRule` | 有 rule 图 | 嵌套图对象 |
| `RuleSummary` | rule 能归纳 | 见下 |

### RuleSummary

一眼读出这条 transition 靠什么进入，不必展开 `TransitionRule` 图。形态标签就是键名本身，同一个对象里只会出现一种。

| 形态键 | 判定 | 附带键 |
| --- | --- | --- |
| `Bound` | 结果 pin 上挂了 property binding | `Type`，取 `Property` / `Function` / `None` |
| `Constant` | 结果 pin 没有连线 | 无。值是 `DefaultValue` 原始字符串，不是布尔 |
| `Getter` | 驱动来自 `K2Node_AnimGetter` | `State` / `Machine` / `Compare` / `Threshold` |
| `PropertyAccess` | 驱动来自 property access 路径 | `Compare` / `Threshold` |
| `Variable` | 驱动来自 `K2Node_VariableGet` | 无 |
| `Custom` | 多驱动或以上都不匹配 | 无。值恒为布尔 `true`，读者转去看 `TransitionRule` 图 |

`Compare` 是比较节点的标题，`Threshold` 是它第一个未连线且有默认值的输入 pin。两者只在 getter 或 property access 经过一层比较节点时才有。

## AnimAssetExport

`ExportType`: `AnimSequence` 或 `AnimMontage`

### 通用字段

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `AssetName` / `AssetPath` / `ExportTimestamp` | 恒发 | 资产身份 |
| `Skeleton` | 非空 | 骨架资产路径 |
| `SequenceLength` / `RateScale` / `bLoop` | 恒发 | 播放设置 |
| `Curves[]` / `TransformCurves[]` | 有曲线 | 见下 |
| `NotifyTracks[]` | 有轨道 | 每项 `Index` / `Name` / `Color`，用来把 `Notifies[].TrackIndex` 反查成轨道名 |
| `Notifies[]` | 恒发 | AN 与 ANS 合并列表 |

`Notifies[]` 每项: `NotifyName` / `TriggerTime` / `Duration` / `TrackIndex` / `IsState`，加非纯骨骼通知才有的 `NotifyClass`，加反射出的 `Parameters` 对象。`Parameters` 可以改完喂回 `EditAnimAsset`。

### Curves

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `Curves[].Name` | 恒发 | 曲线名 |
| `Curves[].Flags` | 非空 | `Editable` / `Metadata` / `Material` / `Morph` / `DriveTrack` / `Disabled` |
| `Curves[].KeyCount` | 恒发 | 完整关键帧数，不受截断影响 |
| `Curves[].Keys` | 恒发 | 每项 `Time` / `Value` / `InterpMode`，每条曲线最多 64 个 |
| `Curves[].KeysTruncated` | 超过 64 个才发 | 恒为 `true`。`KeyCount` 与 `Keys` 长度对不上就是被截了 |
| `TransformCurves[].Name` / `.KeyCount` | 恒发 | 平移旋转缩放三条子曲线的最大帧数，不出明细 |

数据模型无效时 `Curves` 与 `TransformCurves` 都不发。

### AnimSequence 专属

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `NumberOfFrames` / `FrameRate` | 数据模型有效 | 帧数与十进制帧率 |
| `Interpolation` | 恒发 | `Linear` / `Step` |
| `bEnableRootMotion` / `RootMotionRootLock` / `bForceRootLock` / `bUseNormalizedRootMotionScale` | 恒发 | root motion 四值 |
| `AdditiveAnimType` | 恒发 | 叠加类型 |
| `RefPoseType` / `RefFrameIndex` / `RefPoseSeq` | `AdditiveAnimType` 非 `AAT_None` | 非叠加动画完全不发这三个键 |
| `RetargetSource` / `RetargetSourceAsset` | 非空 | 重定向源名与软引用路径 |
| `BoneCompressionSettings` / `CurveCompressionSettings` | 非空 | 压缩设置资产路径 |
| `SyncMarkers[]` | 非空 | 每项 `Name` / `Time` / `TrackIndex` |

Montage 没有 sync marker，那是 sequence 独有的。

### AnimMontage 专属

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `Sections[]` | 恒发 | 每项 `Name` / `StartTime` / `SectionIndex`，加设了下一段才有的 `NextSection` |
| `SlotTracks[]` | 恒发 | 每项 `SlotName` 与 `Segments[]` |
| `SlotTracks[].Segments[]` | 恒发 | `AnimSequence` / `AnimSequencePath` / `StartPos` / `AnimStartTime` / `AnimEndTime` / `AnimPlayRate` |
| `BlendIn` / `BlendOut` | 恒发 | 对象，`Time` / `Option` 加非空才有的 `CustomCurve` |
| `BlendInTime` / `BlendOutTime` | 恒发 | 与 `BlendIn.Time` / `BlendOut.Time` 重复，保留给只要一个数的读者 |
| `BlendOutTriggerTime` / `bEnableAutoBlendOut` | 恒发 | 负的 trigger time 表示自动 |
| `BlendProfileIn` / `BlendProfileOut` | 非空 | blend profile 资产路径 |
| `TimeStretchCurveName` | 非 None 且非类默认值 | 类默认值是一个真实名字，只按 None 过滤会让每个 montage 都带上这个键 |
| `SyncGroup` / `SyncSlotIndex` | 有同步组时成对发 | 同步组名与槽索引 |

`Sections[].NextSection` 缺席即为段落结束，不要按空串判。

## WidgetLayoutExport

`ExportType`: `WidgetLayout`

| 字段 | 含义 |
| --- | --- |
| `WidgetBlueprint` / `AssetPath` / `ParentClass` | 资产身份 |
| `WidgetTree` | 控件树，逐层嵌套 |
| `Animations[]` | 见下 |
| `Graphs[]` | EdGraph，形状见 [EdGraph 通用形状](#edgraph-通用形状) |

`Animations[]` 每项。

| 字段 | 含义 |
| --- | --- |
| `Name` | 动画显示名，`EditBlueprint` 的 `Animation` 字段抄它 |
| `StartTime` / `EndTime` / `Duration` | playback range，秒 |
| `DisplayRate` / `TickResolution` | 时间轴帧率与内部刻度 |
| `Bindings[]` | 每项 `Widget` / `Guid` / `IsRootWidget`，slot 绑定另发 `SlotWidget` |
| `Tracks[]` | 见下 |

`Tracks[]` 每项。

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `BoundWidget` | 恒发 | 轨道绑定的控件名 |
| `TrackType` | 恒发 | 轨道类完整路径 |
| `TrackName` | 恒发 | 编辑器时间轴上显示的名字，`SetKeys` 的 `TrackName` 抄它 |
| `PropertyName` / `PropertyPath` | 属性轨道 | `AddTrack` 的 `PropertyPath` 抄后者 |
| `Sections[]` | 恒发 | 每项 `RowIndex` / `StartTime` / `EndTime` / `Channels[]` |

`Channels[]` 每项 `Name` 与 `Keys[]`，key 带 `Time` 秒、`Value`、`Interp`。单通道轨道的 `Name` 是 `None`，那也是 `SetKeys` 里要填的值。

## LevelExport

`ExportType`: `Level`

只序列化非默认属性，即相对 `UObject::GetArchetype()` 的 delta，与 `.umap` 自己持久化的内容一致，完整度不打折而体积最小。

| 字段 | 含义 |
| --- | --- |
| `LevelName` / `PackageName` | level 身份 |
| `Actors[]` / `ActorCount` | 每个 actor 的 `Name` / `Label` / `Class` / `Folder` / `Tags` / `Loc` / `Rot` / `Scale` / `AttachParent` / `Socket` / `IsEditorOnly` |
| `Actors[].Components[]` | 组件列表 |
| `WorldSettings` | world settings 的 delta |
| `StreamingLevels[]` | 每项 `Name` / `Class` / `ShouldBeLoaded` / `ShouldBeVisible` |

组件的碰撞、网格、mobility 关键字段被提到组件对象的顶层方便 grep: `CollisionEnabled` / `CollisionProfile` / `GenerateOverlapEvents` / `Mobility` / `StaticMesh` / `NumVertices` / `CustomPrimitiveData`。其余 override 落在 `DeltaProperties` 里。

| 情况 | 输出 |
| --- | --- |
| ISM / HISM / Foliage 实例数不超过 200 | `Instances[]` 逐实例 transform |
| 实例数超过 200 | 只记 `InstanceCount` / `InstanceBounds` / `InstanceSamples`，采样 5 个 |
| 非组件的 instanced 子对象，如 WorldSettings 的 NavigationSystemConfig | 按属性名嵌套，写绝对值不写 delta |

顶点色 LOD 另发 `VertexPaintLODs`，每项 `LOD` / `ColorCRC32`。

## MaterialExport

`ExportType`: `Material` 或 `MaterialInstance`

派发先试 `UMaterialInstance`，命中就走实例路径，所以 `UMaterialInstanceConstant` 一定走实例分支。

### Material

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `MaterialName` / `AssetPath` / `ExportTimestamp` | 恒发 | 资产身份 |
| `ShadingModel` / `BlendMode` / `TwoSided` / `MaterialDomain` | 恒发 | 基本设定。`ShadingModel` 只取第一个，多着色模型材质在这里看不全 |
| `OpacityMaskClipValue` | 恒发 | 遮罩裁剪阈值 |
| `bAutomaticallySetUsageInEditor` | 恒发 | 决定缺 usage flag 时编辑器会不会自愈，`AuditMaterial` 的 U2 与 U4 靠它分叉 |
| `UsageFlags` | 恒发 | 开着的 `bUsedWith*` 属性名数组，反射取，可为空数组 |
| `Expressions[]` | 恒发 | 节点图 |
| `OutputConnections` | 至少一个引脚有连线 | 材质输出节点的输入连接表 |

`Expressions[]` 每项: `Name` / `Class` / `Description` 恒发；参数节点另发 `ParameterName` / `Group` 与按类型分叉的 `DefaultValue` 或 `DefaultTexture`；`MaterialExpressionCustom` 另发 `Code` / `OutputType` 与非空才发的 `AdditionalDefines` / `IncludeFilePaths` / `AdditionalOutputs`；有连线的输入进 `Inputs[]`，每项 `InputName` / `ConnectedTo` / `ConnectedClass` / `OutputIndex`。

`OutputConnections` 的键是引脚名，覆盖 `BaseColor` / `Metallic` / `Specular` / `Roughness` / `Normal` / `EmissiveColor` / `Opacity` / `OpacityMask` / `WorldPositionOffset` / `AmbientOcclusion` 十个，每个值是 `Expression` / `ExpressionClass` / `OutputIndex`。

改材质节点图走 Python 的 `unreal.MaterialEditingLibrary`，`EditMaterialAsset` 只改设定不动图。

### MaterialInstance

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `MaterialInstanceName` / `AssetPath` / `ExportTimestamp` | 恒发 | 资产身份 |
| `Parent` / `ParentPath` | 有父材质，成对发 | 短名与完整路径 |
| `ScalarParameters[]` | 恒发 | 每项 `Name` / `Value` |
| `VectorParameters[]` | 恒发 | 每项 `Name` / `Value`，`Value` 是 `(R=,G=,B=,A=)` 字符串不是数组 |
| `TextureParameters[]` | 恒发 | 每项 `Name` / `Texture`，贴图非空时另有 `TexturePath` |
| `StaticSwitchParameters[]` | 恒发 | 每项 `Name` / `Value` / `Override`，`Override` 说明是否真覆盖了父级 |
| `BasePropertyOverrides` | 恒发 | 对象，什么都没覆盖时是 `{}` |

`BasePropertyOverrides` 的四个子键各自由对应的 `bOverride_` 标志单独门控: `BlendMode` / `ShadingModel` / `TwoSided` / `OpacityMaskClipValue`。

## PCGGraphExport

`ExportType`: `PCGGraph` 或 `PCGGraphInstance`

PCG graph 不是 EdGraph。`UPCGGraph` 自己持有节点、pin 与边，编辑器图只是打开窗口时建的镜像，导出直接读运行时模型。

### 顶层字段

`PCGGraph`。

| 字段 | 含义 |
| --- | --- |
| `GraphName` / `AssetPath` / `ExportTimestamp` | 资产身份 |
| `GraphSettings` | 图级设定：`bUseHierarchicalGeneration` / `HiGenGridSize` / `HiGenExponential` / `bUse2DGrid` / `bHasDefaultConstructedInputs` / `bLandscapeUsesMetadata` / `bIgnoreLandscapeTracking` / `bIsEditorOnly` / `bIsStandaloneGraph` / `bDebugFlagAppliesToIndividualComponents` / `Category` / `Description` / `GenerationRadii`，值是 ExportText 字符串 |
| `Parameters[]` | graph parameter，每项 `Name` / `Type` / `TypeObject` / `CppType` / `Value` |
| `Nodes[]` / `NodeCount` | 见下 |
| `Edges[]` / `EdgeCount` | 扁平边表，每项 `From` 与 `To`，各带 `Node` / `Pin`，`From` 是上游 |
| `Subgraphs[]` | 每项 `Graph` / `Nodes[]`，调用自己的另标 `Recursive` |
| `Comments[]` | 每项 `Text` / `PositionX` / `PositionY` / `Width` / `Height` |
| `ExtraEditorNodeCount` | 旧版 comment 对象计数 |

`Nodes[]` 每项。

| 字段 | 何时发 | 含义 |
| --- | --- | --- |
| `NodeId` | 恒发 | 节点的 object name，`EditPCGGraph` 用它寻址。不是 guid，复制粘贴出的节点会换名 |
| `Title` | 恒发 | 编辑器显示的单行标题 |
| `AuthoredTitle` | 用户改过标题 | 用户输入的原名 |
| `ElementType` | 恒发 | `Input` / `Output` / `Native` / `Subgraph` / `Blueprint` / `SettingsInstance` |
| `PositionX` / `PositionY` | 恒发 | 图上位置 |
| `Comment` | 非空 | 节点气泡注释 |
| `Enabled` | 恒发 | `false` 即 bypass |
| `SettingsClass` / `SettingsType` | 有 settings | 类完整路径与 `EPCGSettingsType` 分类 |
| `SettingsAsset` | `SettingsInstance` | 引用的外部 settings 资产 |
| `Seed` | 节点用 seed | 节点 seed |
| `Subgraph` | `Subgraph` | 被调用的 graph 路径 |
| `BlueprintElement` | `Blueprint` | element 类路径 |
| `Settings` | 有非默认值 | 子类属性相对 CDO 的 delta，`-full` 时全量。instanced 子对象嵌套成 `Class` / `Properties` |
| `InputPins[]` / `OutputPins[]` | 恒发 | 见下 |

pin 每项: `Label` / `AllowedTypes` / `Usage` / `PinStatus` / `AllowMultipleData` / `AllowMultipleConnections` / `Connected` / `EdgeCount`，接了线另发 `CurrentTypes`，隐藏 pin 另发 `Invisible`。`PinStatus` 为 `Required` 的输入 pin 没接线就是 `AuditPCG` 的 P1。

`PCGGraphInstance` 顶层: `GraphInstanceName` / `AssetPath` / `Parent`（直接指向的 graph 或 instance）/ `ResolvedGraph`（最终解析到的 graph）/ `Parameters[]`，每项比 graph 的多一个 `Overridden`。

## PCGCatalogExport

`ExportType`: `PCGCatalog`

不吃 `AssetList`。`Native[]` 由反射枚举全部 `UPCGSettings` 子类，跳过 abstract / deprecated / hidden 与 Procedural Vegetation 专属节点，与 PCG 编辑器的节点面板同源。

| 字段 | 含义 |
| --- | --- |
| `Filter` / `ScanDir` | 本次运行的参数 |
| `Native[]` / `NativeCount` | 过滤后的节点类 |
| `Graphs[]` | `-scandir` 下的 `PCGGraph` 与 `PCGGraphInstance` 资产，每项 `AssetPath` / `Class` |
| `SettingsAssets[]` | 独立的 settings 资产 |
| `DataAssets[]` | `PCGDataAsset` |
| `BlueprintElements[]` | 父类是 PCG Blueprint element 的 Blueprint，另发 `NativeParent` |

`Native[]` 每项。

| 字段 | 含义 |
| --- | --- |
| `Class` / `Name` | 完整路径与短名，`EditPCGGraph` 的 `Class` 抄 `Class` |
| `Title` / `Category` / `Tooltip` | 编辑器里的显示名、分类、说明 |
| `Aliases[]` | 旧名别名 |
| `ExposeToLibrary` | 假的不出现在节点面板 |
| `Preconfigured[]` / `OnlyPreconfigured` | 预配置项，每项 `Label` / `Index`，`EditPCGGraph` 的 `Preconfigured` 二选一填 |
| `InputPins[]` / `OutputPins[]` | 默认 pin 表，形状同 `PCGGraphExport` 的 pin 去掉连接状态 |
| `Properties[]` | 可写属性，每项 `Name` / `CppType` / `Category` / `Default`，另按情况发 `Overridable`（可被 pin 覆盖）/ `EditCondition` / `Tooltip` / `Options[]`（枚举可选值）/ `Instanced` 加 `Class`（instanced 子对象） |
