# Audit

只读检查资产，产出报告 JSON，不改任何包。本文覆盖四个 commandlet 与三个配套脚本。

## Commandlet

| RunName | 检查什么 |
| --- | --- |
| `AuditLevelReference` | level 包里指向已不存在资产的引用 |
| `AuditLevelTopology` | level 之间的 streaming 关系，谁是 persistent，谁是 sublevel |
| `AuditTexture` | 贴图设置与用途是否对得上，规则 T1-T15 |
| `AuditMaterial` | 材质的 Nanite 兼容与 usage flag 是否对得上实际挂载，规则 N1-N9 与 U1-U4 |
| `AuditPCG` | PCG graph 的静态规则 P1-P7，加关卡上 PCGComponent 的 P8 |

## AuditLevelReference

用途: 审计 level（`.umap`）包里的破损资产引用，也就是依赖的包在磁盘上已经不存在。典型场景是资产被改名或删除（例如分支部分同步拉进来的状态），而别的 level 还在 import 旧路径。

机制: 只读。不 load world，不保存任何包。走 Asset Registry 的依赖图，逐个依赖用 `FPackageName::DoesPackageExist` 判断。

### 参数

| 参数 | 说明 |
| --- | --- |
| `-levels=` | 逗号分隔的 level 包路径，给了它就不看 `-scandir` |
| `-scandir=` | 要枚举 level 的内容根目录，两个都不给时默认 `/Game` |
| `-report=` | 报告 JSON 输出路径，默认在 `Intermediate/AuditLevelReference/` 下 |

### 调用

扫整个 `/Game`。Audit 组不吃 `AssetList`，位置参数留空串。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    AuditLevelReference "" 10 600 \
    '-scandir="/Game"'
```

只查指定的几张图，报告写到指定路径。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    AuditLevelReference "" 10 600 \
    '-levels="/Game/Maps/L_Foo,/Game/Maps/L_Bar" -report="C:/temp/level_ref.json"'
```

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 没有破损引用 |
| 3 | 有破损引用，明细在报告里 |
| 1 | 参数错误，或报告写失败 |
| 2 | 编辑器在运行 |

3 不是失败。运行本身是成功的，只是查出了要处理的东西，CI 靠它 gate 提交。

### 报告 JSON

顶层字段。

| 字段 | 含义 |
| --- | --- |
| `phase` | 固定 `audit-level-reference`，标识产出这份报告的运行 |
| `timestamp_utc` | 运行时刻，ISO 8601 UTC |
| `levels_scanned` | 本次扫过的 level 数 |
| `levels_with_broken_refs` | 其中含破损引用的 level 数 |
| `total_broken_refs` | 破损引用总条数 |
| `results` | 逐 level 明细，只列有破损的那些 |

`results` 每项。

| 字段 | 含义 |
| --- | --- |
| `level` | level 包路径 |
| `broken_count` | 该 level 的破损引用条数 |
| `broken_refs` | 破损依赖的包路径数组 |

## 挂载 root 约束

包是否存在按已挂载的 content root 解析。位于未挂载 root 下的依赖，例如被禁用的插件里的资产，会被判成 missing。

跑之前必须让项目的插件全部启用，否则报出来的是假破损。

## 配对操作

审计只找问题，不修。修在 Migrate 组的 `SanitizeLevelReference`，它把 level 里对旧资产的引用重指到新资产，见 [Migrate.md](Migrate.md)。

`AuditTexture` 的配对操作是 Edit 组的 `EditTextureAsset`，`AuditMaterial` 的是 `EditMaterialAsset`。

## AuditLevelTopology

用途: 按 streaming 关系给每个 level 分角色。需要驱动 streaming 的工具，性能测量与预算审计都算，必须知道该把哪个 level 当 persistent 打开。把那个名字硬编码进脚本，正是这类工具变成项目专用的原因，这个 commandlet 就是为了消掉那个硬编码。

机制: 只读。逐个 load level 包读它的 streaming levels，不保存任何东西。不需要更新组件，streaming 条目是 authored data。

### 角色

| 角色 | 判定 |
| --- | --- |
| `Standalone` | 没有 streaming levels，也不被别的 level 引用 |
| `PersistentHost` | 有 streaming levels |
| `Sublevel` | 没有 streaming levels，但被别的 level 引用 |

