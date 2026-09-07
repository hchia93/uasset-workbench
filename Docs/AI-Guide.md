# AI Guide

给 agent 用的调用手册，覆盖日常调用。

契约在 Docs。参数、spec 形状、退出码、JSON 字段一律以对应组的 `Docs/<Group>.md` 为准，commandlet header 只留一句用途加一行用法，末行指回它的 Docs。本手册是这些 Docs 的入口。

## 决策表

| 我要知道 / 我要做什么 | RunName | 组 |
| --- | --- | --- |
| 看 Blueprint 里的连线逻辑、节点、函数 | `BlueprintEdGraphExport` | Export |
| 看 Widget 的布局、控件树、动画 | `WidgetLayoutExport` | Export |
| 看 AnimSequence / Montage 的 notify，Montage 另有 section、slot | `AnimAssetExport` | Export |
| 看 DataTable 的行结构与数值 | `DataTableExport` | Export |
| 看 DataAsset 上的属性配置 | `DataAssetExport` | Export |
| 看关卡里摆了什么、碰撞与静态网格配置 | `LevelExport` | Export |
| 看 AnimBP 的状态机与转换条件 | `AnimBlueprintExport` | Export |
| 看 BT 的树结构、节点参数、Blackboard key | `BehaviorTreeExport` | Export |
| 看材质表达式或 MI 的参数覆写 | `MaterialExport` | Export |
| 看 Niagara 的 emitter、script、renderer | `NiagaraSystemExport` | Export |
| 看贴图的压缩、sRGB、LOD group、源尺寸 | `TextureExport` | Export |
| 用代码搭 Widget 布局，或按 spec 重建控件树 | `WidgetLayoutImport` | Import |
| 设 widget 上 `EditDefaultsOnly` 的属性，那些不在控件树里 | `WidgetLayoutImport` 的 `ClassDefaults` | Import |
| 批量填 DataAsset 的属性 | `DataAssetImport` | Import |
| 从零建一批资产，并让它们互相接线 | `CreateAsset` | Import |
| 给既有 BP 加组件、加变量、建节点接线、设默认值 | `EditBlueprint` | Edit |
| 一次改完同一个 BP 的多个方面，要么全成要么不落盘 | `EditBlueprint` | Edit |
| 生成完的图要排版，不想手写坐标 | `EditBlueprint` 的 `Layout` `Arrange` | Edit |
| 连线歪了要拉平，等同编辑器里按 Q | `EditBlueprint` 的 `Layout` `Straighten` | Edit |
| 从零建一台状态机，连 state 与 transition | `EditBlueprint` 的 `StateMachines` | Edit |
| 往 AnimGraph 或某个 state 里加 sequence player / slot 并接 pose | `EditBlueprint` 的 `Graph`，`Type` 写类路径 | Edit |
| 改 anim graph 节点的设定（sequence player 的 PlayRate / StartPosition、state 的 function binding） | `EditBlueprint` 的 `NodeProperties` | Edit |
| 图里要建 cast、Make / Break Struct、Switch、SpawnActor、Timeline，或 override 一个父类事件 | `EditBlueprint` 的 `Graph`，节点类型表见 [Edit.md](Edit.md) | Edit |
| 建一条 `TimeRemaining < x` 形态的 transition rule | `EditBlueprint` 的 `Graph`，`Type` 写 `AnimGetter` | Edit |
| 把 anim node 的某个属性绑到变量或属性路径上，等同 Details 面板的 Bind 下拉框 | `EditBlueprint` 的 `Graph` 的 `Bind` | Edit |
| 让 anim node 的某个属性露出 pin，或把绑上之后自动露出来的 pin 收回去 | `EditBlueprint` 的 `Graph` 的 `ExposePins` | Edit |
| 建一个函数，或按导出的 `Signature` 改它的参数、局部变量、access 与 flag | `EditBlueprint` 的 `Functions` | Edit |
| 删掉一个函数图，连同它的调用点 | `EditBlueprint` 的 `Functions` + `Graph` 的 `Delete` | Edit |
| 加事件分发器，或改它的签名 | `EditBlueprint` 的 `Dispatchers` | Edit |
| 让 BP 实现或撤掉一个接口 | `EditBlueprint` 的 `Interfaces` | Edit |
| 改既有变量的类型、分类、复制这些 flag | `EditBlueprint` 的 `Variables` 的 `Modify` | Edit |
| 改既有状态机: 改 state 或 transition 的字段、改名、删、加 conduit 或 alias | `EditBlueprint` 的 `StateMachines` | Edit |
| 状态机与 pose 图要排版 | `EditBlueprint` 的 `Layout`，认 anim schema | Edit |
| 改 UMG 控件的名字，C++ 侧 `BindWidget` 改名后要让 WBP 跟上 | `EditBlueprint` 的 `Widgets` | Edit |
| 改 UMG 控件或它 slot 的属性，不想整树替换 | `EditBlueprint` 的 `Widgets` | Edit |
| 给 AnimSequence / AnimMontage 加 notify，或改既有 notify 的时间、轨道、参数 | `EditAnimAsset` 的 `Notifies` | Edit |
| 把 `AnimAssetExport` 导出的 notify 参数改完喂回资产 | `EditAnimAsset` | Edit |
| 改 AnimSequence 的曲线关键帧或 sync marker | `EditAnimAsset` 的 `Curves` / `SyncMarkers` | Edit |
| 改 Montage 的段落链，或 slot 里的分段与 play rate | `EditAnimAsset` 的 `Sections` / `Slots` | Edit |
| 改贴图的 LOD group、压缩格式、sRGB、mip、尺寸上限 | `EditTextureAsset` | Edit |
| 改材质的 usage flag、BlendMode、ShadingModel，或 MI 的 parent 与参数覆写 | `EditMaterialAsset` | Edit |
| C++ 改名后 BP 事件不再触发 | `RedirectBlueprintEvent` | Migrate |
| delegate 参数改名后绑定处留下悬空连线 | `RedirectBlueprintPin` | Migrate |
| 图逻辑搬进 C++ 后，BP 图里作废的那几个节点要删掉 | `DeleteBlueprintNode` | Migrate |
| 批量改 Blueprint 的父类 | `ReparentBlueprint` | Migrate |
| 资产改名或搬目录，引用要跟着走 | `RenameAsset` | Migrate |
| 在动手前先复制一份资产做对照 | `DuplicateAsset` | Migrate |
| 让 CoreRedirect 解析过的引用落盘，好撤掉那条 redirect | `ResaveAsset` | Migrate |
| 资产改名后，把别的 level 的 import 改指到新资产 | `SanitizeLevelReference` | Migrate |
| 想知道有没有 level 引用了已经不存在的资产 | `AuditLevelReference` | Audit |
| 提交前 gate 一次破损引用检查 | `AuditLevelReference` | Audit |
| 想知道哪个 level 是 persistent，哪些是 sublevel | `AuditLevelTopology` | Audit |
| 想知道哪些贴图的压缩 / sRGB / group / 尺寸设错了 | `AuditTexture` | Audit |
| 想知道哪些材质缺 usage flag、跟 Nanite 不兼容，或者哪个材质会拖慢 PIE 进入 | `AuditMaterial` | Audit |
| 想测每个 sublevel 的加载卸载耗时 | `run_stream_metric.ps1` | 脚本 |
| 想看每个 level 的组件数是否超预算 | 先 `LevelExport`，再 `level_budget_audit.py` | Export + 脚本 |
| 想改 `CreateDefaultSubobject` 的名字，不想让派生 BP 留下孤儿组件 | 先 `rename_default_subobject.py`，再改 C++ | 脚本 |

