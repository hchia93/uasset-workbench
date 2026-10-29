# Migrate

C++ 或资产改名之后，修复留在 uasset 里的旧引用。

## 为什么需要 Blueprint 侧的 redirect

CoreRedirects 只覆盖调用侧: call 节点、变量引用、类引用。Blueprint 图里的实现侧与消费侧不在它的射程内，会留下两类症状。

| 症状 | 发生条件 | 表现 |
| --- | --- | --- |
| override 静默失联 | 改名的 interface event，`BlueprintNativeEvent` 或 `BlueprintImplementableEvent` | BP 里的 override 退化成一个孤立的 custom event，编译只给 warning，事件不再触发 |
| 悬空 pin | 改名的 delegate 参数 | 绑定节点上留着过期的输出 pin 与连线，指向一个已经不存在的参数 |

两类都编译得过去，靠人眼很难在大项目里扫出来。

## Commandlet

| RunName | 做什么 | 默认行为 |
| --- | --- | --- |
| `RedirectBlueprintEvent` | 把退化成 custom event 的 BP override 重新接回新事件，连线一并搬过去。能识别 UE 加的 `_N` 去重后缀 | dry run |
| `RedirectBlueprintPin` | 把绑定节点的连线从旧输出 pin 移到新 pin，然后重建节点丢掉旧 pin。只处理同时带有新旧两个 pin 的节点 | dry run |
| `DeleteBlueprintNode` | 按 node id 删图节点，图逻辑搬进 C++ 之后的清理步骤。删除会切断该节点所有连线，不重新接线。schema 拒删的节点，function entry 与 result，报出后跳过 | dry run |
| `ReparentBlueprint` | 改 Blueprint 的父类 | 直接执行并保存 |
| `DuplicateAsset` | 把资产复制到新路径。副本独立，内部引用仍指向原来指的东西，不做重定向。目标已存在是错误，重跑不会覆盖掉已经改过的副本 | dry run |
| `RenameAsset` | 改资产名或搬路径，源可以是整个目录，硬引用与软引用一起重指。逐个资产报改名前后的 referencer 数，掉引用会报错而不是静默通过。留下的 redirector 默认 fixup 后删除 | dry run |
| `FixupRedirector` | 重存 redirector 的每个引用方，让它直接指向目标，再删掉已经没人引用的 redirector。收 redirector 包或目录 | dry run |
| `ResaveAsset` | 强制 load、compile、save，让 load 期的 fixup 落盘，例如已被 CoreRedirect 解析的引用。之后就能撤掉那条 redirect。支持 Blueprint 和 map | 直接保存 |
| `ResaveRedirectedAsset` | 按项目 `[CoreRedirects]` 的旧名扫包头，找出还引用旧名的包并 resave，不必事先知道是哪些资产 | dry run |
| `SanitizeLevelReference` | 把 level 里对旧资产的每一处引用换成新资产，然后 resave 这个 level | 直接保存，`-dryrun` 只统计 |
| `RenameDefaultSubobject` | 改 C++ `CreateDefaultSubobject` 的名字之前，把派生 Blueprint 与 level 里摆放的 actor 上存着的那个子对象先改名，改完 C++ 不留孤儿组件 | dry run |
| `RenameMaterialParameter` | 改基材质的参数名，跟进它下面每个 MI 的覆写（含 static switch），并改写 BP 里按字面名字设这个参数的调用 | dry run |
| `RenameNiagaraUserParameter` | 改 Niagara system 的 user 参数名，跟进 system 内的 link、binding、renderer，并改写 BP 里按字面名字设它的调用与 Niagara 组件模板上的覆写 | dry run |

执行后被改动的 uasset 会出现在版本控制的工作副本里。删掉的 redirector 文件只从磁盘删除，要在版本控制里补记删除。

## 写入类的 run 要在编辑器开着时跑

`run_commandlet.sh` 优先走 in-editor 队列，编辑器关着才 fallback 到独立进程直跑。**Migrate / Import / Edit 这几组必须走队列**，独立进程直跑写出的包缺东西。

症状：资产本身完好，打开、渲染、PIE 全正常，但 Reference Viewer 里它是个孤立节点，一条依赖边都没有。asset registry 的依赖图读的是包头 ImportMap，独立进程保存时 harvest 收集不全，那几条 import 就没落盘。同一个包在编辑器进程里再存一次，边全部回来，包也大几 KB。

已经踩了的补救: 编辑器开着，对受影响的资产跑一次 `ResaveAsset`（走队列），然后关掉 Reference Viewer 重开。

