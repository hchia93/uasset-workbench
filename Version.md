# Version

版本定义: `src/Source/UAssetWorkbench/Public/UAssetWorkbenchVersion.h`，同时写进每份导出 JSON 的 `ExporterVersion`

## 2.x · UAssetWorkbench

### 2.7.8

- 新增 `EditBlackboard`，增删改 Blackboard 自己的 key，写 key 类型属性（如 Object 的 `BaseClass`），换 parent。继承来的 key、重名、parent 成环退 1
- 挂 parent 时去掉 parent 已有的持久 key（`SelfActor`），与编辑器一致
- `EditBehaviorTree` 的说明改为 key selector，Blackboard 自己的 key 归 `EditBlackboard`
- README 支持表加 Blackboard 一行

### 2.7.7

- `BehaviorTreeExport` 补上基类节点在 Details 面板的设置：decorator 的 `FlowAbortMode` / `bInverseCondition`，service 的 `Interval` / `RandomDeviation`，task 的 `bIgnoreRestartSelf`，composite 的 `bApplyDecoratorScope`
- `EditBehaviorTree` 在图重建后回读写入的值。引擎因节点位置改掉的值（如 Sequence 下的 `LowerPriority` abort）报错退 1，不存盘

### 2.7.6

- 新增 `SoundExport`，导出 SoundCue / SoundWave / MetaSound 的 sound class、submix、attenuation、concurrency 与 voice 设定，SoundCue 另带节点图
- 新增 `AuditSound`，规则 S1-S3 查没设 concurrency、3D 用途缺 attenuation、sound class 为空。只在 SoundCue 或 MetaSound 里播放的 SoundWave 跳过
- README 支持表补上 Sound 一行

### 2.7.5

- 新增 `EditBehaviorTree`，经编辑器图增删挪 composite / task / decorator / service，改节点属性、blackboard key 与 blackboard 资产，运行时树从图重建。没有图的树先建图
- key selector 收裸 key 名并按 blackboard 校验，`ValueOrBBKey` 字段收裸值
- `BehaviorTreeExport` 每个节点带 `Id`，导出旁边写一份 `<导出>.spec.json`，原样喂回为零改动
- README 支持表 Behavior Tree 的 Edit 列标 ✓

### 2.7.4

- 新增 `RenameMaterialParameter`，改基材质的参数名，跟进每个 MI 的覆写（含 static switch），并改写 BP 里按字面名字设这个参数的调用
- 新增 `RenameNiagaraUserParameter`，改 user 参数名，跟进 system 内的 module 链接、binding、renderer，并改写 BP 调用与 Niagara 组件模板上的覆写
- 默认只列清单，`-apply` 才改写。BP 范围从资产往引用方自动收集，`-blueprints=` 可指定。名字来自连线的调用只报不改
- README 支持表 Material / MaterialInstance 与 Niagara System 的 Migrate 列标 ✓

### 2.7.3

- 新增 `AuditNiagara`，规则 FX1-FX5 查 GPU emitter 用动态 bounds、没设 effect type、warmup 过长、启用的 emitter 过多、renderer 不支持 sim target，字段名同 `NiagaraSystemExport`
- `AuditNiagara` 的 `Spec` 块直接喂给 `EditNiagaraSystem`，修完再审，带建议值的规则为零
- README 支持表 Niagara System 的 Audit 列标 ✓

### 2.7.2

- 新增 `MeshExport`，StaticMesh / SkeletalMesh 导出每级 LOD 的面数与 reduction 设定、Nanite 设置、碰撞、材质槽、socket，SkeletalMesh 另带骨骼数与 physics asset
- 新增 `AuditMesh`，规则 M1-M5 查缺 LOD、Nanite 与材质或面数不符、复杂碰撞当简单碰撞用、材质槽为空。建议值确定的规则进 `Spec` 块
- 新增 `EditMeshAsset`，StaticMesh 改 LOD 数与每级 reduction、Nanite 开关、碰撞、材质槽指派，SkeletalMesh 改 Nanite 与材质槽，收 `AuditMesh` 的 `Spec`。默认 dry run，`-apply` 落盘
- README 支持表补上 Mesh 一行，DataAsset 的 Edit 列标 ✓，对应 `DataAssetImport` 的部分写入

### 2.7.1

- `editor_session.py launch` 默认带 `-RenderOffScreen -unattended -nosplash`，编辑器不建窗口，不抢前台焦点。要窗口时加 `--visible`
- AI-Guide 的编辑器会话表同步上述行为

### 2.7.0