hosting 优先于被 hosted。一个 level 只要自己挂着 sublevel 就是 `PersistentHost`，哪怕它同时被别的 level 引用。

判据是这个 level 能不能被独立打开来驱动它的 sublevel。一个 level 被复制一份塞进别的 level 里，例如做概念验证或代理关卡，不影响它自己仍然可以独立打开测量。反过来让被引用优先，真正有内容的关卡会被判成 `Sublevel`，只有一层壳的代理关卡反而被判成 `PersistentHost`，正好判反。

嵌套关系不丢，`referenced_by` 照常记录谁把它 stream 了进去。

### 参数

| 参数 | 说明 |
| --- | --- |
| `-levels=` | 逗号分隔的 level 包路径，给了它就不看 `-scandir` |
| `-scandir=` | 要枚举 level 的内容根目录，两个都不给时默认 `/Game` |
| `-report=` | 报告 JSON 输出路径，默认在 `Intermediate/AuditLevelTopology/` 下 |

### 调用

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    AuditLevelTopology "" 10 600 \
    '-scandir="/Game"'
```

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    AuditLevelTopology "" 10 600 \
    '-levels="/Game/Maps/L_Foo,/Game/Maps/L_Bar" -report="C:/temp/level_topology.json"'
```

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 报告已写出 |
| 1 | 参数错误，或报告写失败 |
| 2 | 编辑器在运行 |

本 commandlet 不返回 3，拓扑没有"发现问题"的概念。

### 报告 JSON

顶层字段。

| 字段 | 含义 |
| --- | --- |
| `phase` | 固定 `audit-level-topology` |
| `timestamp_utc` | 运行时刻，ISO 8601 UTC |
| `levels_scanned` | 本次扫过的 level 数 |
| `standalone_count` | `Standalone` 数 |
| `persistent_host_count` | `PersistentHost` 数 |
| `sublevel_count` | `Sublevel` 数 |
| `results` | 逐 level 明细 |

`results` 每项。

| 字段 | 含义 |
| --- | --- |
| `level` | level 包路径 |
| `role` | `Standalone` / `PersistentHost` / `Sublevel` |
| `streaming_levels` | 该 level 挂的 sublevel 包路径数组 |
| `referenced_by` | 把它 stream 进去的 level 数组 |

扫描范围之外的 streamed level 没有对应节点可以标注 `referenced_by`，host 侧仍然会把它记进 `streaming_levels`。

## AuditTexture

用途: 审计 Texture2D 的构建设置与实际用途是否对得上。压缩格式、sRGB、LOD group、mip、2 的幂、尺寸预算、streaming、VT。

机制: 只读，不保存任何包。两趟。第一趟只读 Asset Registry 的 tag（LODGroup / CompressionSettings / SRGB / VirtualTextureStreaming / NeverStream / Dimensions / MipGenSettings / MaxTextureSize / PowerOfTwoMode），够判的规则当场判完；只有需要 load 才能判的（built size 超预算、非 2 的幂、normal map 的 RDO）才把那张贴图 load 进来。用途相关的规则（T1 T2 T3 T12 T15）另外 load 引用这些贴图的材质，走引擎自己的 sampler 规则 `UMaterialExpressionTextureBase::VerifySamplerType`。

用途分类走 Asset Registry 反向图：贴图 → 材质 → 材质实例 → mesh / widget / Niagara，最多爬 4 层，遇到能分类的那层就停。分五类 UI / Character / Prop / World / VFX，材质与 Blueprint 是透明层，继续往上爬。

### 参数

| 参数 | 说明 |
| --- | --- |
| `-assets=` | 逗号分隔的入口资产，由 wrapper 的位置参数 `AssetList` 生成，不要写进 `EXTRA_ARGS`。可以直接给贴图，也可以给 level / BP / 材质，走依赖链把 `Texture2D` 收出来。给了它就不看 `-scandir` |
| `-scandir=` | 要枚举贴图的内容根目录，两个都不给时默认 `/Game` |
| `-report=` | 报告 JSON 输出路径，默认 `Intermediate/AuditTexture/report.json` |
| `-rules=` | 只跑列出的规则，例如 `-rules=T1,T3`。不给就全跑。写了表里没有的规则名退 1 |

### 调用

扫一个目录下的贴图。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    AuditTexture "" 10 600 \
    '-scandir="/Game/Characters"'