Export 与 Audit 是只读的，编辑器开不开都行。

## 调用

`RedirectBlueprintEvent`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    RedirectBlueprintEvent "/Game/Blueprints/BP_Foo" 10 600 \
    '-OwnerClass="/Script/MyModule.MyActor" -OldEvent="OnPickedUp" -NewEvent="HandlePickedUp"'
```

`RedirectBlueprintPin`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    RedirectBlueprintPin "/Game/Blueprints/BP_Foo" 10 600 \
    '-OldPin="Amount" -NewPin="DeltaAmount"'
```

`RenameAsset`。目标目录不必先存在。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh     "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject"     RenameAsset "" 10 600     '-pairs="/Game/UI/Debug/WBP_DebugMenu>/Game/UI/Cheat/WBP_CheatMenu" -apply'
```

源是目录（没有同名包）时展开成目录下逐个资产的 pair，相对路径原样接到目标目录下。目录里已有的 redirector 跳过并报 warning，先对这个目录跑 `FixupRedirector`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    RenameAsset "" 10 600 \
    '-pairs="/Game/Audio/Settings>/Game/Audio" -apply'
```

加 `-keepredirectors` 保留 redirector，跨仓库引用还没跟上时才需要。默认删掉，SVN 工作副本里留 redirector 就是垃圾。redirector 的 fixup 与 `FixupRedirector` 同一份代码，不弹报告窗，in-editor 队列里照常跑。

改名前已 load 的引用方若 import 了资产却没有对象在用（复制粘贴留下的残留 import 等），记一行 `imports X but no object in it uses it, saving drops that import`，不进改名后的引用核对。

带 `-keepredirectors` 时，改名没 load 到的引用方（没打开的 map 等）还写着旧路径，经 redirector 找到资产。这类记成 Display 行 `... still reaches ... through the redirector at ...`，计入 `through the redirector`，结尾给 Warning `run FixupRedirector before deleting it`，退 0。

逐资产摘要: `<dest>  R/N referencer(s) repointed, T through the redirector, S stale import(s) dropped`
失败行: `RenameAsset failed for <资产>. Reason: <原因>`，多个资产时 `<资产>` 写成 `<第一个> and N more`
原因: 被 unattended 自动取消的对话框文本，没有对话框时取插件记下的第一条 error
独立进程: 改名前先同步扫一遍 asset registry

C++ 类改名同时发生时，先在 `DefaultEngine.ini` 的 `[CoreRedirects]` 写好 class redirect 再跑，否则资产加载时找不到父类，rename 会在 load 阶段就失败。

ini 或 CDO 有软引用指向待改名资产时，`FAssetRenameManager` 会弹一个 OkCancel 确认框。commandlet 跑在 unattended 下，那个框必然答 Cancel，rename 直接返回失败。**先把 ini 指到目标路径再跑**，目标此刻还不存在没关系，软引用解析成 null 不报错。编辑器开着时改完 ini 要重启编辑器。撞上这条时失败行的原因就是那个框的文本 `Source code, config INI, and text files may need Find/Replace for: ... Continue with rename?`，后面跟一行 Display 提示改 ini、重启编辑器再跑。

`DeleteBlueprintNode`。node id 从 `BlueprintEdGraphExport` 加 `-graphs` 的导出产物里的 `NodeId` 原样抄，纯十六进制加逗号，不需要引号。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    DeleteBlueprintNode "/Game/Blueprints/BP_Foo" 10 600 \
    '-nodes=A1B2C3D4E5F64A7B8C9D0E1F2A3B4C5D,0F1E2D3C4B5A69788796A5B4C3D2E1F0'
```

没命中任何节点的 id 会在结尾统一报出来，打错字不会静默通过。因删除而失去引用的 Blueprint 变量不动，要另外处理。

`ReparentBlueprint`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    ReparentBlueprint "/Game/Blueprints/BP_Foo" 10 600 \
    '-blueprints="/Game/Blueprints/BP_Foo" -oldclass="AOldActor" -newclass="ANewActor"'
```

`-oldclass` / `-newclass` 收三种写法: 裸类名 `OldActor`、带该类自己 C++ 前缀的 `AOldActor` / `UOldWidget`、完整路径 `/Script/MyModule.OldActor`。前缀与类不符时按不匹配处理。

改父类走编辑器 Reparent 的同一套流程，编译出错时不保存，退 1。

`ResaveAsset`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    ResaveAsset "/Game/Blueprints/BP_Foo,/Game/Maps/MyLevel" 10 600 \
    '-assets="/Game/Blueprints/BP_Foo,/Game/Maps/MyLevel"'