拿不准 Blueprint 里的问题出在 C++ 还是图上时，先 `BlueprintEdGraphExport` 看图，再决定。

破损引用先 audit 后 sanitize: `AuditLevelReference` 拿到明细，再用 `SanitizeLevelReference` 逐对重指向。

写入类的 run（Migrate / Import / Edit）在编辑器开着时跑，脚本会走 in-editor 队列。编辑器关着时脚本 fallback 到独立进程，写出的包 asset registry 建不出依赖边，细节见 [Migrate.md](Migrate.md)。

## 调用模板

Export。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    BlueprintEdGraphExport \
    "/Game/Blueprints/BP_Foo,/Game/Blueprints/BP_Baz"
```

Import。三个 commandlet 都用 `-spec=` 指向 spec 绝对路径，走 `EXTRA_ARGS`，`AssetList` 传空串。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    WidgetLayoutImport \
    "/Game/UI/WBP_Bar" \
    10 600 \
    '-spec="C:/temp/WBP_Bar.spec.json"'
```

`DataAssetImport` 同形，换 RunName 与资产路径即可。

`CreateAsset` 额外要求 `-unattended`。已存在的资产会被跳过并报 warning，不会覆盖，结尾统计里 created 与 already existed 分开计数。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    CreateAsset \
    "" \
    10 600 \
    '-spec="C:/temp/create.spec.json" -unattended'