```

从一个 BP 走依赖链收贴图，只跑点名的规则。入口资产写位置参数，`-rules` 走 `EXTRA_ARGS`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    AuditTexture "/Game/Characters/BP_Hero" 10 600 \
    '-rules=T2,T3,T4'
```

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 没有 Error 或 Warning |
| 3 | 有 Error 或 Warning，明细在报告里 |
| 1 | 参数错误、`-assets` 路径不存在、`-rules` 规则名不认识，或报告写失败 |
| 2 | 编辑器在运行 |

3 不是失败。Info 不影响退出码。

### 规则

| 规则 | 级别 | 判定 | 建议值 |
| --- | --- | --- | --- |
| T1 | Error | 材质里的 SamplerType 与贴图实际格式不符，引擎自己的 `VerifySamplerType` 说不行 | 无，改材质不改贴图 |
| T2 | Error | 被当 Normal 采样，但 `CompressionSettings` 不是 `TC_Normalmap` | `CompressionSettings=TC_Normalmap` |
| T3 | Error | 被当 Normal / Masks 采样，但 `SRGB` 开着 | `SRGB=false` |
| T4 | Error | `CompressionSettings` 是只吃线性的那批（`TC_Masks` / `TC_Alpha` / `TC_Normalmap` / `TC_HDR*` / `TC_SingleFloat` / `TC_HalfFloat`），但 `SRGB` 开着 | `SRGB=false` |
| T5 | Warning | group 与用途对不上：只被 UI 引用却不在 `TEXTUREGROUP_UI`；normal map 不在 `*NormalMap` group | 对应 group |
| T6 | Warning | `TEXTUREGROUP_UI` 但生效的 MipGen 不是 `TMGS_NoMipmaps` | `MipGenSettings=TMGS_NoMipmaps` |
| T7 | Warning | `GetBuiltTextureSize` 超用途预算 | 预算值写进 `MaxTextureSize` |
| T8 | Error | 非 2 的幂，`PowerOfTwoMode` 是 `None`，且还在出 mip | `PowerOfTwoMode=PadToPowerOfTwo` |
| T9 | Error | `TC_EditorIcon` / `TC_VectorDisplacementmap` 且源尺寸 >= 512 | `CompressionSettings=TC_Default` |
| T10 | Warning | `NeverStream` 开着，不是 UI / LUT group，且源尺寸 >= 1024 | `NeverStream=false` |
| T11 | Warning | 开了 VT，但 group 是 `UI` / `ColorLookupTable` / `Pixels2D` | `VirtualTextureStreaming=false` |
| T12 | Warning | 被当 Grayscale / LinearGrayscale / Alpha 采样，但 `CompressionSettings` 还是 `TC_Default` | `TC_Grayscale` 或 `TC_Alpha` |
| T13 | Info | normal map 上开了 Oodle RDO | 无 |
| T14 | Info | 没有任何引用者 | 无 |
| T15 | Info | 同一个材质下的多张 normal map，`bFlipGreenChannel` 不一致 | 无 |

尺寸预算: Character 2048、Prop 2048、World 2048、VFX 1024。UI 与未分类不判 T7。

规则表与预算硬编码在 C++ 里，不出 UPROPERTY。

### 报告 JSON

顶层字段。

| 字段 | 含义 |
| --- | --- |
| `RunName` | 固定 `AuditTexture` |
| `Scanned` | 本次扫过的贴图数 |
| `Findings` | 逐条明细 |
| `Summary` | 按级别与规则的计数 |
| `Spec` | 可直接喂回 `EditTextureAsset` 的 spec 块 |

`Findings` 每项。

| 字段 | 含义 |
| --- | --- |
| `Asset` | 贴图包路径 |
| `Rule` | 规则名，`T1` 到 `T15` |
| `Severity` | `Error` / `Warning` / `Info` |
| `Property` | 要改的属性名 |
| `Current` | 当前值 |
| `Expected` | 建议值 |
| `Context` | 触发这条的上下文，用途分类与点名的材质 |

`Property` 与 `Expected` 为空的条目不进 `Spec`（T1 与 Info 级）。

### 喂回 EditTextureAsset

报告里的 `Spec` 块形状就是 `EditTextureAsset` 的 spec。只有 Error 与 Warning 且建议值确定的规则会进去。