- 新增 `EditNiagaraSystem`，按 spec 改 Niagara system，分 `System` / `Emitters` / `Renderers` / `Modules` / `StaticSwitches` / `Inputs` 六段，固定按这个顺序执行。默认 dry run，`-apply` 编译后落盘，编译出错不保存
- `Modules` 增删、启停、在 stage 内移动模块。`Inputs` 设本地值、链接参数、dynamic input、data interface、对象引用，或清掉覆写回到默认值。`StaticSwitches` 按当前开关可见性逐个写，一个开关能藏起另一个
- `Emitters` 增删、改名、启停，改 emitter 属性。`Renderers` 增删、改属性。`System` 改 system 属性与 user parameter
- 已经是目标值的条目不算改动，日志记成 `entries already held the value`。整份 spec 都没改动的资产不编译不保存
- `NiagaraSystemExport` 格式重写：按 stage 列出模块的顺序、启停、static switch 与每个 input 的模式和值，dynamic input 嵌套导出，带 system stage、全部 data interface 与 renderer。属性只写和默认值不同的部分，struct 值不丢精度
- `NiagaraSystemExport` 在导出旁边写一份 `<导出>.spec.json`，能直接喂给 `EditNiagaraSystem`，原样喂回为零改动
- 队列 done 文件带 `EditorPid`，`run_commandlet.sh` 输出 `ran in` 一行，写明是哪台编辑器跑的任务
- 自动化测试进程不开任务队列，不写 `.alive`，测试结束后不会让 `run_commandlet.sh` 去等一个已经退出的编辑器

### 2.6.12

- 新增 `RenameDefaultSubobject`，C++ 改 `CreateDefaultSubobject` 的名字之前，把派生 Blueprint 与 level 里摆放的 actor 上存着的子对象先改名，改完 C++ 不留孤儿组件。默认 dry run，`-apply` 落盘
- `RenameDefaultSubobject` 的 level 按包加载，不初始化 world，sublevel 不丢 navmesh chunk。Blueprint 只改名不编译，编译会按 C++ 构造函数把名字改回去
- `RenameDefaultSubobject` 发现 C++ 已经不建旧名、或 level 实例的 Blueprint 不在扫描范围里、或是直接摆放的原生 actor 时报错，一个包都不存。存完重读每个包的名字表核对新名
- 删除 `scripts/rename_default_subobject.py`，由 `RenameDefaultSubobject` 取代
- 读包头的代码从 `ResaveRedirectedAsset` 抽成共用，`ReparentBlueprint` 的类名解析挪进 `UAssetWorkbenchUtil`
- `UASSET_WORKBENCH_VERSION_PATCH` 与版本字符串对齐，2.6.11 只改了字符串

### 2.6.11

- `EditBlueprint` 新增 `Timelines`，改既有 timeline 的长度，整条替换 float 轨道的关键帧。读外部曲线资产的轨道拒写，增删轨道与 vector / color / event 轨道不在范围内
- `BlueprintEdGraphExport` 的 float 轨道带出 `Keys` 与 `External`，导出可以原样喂回 `Timelines`
- `EditAnimAsset` 的 `Curves` 与 `Timelines` 共用同一个 `InterpMode` 解析

### 2.6.10

- 修复 `WidgetLayoutExport` 漏掉 `UWidget` 自身声明的属性，`RenderTransform` / `RenderTransformPivot` / `bIsVariable` / `Clipping` 等进 `Properties`，导出再导入不再把 shear 清零
- `WidgetLayoutExport` 的 `Visibility` / `RenderOpacity` / `IsEnabled` 从节点顶层挪进 `Properties`。`WidgetLayoutImport` 只读 `Properties`，顶层那份往返时被忽略，CanvasPanel 的 `Visibility` 会落成构造默认的 `SelfHitTestInvisible`
- `WidgetLayoutExport` 不再导出委托属性与 `TextExportTransient` 属性，前者只有占位值，后者是 `AddChild` 重建的 `Slot`
- 修复 `ReparentBlueprint` 改到新父类时编辑器崩溃在 `FBlueprintCompileReinstancer::PreCreateSubObjectsForReinstantiation`。改走编辑器 Reparent 的同一套流程，不再手动 `SetSuperStruct`，编译出错时不保存
- `ReparentBlueprint` 的 `-oldclass` / `-newclass` 收裸类名、带该类 C++ 前缀的类名与完整路径。修复 `U` 前缀报 `Blueprint parent mismatch`，`-newclass` 带前缀报 `New class not found`
- AI-Guide 补上拿 2.6.10 之前的导出做 round-trip 的陷阱