```

`-nocompile` 可以跳过编译，只做 load 与 save。

`SanitizeLevelReference`。level 路径同时放 `AssetList` 与 `-levels`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    SanitizeLevelReference "/Game/Maps/L_Foo" 10 600 \
    '-levels="/Game/Maps/L_Foo" -replace="/Game/Old/SM_Arch=/Game/New/SM_Foundation" -dryrun'
```

## SanitizeLevelReference

用途: 重指向 level 包内部的资产引用，把每一处对旧资产的引用换成新资产，然后 resave 这个 level。场景是资产改了名但旧的那份还留着，别的 level 仍然 import 旧路径，先用这个把 import 改指到新资产，之后才能安全删掉旧的。

它是 `AuditLevelReference` 的配对操作，audit 找出破损，sanitize 修，见 [Audit.md](Audit.md)。

### 参数

| 参数 | 说明 |
| --- | --- |
| `-levels=` | 逗号分隔的 level 包路径 |
| `-replace=` | 逗号分隔的 `old=new` 资产路径对 |
| `-report=` | 报告 JSON 输出路径，默认在 `Intermediate/SanitizeLevelReference/` 下 |
| `-dryrun` | 统计会改动的引用数并写报告，但不修改也不保存 |

关键约束: 新旧资产都必须还在磁盘上，`FArchiveReplaceObjectRef` 要把两边都 load 起来才能换指针。必须在删除旧资产之前跑，删完再跑就来不及了。

每个 level 独立处理。某个 level load 或 save 失败只记录该 level 失败然后继续，内存里的改动随 GC 丢弃，磁盘上的 `.umap` 不受影响。

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 完成，或没有需要替换的 |
| 1 | 至少一个 level 失败，或参数错误 |
| 2 | 编辑器在运行 |

## FixupRedirector

用途: 修 redirector。重存每个引用方让它直接指向 redirector 的目标，再删掉已经没人引用的 redirector。

### 参数

| 参数 | 说明 |
| --- | --- |
| `-assets=` | 逗号分隔的 redirector 包或目录，目录递归查找。由 `AssetList` 生成 |
| `-apply` | 落盘，不给即 dry run |

### 调用

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    FixupRedirector "/Game/Folder,/Game/Old/WBP_Old" 10 600 \
    '-apply'
```

### 行为

| 情况 | 结果 |
| --- | --- |
| dry run | 逐个列出 `<redirector> -> <目标>  (N referencer package(s))`，不写盘 |
| `-apply` | 重存引用方，再删掉没人引用的 redirector，每删一个打一行 `deleted <file>` |
| 引用方 load 不到、存不了、磁盘上只读，或代码引用了 redirector | 该 redirector 留下，日志说明原因，退 1 |
| 路径下没有 redirector | Warning `<path> holds no redirector` |

不打开引擎 Fix Up Redirectors 的报告窗，in-editor 队列里能跑。

删掉的 redirector 文件只从磁盘删除，提交前要在版本控制里补记删除，清单就是 `deleted <file>` 行。

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 完成，或 dry run |
| 1 | 参数错误，或至少一个 redirector 留下 |
| 2 | 编辑器在运行 |

## ResaveRedirectedAsset

用途: 撤一条 CoreRedirect 之前，找出还引用旧名的包并 resave，让新名落盘。

### 参数

| 参数 | 说明 |
| --- | --- |
| `-scandir=` | 扫描根，默认 `/Game`。map 对应的 `__ExternalActors__` / `__ExternalObjects__` 目录一并扫 |
| `-oldnames=` | 逗号分隔，只看这几条 redirect。写完整旧名，或最后一个点之后的短名 |
| `-apply` | 落盘，不给即 dry run |

### 调用

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    ResaveRedirectedAsset "" 10 600 \
    '-scandir=/Game -oldnames="OldActor,/Game/Old/BP_A" -apply'
```

### 扫描

redirect 来源: 项目 `Config/DefaultEngine.ini` 的 `[CoreRedirects]`，引擎自带的 redirect 不算
扫描方式: 直接读磁盘上 `.uasset` / `.umap` 的包头，不加载，整个 `/Game` 扫一遍是秒级

| redirect | 怎样算引用 |
| --- | --- |
| `ClassRedirects` / `PackageRedirects` | 包 import 了旧类或旧包。名字表里只是同名的普通名字（如 anim notify 的显示名）不算 |
| `StructRedirects` / `EnumRedirects` | 名字表里有旧名，它们只出现在 property tag 里 |
| `PropertyRedirects` / `FunctionRedirects` | 不扫。Blueprint 节点把它们存成普通名字，resave 不改写 |