```json
{"Targets":[{"AssetPath":"/Game/Path/T_Foo.T_Foo","Properties":{"SRGB":"false","CompressionSettings":"TC_Normalmap"}}]}
```

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    EditTextureAsset "" 10 600 \
    '-spec="C:/temp/texture_spec.json" -apply'
```

见 [Edit.md](Edit.md)。

## AuditMaterial

用途: 审计 Material 与 MaterialInstance。两组规则：Nanite 兼容 N1-N9，usage flag 与实际挂载对照 U1-U4。每个基材质另外产出 `Cost` 与 `Permutation` 两块数字，`-stats` 再补一层编译出来的 shader 数字，`PIEWarmup` 把这些数字排成「谁会让 PIE 等」的名单。

机制: 只读，不保存任何包。审计对象永远是基 `UMaterial`，usage flag 只存在于基材质上，扫到的 MI 折算回它的 parent。用途判定走 Asset Registry 反向图：基材质 → 子 MI 树 → 引用者。SkeletalMesh 与 StaticMesh 加载后读 slot，Niagara 读每个 renderer 的 `GetUsedMaterials`，ISM 与 SplineMesh 只在入口资产是 level 时扫组件。MorphTargets 与 Clothing 由 skeletal mesh 自身有没有 morph target / clothing asset 推出。

### 参数

| 参数 | 说明 |
| --- | --- |
| `-assets=` | 逗号分隔的入口资产，由 wrapper 的位置参数 `AssetList` 生成，不要写进 `EXTRA_ARGS`。可以直接给材质，也可以给 level / BP / mesh，走依赖链把材质收出来。给了它就不看 `-scandir` |
| `-scandir=` | 要枚举材质的内容根目录，两个都不给时默认 `/Game` |
| `-report=` | 报告 JSON 输出路径，默认 `Intermediate/AuditMaterial/report.json` |
| `-stats` | 开二档：逐材质编译代表性 shader，读采样器数、纹理采样数、指令数。很慢 |
| `-rules=` | 只跑列出的规则，例如 `-rules=N3,U1`。不给就全跑。写了表里没有的规则名退 1 |

### 调用

扫一个目录下的材质。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    AuditMaterial "" 10 600 \
    '-scandir="/Game/VFX"'
```

从一个 BP 走依赖链收材质，并开二档。入口资产写位置参数，`-stats` 走 `EXTRA_ARGS`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    AuditMaterial "/Game/Characters/BP_Hero" 10 3600 \
    '-stats'