### 2.6.9

- 新增 `FixupRedirector`，重存 redirector 的每个引用方让它直接指向目标，再删掉没人引用的 redirector。收 redirector 包或目录，不弹引擎的报告窗，编辑器内队列里能跑
- 新增 `ResaveRedirectedAsset`，按项目 `DefaultEngine.ini` 的 `[CoreRedirects]` 读包头，找出还引用旧名的包并 resave，map 的 external actor / object 包一并扫，撤 redirect 之前用
- `RenameAsset` 接受目录，展开成目录下逐个资产的 pair，相对路径保留在目标目录下
- `RenameAsset` 失败时输出 `RenameAsset failed for <资产>. Reason: <原因>`，原因取自被自动取消的确认框文本，没有就取插件记下的第一条 error
- 修复 `RenameAsset` 在编辑器内队列里修 redirector 时报告窗断言崩溃，改用 `FixupRedirector` 同一份代码，不必再加 `-keepredirectors` 绕开
- `RenameAsset` 带 `-keepredirectors` 时，经 redirector 到达资产的引用方记成 `through the redirector`，以 Warning 收尾并退 0，不再报成引用丢失
- 修复 `RenameAsset` 把引用方里没有对象在用的残留 import 误报成引用丢失，同一批资产移动后的误报即此。这类 import 改名前单独记出，不进引用核对
- `RenameAsset` 独立进程路径改名前先同步扫一遍 asset registry
- `Edit*` 失败输出 `<RunName> failed for <资产>. Reason: <原因>`，替换原来的 `Target failed, nothing saved`，与 `RenameAsset` 同一格式。queue 路径的摘要、toast 与 done 文件的 `Error` 字段列出失败的资产与第一条 error
- 修复 `EditBlueprint` 的 `Widgets` `Add` 带 `AsRoot` 时已有根控件连同整棵子树被静默丢弃，改为报错，目标位置在建控件之前校验，被拒的 `Add` 不留任何东西
- `Widgets` 的 `Reparent` 带 `AsRoot` 只放行拆包，即旧根是只包着该控件的面板。旧根走编辑器的删除路径，修复旧根 GUID 残留触发 `was deleted but still has a GUID` ensure
- 修复 `Reparent` 带 `AsRoot` 拆包时骨架编译把挪上来的子树清掉，改为新根就位后再删旧根
- 修复 `WidgetLayoutExport` 到 `WidgetLayoutImport` 往返丢零值，导出改为写出 struct 的全部字段，CanvasPanel 的 `Offsets` 不再落成引擎默认的 (0,0,100,30)
- 编辑器内队列的 heartbeat 随编辑器启动上线，任务要等 asset registry 首轮扫描结束再取，消除 `UpdateValidators request made before RegisterBlueprintValidators` 校验错误，刚启动就跑 `RenameAsset` 也不再撞上 Unable To Rename While Discovering Assets
- 队列 run 期间保存触发的资产校验改在摘要之前对本 run 存过的包同步跑完，结果记成 `Save validation: <asset>: <message>`，并入该 run 的 Message Log 页与 warning / error 计数。插件新增依赖 DataValidation
- 修复 run 的 Message Log 页在编辑器里一直空着，日志行改为即时写入
- `run_commandlet.sh` 遇到 `editor_session.py` 登记的本项目编辑器还在启动时，等它的 heartbeat，最多 300 秒，不再退回独立 commandlet 与启动中的编辑器同时写资产
- `run_commandlet.sh` 走队列时 Export 也核对产物，打印 `output:` / `missing output:`，缺产物退 1，与独立进程路径一致
- 修复被杀编辑器留下的 heartbeat 在 pid 被 Windows 分给别的进程后仍被判成编辑器存活，wrapper 与 commandlet 的活编辑器检查都改为要求该 pid 是 `UnrealEditor.exe`
- `editor_session.py` 新增 `running`，本项目有登记的活编辑器时退 0，`close` / `release` 删掉被杀编辑器留下的 `.alive`
- 消除 `GetBindings` 的弃用编译警告
- AI-Guide 补上 Python 远程执行比任务队列 heartbeat 晚几秒上线，刚启动时 `exec_in_editor.py` 会 ping 不到

### 2.6.8

- `EditBlueprint` 的 `Graph` 新增 `AddDelegate` / `CreateDelegate`，对应编辑器的 Bind Event to 与 Create Event，绑定 `BlueprintAssignable` 委托

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