```

Migrate。两个 redirect 与 `DeleteBlueprintNode` 默认 dry run，确认输出无误后补 `-apply`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    RedirectBlueprintEvent \
    "/Game/Blueprints/BP_Foo" \
    10 600 \
    '-OwnerClass="/Script/MyModule.MyActor" -OldEvent="OldName" -NewEvent="NewName"'
```

`DeleteBlueprintNode`。资产路径只放 `AssetList`，node id 从 `BlueprintEdGraphExport` 加 `-graphs` 的导出产物里的 `NodeId` 原样抄，纯十六进制加逗号，不需要引号。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    DeleteBlueprintNode \
    "/Game/Blueprints/BP_Foo" \
    10 600 \
    '-nodes=A1B2C3D4E5F64A7B8C9D0E1F2A3B4C5D,0F1E2D3C4B5A69788796A5B4C3D2E1F0'
```

Audit。`AuditLevelReference` 与 `AuditLevelTopology` 不吃 `AssetList`，位置参数留空串。`AuditTexture` 与 `AuditMaterial` 的入口资产写 `AssetList`，不给就按 `-scandir` 扫。

## 跑完了去哪看

编辑器开着时走 queue 路径，每个 run 在 Message Log 的 `UAsset Workbench` 面板下开一页，页名就是 run 名。run 自己的每一行日志（`LogUAssetWorkbench*` 全部六个 category）都镜像到那一页，按 Info / Warning / Error 分级，可以用面板顶部的 filter 只看 warning 和 error。

页是历史，跑完不会消失，之后回头翻得到。摘要行在页尾，带 warning 与 error 计数。只要计数非零就弹 toast，干净的 run 不打扰。

编辑器关着时走 commandlet 路径，没有 UI，记录在 `Saved/Logs/` 下的日志文件里。

摘要行**只在 run 确实由 `AssetList` 驱动时**才带资产名。spec 驱动的 run（`Create*` / `*Import` / `Edit*`）摘要不带名字，因为它们碰了哪些资产写在 spec 里，位置参数说了不算。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    AuditLevelReference \
    "" \
    10 600 \
    '-scandir="/Game" -report="C:/temp/level_ref.json"'
```

## 结果怎么读

Export。

输出: `Intermediate/UAssetExport/<AssetPath>_r<revision>_<YYYYMMDD-HHMMSS>.json`，同一资产的多次导出互不覆盖，读最新那份
先 grep 定位关心的节点标题、函数名、控件名，再按行号区间读。

```bash
grep -n "MyFunctionName" Intermediate/UAssetExport/Game/Blueprints/BP_Foo_r*.json
```

Import 与 Migrate。

判定依据: wrapper 退出码
细节: 编辑器开着看 Message Log 的 `UAsset Workbench` listing，编辑器关着看 `Saved/Logs/` 与 `Saved/UAssetWorkbenchTaskQueue/last_commandlet.log`
例外: `WidgetLayoutImport` 的退出码会被引擎改写，以日志里的 `Imported layout into ...` 为准

Audit。

判定依据: 退出码 0 干净，3 有问题
明细: 报告 JSON，路径由 `-report` 决定，默认在 `Intermediate/<RunName>/` 下

## 坑