```

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 没有 Error 或 Warning |
| 3 | 有 Error 或 Warning，明细在报告里 |
| 1 | 参数错误、`-assets` 路径不存在、`-rules` 规则名不认识，或报告写失败 |
| 2 | 编辑器在运行 |

3 不是失败。Info 不影响退出码。

### Nanite 规则

只对挂在 Nanite mesh 上的材质判定。`Asset` 是材质，N9 例外，它的 `Asset` 是那个 mesh。

| 规则 | 级别 | 判定 | 建议值 |
| --- | --- | --- | --- |
| N1 | Error | BlendMode 不是 Opaque 或 Masked | 无，改 BlendMode 是美术决定 |
| N2 | Error | ShadingModel 含 `MSM_SingleLayerWater` | 无 |
| N3 | Error | 缺 `bUsedWithNanite`，domain 在 Surface / DeferredDecal / Volume 且不是 special engine material | `bUsedWithNanite=true` |
| N4 | Error | 挂在 Nanite skeletal mesh 上却缺 `bUsedWithSkeletalMesh` | `bUsedWithSkeletalMesh=true` |
| N5 | Warning | `bIsSky` 开着，Nanite 会整个跳过这种材质 | 无 |
| N6 | Warning | Masked，但 `r.Nanite.AllowMaskedMaterials` 是 0 | 无 |
| N7 | Info | WorldPositionOffset 接了线 | 无 |
| N8 | Info | PixelDepthOffset 接了线 | 无 |
| N9 | Warning | Nanite mesh 上有空的材质槽 | 无，改的是 mesh 不是材质 |

N1 / N2 / N5 / N6 只报不进 `Spec`：BlendMode 与 ShadingModel 是美术决定，工具不替人改。

### Usage flag 规则

| 规则 | 级别 | 判定 | 建议值 |
| --- | --- | --- | --- |
| U1 | Error | 实际挂在 X 上却没开对应的 `bUsedWithX` | `bUsedWithX=true` |
| U2 | Error | U1 成立且 `bAutomaticallySetUsageInEditor` 开着，每次启动编辑器同步重编一次直到资产被重存 | 无 |
| U3 | Warning | flag 开着但扫描范围内找不到任何消费者 | 无，范围外的引用工具看不见，手工确认 |
| U4 | Error | U1 成立且 `bAutomaticallySetUsageInEditor` 关着，编辑器不会自愈，游戏里直接吃 default material | 无 |

U1 是事实，U2 与 U4 是它的后果，同一个缺失 flag 会同时报出 U1 加其中一条。只有 U1 进 `Spec`。

规则表与阈值硬编码在 C++ 里，不出 UPROPERTY。

### Cost

每个基材质一块，一档字段永远有，二档字段只有 `-stats` 跑过才有。

| 字段 | 档 | 含义 |
| --- | --- | --- |
| `MaterialDomain` / `BlendMode` / `ShadingModel` / `TwoSided` / `OpacityMaskClipValue` / `NumCustomizedUVs` / `bIsSky` / `bEnableTessellation` | 1 | 基本设定 |
| `bAutomaticallySetUsageInEditor` | 1 | 决定缺 flag 时走 U2 还是 U4 |
| `WorldPositionOffsetConnected` / `PixelDepthOffsetConnected` / `DisplacementConnected` | 1 | 接线状态，序列化过的 `PropertyConnectedMask` |
| `bHasVertexInterpolator` / `bHasPerInstanceRandom` / `bHasPerInstanceCustomData` / `bHasCustomizedUVs` / `bHasSceneColor` / `bHasWorldPosition` / `bHasMaterialLayers` / `bHasRuntimeVirtualTextureOutput` | 1 | cached expression data 的布尔位 |
| `ReferencedTextureCount` / `ExpressionCount` / `FunctionCount` | 1 | 规模 |
| `SamplerUsage` / `EstimatedTextureSamplesVS` / `EstimatedTextureSamplesPS` | 2 | 采样器与纹理采样 |
| `EstimatedVirtualTextureLookups` / `VirtualTextureStacks` | 2 | VT |
| `UsedUVScalars` / `UsedCustomInterpolatorScalars` | 2 | 插值器占用 |
| `LWCFuncUsagesVS` / `LWCFuncUsagesPS` / `LWCFuncUsagesCS` | 2 | LWC 函数调用总数 |
| `ShaderCount` / `MaxInstructionCount` / `Instructions[]` | 2 | 编译出来的 shader 数与逐代表性 shader 的指令数 |

编辑器内跑（queue 通道）时多一块 `EditorStats`，那是 `UMaterialEditingLibrary::GetStatistics` 的原样输出。

### Permutation

| 字段 | 含义 |
| --- | --- |
| `UsageFlagCount` | 开着的 `bUsedWith*` 个数 |
| `QualityLevels` | cached expression data 里实际用到的 quality level 数 |
| `StaticSwitchCount` | 基材质上的 static switch 参数个数 |
| `StaticSwitchCombosUsed` | 子 MI 实际覆写出的不同 switch 组合数，基材质自己算一种 |
| `ChildInstanceCount` | 反向图里的子 MI 总数 |
| `Size` | 前三项的乘积，`PIEWarmup` 的排序依据 |

### PIEWarmup

回答「PIE 里哪个材质会让人等」。只用本次跑出来的数字，不推测。

| 字段 | 含义 |
| --- | --- |
| `Suspects[].Material` | 材质路径 |
| `Suspects[].Reason` | `SynchronousUsageRecompile` / `PermutationScale` / `ShaderVolume`，优先级从高到低 |
| `Suspects[].Evidence` | 支撑这条的具体数字 |
| `Notes` | 排了几条，以及本次有没有 `-stats` |

`SynchronousUsageRecompile` 就是 U2：缺 flag 加自动补 flag，等于每次启动编辑器同步重编一次，是唯一能靠改资产根治的那类。`PermutationScale` 是 `Permutation.Size` 到了 8。`ShaderVolume` 要 `-stats`，门槛是 `ShaderCount` 200 或 `MaxInstructionCount` 400。一条都没有时 `Notes` 会明说没有。

### -stats 的代价

二档逐材质编译代表性 shader，一个材质几十秒到几分钟。扫描模式下只对前 20 个材质开，超出的会打一条 Warning 说跳了几个。要强制某个材质出二档数字，把它写进 `-assets=`，点名的材质不受这个上限管。

### 报告 JSON

顶层字段。

| 字段 | 含义 |
| --- | --- |
| `RunName` | 固定 `AuditMaterial` |
| `Scanned` | 本次审计的基材质数 |
| `MaterialInterfacesInScope` | 折算之前收到的材质与材质实例总数 |
| `Stats` | 本次有没有开 `-stats` |
| `Findings` | 逐条明细 |
| `Summary` | 按级别的计数，加 `Rules` 逐规则计数 |
| `Materials` | 逐材质的 `UsageFlags` / `AppliedUsages` / `NaniteMeshes` / `Cost` / `Permutation` |
| `PIEWarmup` | 上面那块 |
| `Spec` | 可直接喂回 `EditMaterialAsset` 的 spec 块 |

`Findings` 每项的字段与 `AuditTexture` 一致：`Asset` / `Rule` / `Severity` / `Property` / `Current` / `Expected` / `Context`。

### 喂回 EditMaterialAsset

只有 `Expected` 确定的规则会进 `Spec`，也就是 U1 / N3 / N4 三条 usage flag 规则。

```json
{"Targets":[{"AssetPath":"/Game/Path/M_Foo.M_Foo","Properties":{"bUsedWithNanite":"true"}}]}
```

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    EditMaterialAsset "" 10 600 \
    '-spec="C:/temp/material_spec.json" -apply'
```