### 输出

| 行 | 含义 |
| --- | --- |
| `<包>  (<旧名>...)` | 每个包一行，列出它还引用的旧名 |
| `<key> <旧名>: no package under <dir> names it` | 扫描范围内没有包引用，可以候选撤掉。撤之前先查代码、ini 与存档 |
| `<包> still names <旧名> after the resave` | `-apply` 存完重读包头仍有旧名，Warning。struct / enum 旧名可能以普通名字留下（如 Blueprint pin 类型），这条 redirect 还撤不掉 |

### apply

`-apply` 逐包 load、编译 Blueprint，走编辑器的保存路径落盘，map 与 external actor 包都在内，存完重读每个包核对。

编辑器里有未保存改动的包拒绝处理，报 error，先保存或丢弃那些改动。

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 完成，dry run，或没有包要存 |
| 1 | `[CoreRedirects]` 里没有匹配的 Class / Struct / Enum / Package redirect，或至少一个包没存成 |
| 2 | 编辑器在运行 |

## RenameDefaultSubobject

用途: C++ 改 `CreateDefaultSubobject` 的名字。先改资产再改代码，顺序反过来，引擎会按新名建一个组件，旧名那份留成孤儿，两个同时挂在 actor 上。

### 流程

| 步 | 动作 |
| --- | --- |
| 1 | 跑本 commandlet，`-apply` |
| 2 | C++ 改子对象名，成员名一起改时加 `+PropertyRedirects`，重编 |
| 3 | 对结尾列出的包跑 `ResaveAsset`。它会编译 Blueprint，名字表里的旧名靠这一步才清掉，引擎的 `ResavePackages` 清不干净 |
| 4 | 撤掉第 2 步的 redirect |

### 参数

| 参数 | 说明 |
| --- | --- |
| `-classes=` | 逗号分隔，持有这个子对象的 C++ 类。收裸类名、带 C++ 前缀的类名与完整路径 |
| `-oldname=` | 子对象现在的名字，C++ 还在用的那个 |
| `-newname=` | 改成的名字 |
| `-scandir=` | 扫描根，默认 `/Game`，Blueprint 与 level 都在这个范围里找 |
| `-apply` | 落盘，不给即 dry run |

### 调用

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    RenameDefaultSubobject "" 10 600 \
    '-classes="AMyActor,AMyOtherActor" -oldname=InteractComponent -newname=ResponseRunner -apply'
```

### 规则

| 情况 | 处理 |
| --- | --- |
| C++ 已经不建 `-oldname` 这个子对象 | 报错退出。说明 C++ 先改了，孤儿已经产生 |
| Blueprint | 从 asset registry 的 native parent 找派生 Blueprint，在 CDO 上改名，不编译。编译会按 C++ 构造函数重建子对象，名字又变回旧的 |
| level | 先读包头，名字表里有旧名的 `.umap` 与 external actor 包才加载。按包加载，不初始化 world，sublevel 不会丢 navmesh chunk |
| level 里的实例 | 存盘时跟随 actor 类 CDO 上那份子对象的名字。actor 的 Blueprint 不在这次范围里，或者是直接摆放的原生 actor，报错 |
| 任何一处报错 | 一个包都不存，全部成功或全部不写 |
| 存完 | 重读每个包的名字表，新名不在就报错 |

编辑器里有未保存改动的包拒绝处理，先保存或丢弃那些改动。

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 完成，或 dry run |
| 1 | 参数错，类解析不了，C++ 已不建旧名，任一实例的 Blueprint 不在范围里，或任一包没存成 |
| 2 | 编辑器在运行 |

## RenameMaterialParameter / RenameNiagaraUserParameter

用途: 资产侧的参数改名。BP 按字面名字设参数，名字一改调用就静默失效，编译不报错。这两个 run 把资产内部与 BP 调用一次改齐。

### 参数

| 参数 | 说明 |
| --- | --- |
| `-material=` / `-system=` | 基材质或 Niagara system 的路径。给 MI 退 1，参数声明在基材质上 |
| `-from=` / `-to=` | 旧名与新名。Niagara 带不带 `User.` 前缀都收 |
| `-blueprints=` | 逗号分隔，指定要改写的 BP，给了就不自动收集 |
| `-apply` | 落盘，不给只列清单 |

### 调用

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    RenameMaterialParameter "" 10 900 \
    '-material="/Game/Materials/M_Base" -from="Fill Amount" -to="Fill Ratio"'
```

确认清单后补 `-apply` 再跑一次。

