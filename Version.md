# Version

版本定义: `src/Source/UAssetWorkbench/Public/UAssetWorkbenchVersion.h`，同时写进每份导出 JSON 的 `ExporterVersion`

## 2.x · UAssetWorkbench

### 2.6.7

- 新增 `EditLevel`，按编辑器 label 找到关卡里摆放的 actor 写属性，路径能穿过数组、struct 与 instanced 子对象，Python 写不进去的 inline 对象 `EditInstanceOnly` 属性也收
- 编辑器内队列等编辑器启动完成才接任务，启动前就排进来的任务不再在引擎初始化途中执行
- 启动时丢弃崩溃编辑器留在 `processing/` 的任务，不再重跑
- `run_commandlet.sh` 超时或 heartbeat 过期放弃等待时，撤回还在 `pending/` 的任务
- AI-Guide 补上关编辑器后 15 秒内调 wrapper 的陷阱

### 2.6.6

- `EditBlueprint` 的 `WidgetAnimations` 新增 `ReplaceBinding` / `DeleteBinding`，对应编辑器动画绑定的右键菜单，按名字从绑定表寻址，控件已删、显示 missing 的绑定也找得到
- 同位替换控件: 新控件就位，`ReplaceBinding` 把轨道挪过去，再删旧控件，三步可以写在同一个 spec 里
- 修正 Edit 文档: `Widgets` 的 `Delete` 不清 widget animation 绑定，也不清指向原生 `BindWidget` 属性的 Get / Set 节点

### 2.6.5

- 新增 `scripts/editor_session.py`，全机共享的编辑器登记表，多个 agent 与同一项目的多份 checkout 起停编辑器互不干扰
- `exec_in_editor.py` 只连本项目的编辑器，同时开着多个时用 `--editor <N>` 指定
- AI-Guide 补上编辑器会话的用法

### 2.6.4

- 编辑器内排队的 run 以 unattended 模式执行，引擎弹窗不再卡住 game thread
- 失败 run 的通知带上第一条 error
- `RenameAsset` 失败日志列出源资产，并补上重启编辑器的步骤

### 2.6.3

- 修复 `EditBlueprint` 的 `Widgets` `Add` 未登记新控件 GUID，编译 Blueprint 时触发 ensure

### 2.6.2

- 属性写入支持 instanced 子对象，给 `Class` 新建实例，不给则改现有实例
- 覆盖 `DataAssetImport`、`CreateAsset`、`EditBlueprint`、`EditAnimAsset`、`EditDataTable`、`EditPCGGraph`

### 2.6.1

- `WidgetLayoutExport` 导出动画的绑定、轨道、属性路径与关键帧
- `EditBlueprint` 的 `WidgetAnimations` 能从零建动画，新增 `Add` / `Delete` / `Rename` / `SetPlaybackRange` / `AddTrack` / `DeleteTrack`
- 轨道类按属性类型选，建轨道时连带建覆盖播放区间的 section

### 2.6.0

- 新增 PCG 支持: `PCGGraphExport`、`PCGCatalogExport`、`EditPCGGraph`、`AuditPCG`，直接读写 PCG graph 的运行时模型，不依赖编辑器 UI
- 新建 PCG graph 走 `CreateAsset` 建空图，再用 `EditPCGGraph` 填

### 2.5.4

- 新增 `EditDataTable`，按行名写既有行的属性值，与 `DataTableExport` 互通
- 属性路径可以以裸 struct 为根

### 2.5.3

- `EditBlueprint` 的 `Widgets` 新增 `Add` / `Delete` / `Reparent`，改控件树结构不必整树 `WidgetLayoutImport`
- 新增 `WidgetAnimations` writer，`SetKeys` 改既有动画通道的关键帧，与 `WidgetLayoutExport` 互通
- 新增脚本 `rename_default_subobject.py`，C++ default subobject 改名前先改掉各 Blueprint 里序列化的旧名，避免留下孤儿组件
- 修复 `EditBlueprint` 报错文本漏列 `Widgets`

### 2.5.2

- `EditBlueprint` 新增 `Widgets` writer，`Rename` / `Modify` 控件与 slot 属性，改名连带 GUID 映射与动画绑定

### 2.5.1

- 编辑器存活判定: heartbeat mtime 过期时改查文件里记的编辑器 pid，asset registry 扫描或 shader 编译造成的卡顿不再被误判为编辑器已关
- wrapper 报 heartbeat 过期前重读 done 文件，同一秒落地的结果不再被丢弃
- **不兼容** 队列目录 `Saved/UAssetExportQueue/` 更名为 `Saved/UAssetWorkbenchTaskQueue/`

### 2.5.0

- 新增 `RenameAsset`，改名或搬路径时硬引用与软引用一起重指，逐个资产报改名前后的 referencer 数，redirector 默认 fixup 后删除

### 2.4.0