见 [Edit.md](Edit.md)。

## AuditPCG

RunName `AuditPCG`。只做静态检查，不触发生成，节点在生成期报的 error / warning 拿不到。

### 参数

| 参数 | 含义 |
| --- | --- |
| `AssetList` | 点名的 graph，给了就不扫目录 |
| `-scandir=` | 扫 `PCGGraph` 资产的目录，默认 `/Game` |
| `-levels=` | 点名的关卡，扫其中的 PCGComponent |
| `-withlevels` | 扫 `-scandir` 下全部关卡。关卡要整个加载，所以默认不扫 |
| `-report=` | 报告路径，默认 `Intermediate/AuditPCG/<stamp>.json` |

### 调用

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    AuditPCG \
    "" \
    10 600 \
    '-scandir=/Game/PCG -withlevels -report="C:/temp/pcg.json"'
```

### 规则

| 规则 | 级别 | 判定 |
| --- | --- | --- |
| P1 | Error | `Required` 输入 pin 没接线 |
| P2 | Warning | 孤立节点，任何 pin 都没连线，输入输出节点除外 |
| P3 | Warning | 节点被 bypass（`Enabled` 为假） |
| P4 | Error | subgraph 节点指向空，或调用自己的 graph |
| P5 | Warning | graph parameter 没有任何 Get Graph Parameter 节点读它 |
| P6 | Error | Static Mesh Spawner 的 weighted selector 没有 mesh entry，或 Spawn Actor 没有模板类且 `TemplateActorClass` pin 没接线 |
| P7 | Warning / Info | 图没开 HiGen 却有 Grid Size 节点（Warning），或开了 HiGen 但一个 Grid Size 节点都没有（Info） |
| P8 | Error / Warning / Info | 关卡上的 PCGComponent：graph 为空（Error），runtime 生成没有 scheduling policy（Warning），`bActivated` 为假（Info） |

没有 debug 残留规则：`bDebug` 是 Transient，不入库。

### 退出码

同其他 Audit：0 干净，3 有 Warning 或 Error，1 跑不起来，2 编辑器在跑。

### 报告 JSON

| 字段 | 含义 |
| --- | --- |
| `RunName` / `ExporterVersion` | 身份 |
| `GraphsScanned` / `LevelsScanned` / `ComponentsScanned` | 规模 |
| `Summary` | `Error` / `Warning` / `Info` 计数 |
| `Findings[]` | 每项 `Asset` / `Rule` / `Severity` / `Node` / `Current` / `Expected` / `Context`。graph 规则的 `Asset` 是 graph 路径，P8 的是 `<关卡包>:<actor>.<组件>` |

不带 `Spec` 块。修法要看图，按 `Findings` 手写 `EditPCGGraph` 的 spec。

## Stream metric 工作流

逐 sublevel 测加载卸载耗时，并把耗时对上规模。入口 `run_stream_metric.ps1`，一条命令跑完四步。

```bash
powershell -File Plugins/UAssetWorkbench/scripts/run_stream_metric.ps1
powershell -File Plugins/UAssetWorkbench/scripts/run_stream_metric.ps1 -PersistentLevel "/Game/Maps/L_Foo"
```

### 参数

| 参数 | 说明 |
| --- | --- |
| `-PersistentLevel` | 可选，直接指定 persistent level 的包路径，不给就从拓扑报告里推 |
| `-ScanDir` | 枚举 level 的内容根，默认 `/Game` |
| `-SkipExport` | 跳过 `LevelExport` 那一步，导出没变化时用 |
| `-TimeoutSec` | 默认 1800 |

### 流程

1. 跑 `AuditLevelTopology` 出拓扑报告。这一步总是执行，即使显式给了 `-PersistentLevel`，下一步要靠报告拿目标 level 的 sublevel 列表
2. 对目标 level 的 sublevel 跑 `LevelExport` 拿规模数据，`-SkipExport` 跳过这一步
3. 起 unattended 编辑器跑 `stream_metric_probe.py` 测耗时
4. 跑 `stream_metric_report.py` 把耗时与规模合并成表并打印

挑 persistent level 的逻辑在 ps1 里: 读拓扑报告，没给 `-PersistentLevel` 时找唯一的 `PersistentHost`，找到多个就报错并列出候选，要求显式指定。

探针把 `AlwaysLoaded` 的 sublevel 在内存里换成 `LevelStreamingDynamic`，再起 Simulate 逐 sublevel 测量。换类这一步永远不保存，`AlwaysLoaded` 的 `ShouldBeLoaded` 恒为真，不换就没法卸载。耗时写 `Saved/StreamMetric/stream_metric_result.json`，增量写入，编辑器崩了也留得住。

前提: 不能有 `UnrealEditor` 正在运行，脚本会直接报错退出。

测量项: 每个 sublevel 的 unload 墙钟毫秒、gc 毫秒、load 墙钟毫秒、最差帧毫秒。

### 环境变量契约

探针读这两个，手动设置后也可以脱离 ps1 单独跑探针。

| 变量 | 作用 |
| --- | --- |
| `UAW_PERSISTENT_LEVEL` | 直接指定 persistent level 的包路径，优先级最高 |
| `UAW_TOPOLOGY_REPORT` | `AuditLevelTopology` 报告路径，探针从里面找唯一的 `PersistentHost` |

## stream_metric_report.py

把 stream metric 的耗时与 `LevelExport` 的规模按 level 名合并，出一张能看出趋势的表。`run_stream_metric.ps1` 最后一步会自动调它，也可以单独跑。

单看毫秒数说明不了问题，跟规模对起来才知道耗时是随内容量线性长，还是被别的因素主导。

```bash
python Plugins/UAssetWorkbench/scripts/stream_metric_report.py
```

输入: `Saved/StreamMetric/stream_metric_result.json` 与 `Intermediate/UAssetExport` 下的 `LevelExport` 产物。

### 参数

| 参数 | 说明 |
| --- | --- |
| `--project` | 项目目录，默认按脚本位置往上推三级 |
| `--result` | stream metric 结果 JSON，默认按 `--project` 推导 |
| `--csv` | 额外写一份 CSV |

输出: 每个 sublevel 一行，列 actors、components、unique static meshes、load_ms、ms/1k（每千组件的加载毫秒）、unload_ms、gc_ms，按组件数排序，末行给合计。缺某个 level 的 `LevelExport` 产物时会把它列出来，提示先跑 `LevelExport`。

规模解析直接复用 `level_budget_audit.py`，没有重复实现。

## level_budget_audit.py

离线读 `LevelExport` 的 JSON，出组件预算表。

```bash
python Plugins/UAssetWorkbench/scripts/level_budget_audit.py --budget 2000
```

前置: 先跑过 `LevelExport`，这个脚本只读那些产物。

### 参数

| 参数 | 说明 |
| --- | --- |
| `--project` | 项目目录，默认按脚本位置往上推三级 |
| `--levels` | `Intermediate/UAssetExport` 下的相对 glob，默认 `Game/**/*.json` |
| `--budget` | 每个 level 的组件预算，默认 2000 |

输出: 每个 level 一行，列 actors、components、lights、spline mesh components、unique static meshes，以及组件数与预算的比值，超预算的行会标出来。