| 坑 | 处理 |
| --- | --- |
| 编辑器开着时直接起 commandlet | 一律走 wrapper。commandlet 检测到活的 heartbeat 会退出码 2 自保 |
| 在 `EXTRA_ARGS` 里写 `-assets=` | 入口资产只走位置参数 `AssetList`。wrapper 由它生成 `-assets`，queue 路径由任务 json 的 `Assets` 数组构造，两边在 `AssetList` 为空时都不发这个 flag |
| 导出的 JSON 可能上万行 | 先 grep 定位再按行号区间读，不要整份读进上下文 |
| `RedirectBlueprintEvent`、`RedirectBlueprintPin`、`DeleteBlueprintNode` 默认不写盘 | 先读 dry run 输出，确认命中的资产与节点，再加 `-apply` |
| `DeleteBlueprintNode` 的 node id 靠手写或从图上认 | id 只能从 `BlueprintEdGraphExport` 加 `-graphs` 的导出里原样抄。没命中的 id 会在结尾统一报出来，别当成删干净了 |
| 以为删了节点，只被它引用的变量也跟着没了 | 删节点只切连线，不重新接线也不动变量。失去引用的 Blueprint 变量另外处理，schema 拒删的 function entry / result 会被跳过并报警 |
| `Edit*` 不给 `-apply` 时编辑器还开着 | `EditBlueprint` 与 `EditAnimAsset` 的 dry run 靠进程退出丢弃内存里的改动，走 queue 通道没有这层保护，直接退 2。关掉编辑器跑 commandlet，或者确认无误直接 `-apply` |
| 两次导出做 diff 时 `K2Node_MathExpression` 一片红 | 它每次加载重建内部图并换 `NodeGuid`，`SubGraphs` 子树的差是既有非确定性不是回归。比对时排除这棵子树 |
| `WidgetLayoutImport` 是整树替换 | spec 必须描述完整的树，不是增量补丁 |
| 用 `WidgetLayoutImport` 改控件名 | 整树替换靠同名保 GUID，改名会让 widget animation 的绑定悬空且不报错。控件改名走 `EditBlueprint` 的 `Widgets` 的 `Rename` |
| `WidgetLayoutImport` 退出码 1 但资产其实写进去了 | 引擎会改写 commandlet 的退出码。判断成败以日志里的 `Imported layout into ...` 为准，别只看退出码 |
| `CreateAsset` 漏了 `-unattended` | 引擎的 `FMessageDialog` 不检查 commandlet 模式，某些创建路径会弹窗把进程挂住。这个参数必填 |
| `CreateAsset` 建 Texture2D 拿到空结果 | 静默失败，先查 `FactoryProperties` 有没有给 `Width` / `Height`，且必须是 2 的幂 |
| 想改材质的节点图，或增删 DataTable 的行 | 走 Python 更合适。`unreal.MaterialEditingLibrary` 与 `unreal.DataTableFunctionLibrary` 都有完整的脚本接口，后者的 `export_data_table_to_json_string` / `fill_data_table_from_json_string` 是一对 round-trip，commandlet 那边反而绕 |
| 导出产物不入版本控制 | `Intermediate/UAssetExport` 是临时目录，需要留证据就自行拷走 |
| 把 Audit 的退出码 3 当成失败 | 3 不是失败，运行本身成功，只是报告里有要处理的东西。跑不起来才是 1 |
| `AuditLevelReference` 报出一大堆破损 | 先确认项目的插件全部启用。未挂载 content root 下的依赖会被判成 missing，那是假破损 |
| `SanitizeLevelReference` 在旧资产删掉之后才跑 | 来不及了，换指针需要新旧两边都还在磁盘上。必须在删除旧资产之前跑 |
| 分不清 log 属于哪一组 | `LogUAssetWorkbenchExporter` / `LogUAssetWorkbenchImporter` / `LogUAssetWorkbenchMigrator` / `LogUAssetWorkbenchAuditor` 各对应一组，调度与公共部分在 `LogUAssetWorkbenchCore` |
| 编辑器开着就跑 stream metric | 跑之前编辑器必须关闭，脚本会直接报错退出 |
| 担心探针改 streaming class 污染 level | 只在内存里换成 `LevelStreamingDynamic`，不落盘 |
| `AuditMaterial` 加了 `-stats` 跑到天亮 | 二档逐材质编译代表性 shader，一个材质几十秒到几分钟。扫描模式已经压到前 20 个，仍然慢。只要某几个材质的数字就把它们写进 `-assets=`，点名的不受上限管；只要规则不要数字就别加 `-stats` |
| `AuditMaterial` 报出成片的 U3 | U3 是「flag 开着但扫描范围里没找到消费者」，缩小的扫描范围本身就会造出一批。先把范围放到 `/Game` 再看，U3 不进 `Spec`，也不会被 `EditMaterialAsset` 自动关掉 |
| `level_budget_audit.py` 结论对不上现状 | 它只读 `LevelExport` 的产物，产物过期就得到过期结论。脚本会打印导出日期，先看那个 |