### 改什么

| 对象 | `RenameMaterialParameter` | `RenameNiagaraUserParameter` |
| --- | --- | --- |
| 资产本身 | 基材质里同名的每个参数表达式 | user 参数存储与它的 script variable |
| 下游资产内 | 父链通到这个基材质的每个 MI 的覆写，含 static switch，覆写值保留 | 链接到它的 module input、user 参数 binding、renderer 的属性绑定 |
| BP 调用 | owner 是材质或 primitive component、带 `ParameterName` pin 的调用，如 `SetScalarParameterValue` / `SetScalarParameterValueOnMaterials` | owner 是 `NiagaraComponent` / `NiagaraFunctionLibrary` / `NiagaraDataInterfaceArrayFunctionLibrary`、带 `InVariableName` / `OverrideName` 一类 pin 的调用。写 `User.X` 的改成 `User.Y`，写 `X` 的改成 `Y` |
| BP 组件 | 无 | 指向这个 system 的 Niagara 组件模板上的覆写，值保留 |

### BP 作用域

不给 `-blueprints=` 时，从资产往引用方走，收 Material 3 层、Niagara 2 层以内的 BP。中间经过 mesh、MI、别的 BP 都算。

BP 调用不知道运行时打到的是哪个材质或 system。作用域内另一个材质也有同名参数时，那处调用同样会被改写。dry run 的清单逐条列出每处调用，确认后再 `-apply`。

名字来自连线的调用只报 warning，不改，要人工核对。

`UKismetMaterialLibrary` 设的是 material parameter collection 的参数，不在范围里。

### 不改的东西

| 对象 | 原因 |
| --- | --- |
| 关卡里摆放的 Niagara 组件的 per-instance 覆写 | 不扫关卡 |
| 材质函数里的参数 | 参数属于函数，不属于这个材质 |

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 成功 |
| 1 | 执行失败或参数错误 |
| 2 | 编辑器在运行 |

Migrate 组不会返回 3，3 是 Audit 组专用。退 1 的具体口径逐 commandlet 不同。

| RunName | 什么情况退 1 |
| --- | --- |
| `DeleteBlueprintNode` | 参数错、node id 解析不了、没有资产、编译或保存失败 |
| `DuplicateAsset` | 参数错、源资产缺失、目标已存在 |
| `RenameAsset` | 参数错、源 load 不到、目标已存在、改名被拒、保存失败、有引用方掉了引用、redirector 修不掉。只剩经 redirector 的引用方不算失败，退 0 |
| `FixupRedirector` | 参数错，或至少一个 redirector 留下 |
| `ResaveRedirectedAsset` | 没有匹配的 redirect，或至少一个包没存成 |
| `SanitizeLevelReference` | 参数错，或至少一个 level 失败 |
| `RenameDefaultSubobject` | 参数错、类解析不了、C++ 已不建旧名、实例的 Blueprint 不在范围里、任一包没存成 |
| `RenameMaterialParameter` | 参数错、不是基材质、旧名不存在、新名已被占、任一包没存成 |
| `RenameNiagaraUserParameter` | 参数错、system load 不到、旧名不存在、新名已被占、改完编译报错、任一包没存成 |

没命中任何节点的 `DeleteBlueprintNode` 不算失败，退 0 并在结尾把没命中的 id 报出来。

## dry run 与 apply

默认行为按 commandlet 分两种。

| RunName | 默认 | 落盘方式 |
| --- | --- | --- |
| `RedirectBlueprintEvent` | 只扫描 | 补 `-apply` |
| `RedirectBlueprintPin` | 只扫描 | 补 `-apply` |
| `DeleteBlueprintNode` | 只扫描 | 补 `-apply` |
| `DuplicateAsset` | 只扫描 | 补 `-apply` |
| `FixupRedirector` | 只扫描 | 补 `-apply` |
| `ResaveRedirectedAsset` | 只扫描 | 补 `-apply` |
| `RenameDefaultSubobject` | 只扫描 | 补 `-apply` |
| `RenameMaterialParameter` | 只扫描 | 补 `-apply` |
| `RenameNiagaraUserParameter` | 只扫描 | 补 `-apply` |
| `SanitizeLevelReference` | 直接落盘 | 想先看结果就加 `-dryrun` |

两个 redirect 的扫描输出里逐条列出命中的 Blueprint、事件或节点、以及会搬多少组连线，确认无误再补 `-apply` 编译并保存。

扫描阶段一条都没命中时会给 warning，先核对 `-OwnerClass` 与旧名拼写，再考虑扩大 `-assets` 范围。