- **不兼容** `AnimMontageExport` 扩成 `AnimAssetExport`，覆盖 AnimSequence 与 AnimMontage，新增曲线、sync marker、轨道名、root motion
- **不兼容** `EditAnimMontage` 扩成 `EditAnimAsset`，新增 `Curves`、`SyncMarkers`、`Sections`、`Slots`
- `EditBlueprint` 新增 `Functions`、`Dispatchers`、`Interfaces`、`StateMachines` writer
- `EditBlueprint` 的 `Graph` 支持 25 种节点类型、`Bind` 与 `ExposePins`，`Layout` 的 `Arrange` 认 pose 图与状态机
- Blueprint、AnimBlueprint、Widget 三个导出共用一份 EdGraph 序列化器，新增函数 `Signature`、变量元数据、`EventDispatchers`、`Timelines`、anim 节点 `Settings` / `Bindings` / `ExposedPins`、transition `RuleSummary`
- 新增 `EditTextureAsset`、`EditMaterialAsset`
- 新增 `AuditTexture`、`AuditMaterial`，报告里的 `Spec` 块直接喂给对应的 Edit

### 2.3.3

- `EditBlueprint` 的 `Graph` 新增 `Delete`，同一次运行可以先改道连线再删节点

### 2.3.2

- 新增 `EditAnimMontage`，增删改 AnimMontage 的 notify

### 2.3.1

- 编辑器内的每次 run 在 Message Log 的 `UAsset Workbench` 面板开一页，有 warning 或 error 才弹 toast
- `CreateAsset` 必须带 `-unattended`，已存在的资产跳过并单独计数

### 2.3.0

- 新增 Edit 组: `EditBlueprint`，`Components`、`Variables`、`Defaults`、`Graph`、`Layout` 五个面，一个资产一次 load、一次编译保存
- 新增 `DuplicateAsset`

### 2.2.0

- `LevelExport` 导出非组件的 instanced 子对象，WorldSettings 的导航配置这类内容此前被静默漏掉

### 2.1.0

- 新增 `DeleteBlueprintNode`，按 node id 删图节点，用于图逻辑搬进 C++ 之后的清理

### 2.0.0

- **不兼容** 插件由 `UAssetJsonExporter` 更名为 `UAssetWorkbench`，并入 UAssetOps，从只读导出扩成 Export / Import / Migrate / Audit 四组
- Import: `WidgetLayoutImport`、`DataAssetImport`、`CreateAsset`
- Migrate: `RedirectBlueprintEvent`、`RedirectBlueprintPin`、`ReparentBlueprint`、`ResaveAsset`、`SanitizeLevelReference`
- Audit: `AuditLevelReference`、`AuditLevelTopology`
- 脚本: `exec_in_editor.py`、`level_budget_audit.py`，以及 `run_stream_metric.ps1` 牵头的 stream metric 工作流

## 1.x · UAssetJsonExporter

### 1.9.0

- `AnimBlueprintExport` 导出 transition 的优先级、逻辑类型与 blend 设置，逐节点的动画资产与非默认设定，以及 property access 绑定路径

### 1.8.0

- 新增 `TextureExport`，导出压缩、sRGB、LOD group、mip、源尺寸

### 1.7.3

- `NiagaraSystemExport` 导出 static switch

### 1.7.2

- `MaterialExport` 导出生成的 HLSL

### 1.7.1

- `NiagaraSystemExport` 解出 module 输入值，导出 emitter 的 module stack 与曲线 data interface

### 1.7.0

- `BlueprintEdGraphExport` 导出实现的接口

### 1.6.1

- `WidgetLayoutExport` 导出 UMG 动画关键帧
- 修复独立 commandlet 自锁，commandlet 进程不再创建编辑器内队列的 subsystem

### 1.5.0

- 编辑器开着也能导出: wrapper 按 heartbeat 路由，开着走编辑器内 subsystem，关着走 commandlet，两边产出一致
- 修复插件加载阶段过晚导致 commandlet 不可用
- wrapper 只结束自己起的编辑器进程

### 1.4.0

- `BlueprintEdGraphExport` 导出解析后的属性与 Actor CDO 属性，节点带位置、宏与变量引用
- `LevelExport` 导出 custom primitive data 与顶点色 LOD

### 1.2.0

- `BlueprintEdGraphExport` 新增资产摘要: 组件树、节点与变量计数、是否含逻辑

### 1.1.0

- 新增 `LevelExport`，导出 actor 与 component、与 archetype 的差异属性、碰撞、静态网格与 ISM 摘要、streaming level

### 1.0.0

- 首版，九个只读导出: `BlueprintEdGraphExport`、`AnimMontageExport`、`WidgetLayoutExport`、`DataAssetExport`、`DataTableExport`、`NiagaraSystemExport`、`MaterialExport`、`BehaviorTreeExport`、`AnimBlueprintExport`
- wrapper `run_commandlet.sh`，输出稳定后自动结束编辑器进程
- `BlueprintEdGraphExport` 导出 CDO 属性覆写、继承组件覆写与组件的静态网格属性
- 修复批量导出每次只出一个文件
