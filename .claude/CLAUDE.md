# UAsset Workbench

Unreal Engine 5 Editor-only plugin，版本 2.5.3。让脚本与 AI agent 能对 uasset 做非交互操作，五组能力 Export / Import / Edit / Migrate / Audit。

## 项目结构

```
src/                                  UE5 插件，复制到 Plugins/ 即可使用
├── UAssetWorkbench.uplugin
├── scripts/                          wrapper 与辅助脚本
└── Source/UAssetWorkbench/
    ├── Public/ 与 Private/           各自按 Export / Import / Edit / Migrate / Audit 分五个子目录
    │                                 共享层（Module / Util / Version / QueueSubsystem）在两者根部
    └── UAssetWorkbench.Build.cs
Docs/                                 给 AI agent 读的中文文档，六份
```

## 版本管理

版本号定义在 `src/Source/UAssetWorkbench/Public/UAssetWorkbenchVersion.h`。
修改版本时同步更新 `.uplugin` 的 `Version` 和 `VersionName` 字段。

## 代码规范

- Allman 风格大括号，4 空格缩进
- 不依赖任何项目模块，只使用 Engine API 和引擎插件（Niagara）
- 注释禁止 Doxygen 风格（`/** */`、`///`、`@param` 等），一律用 `//`，注释里不用分号断句
- 插件内不出现任何具体游戏项目的名字、类名前缀或资产路径，示例一律用中性名
- 语义指 level 的地方写 Level 不写 Map，`TMap` / `ContainsMap` / `.umap` 这类 UE API 与扩展名除外
- 退出码用 `EUAssetWorkbenchExitType`（Success 0 / Failed 1 / EditorConflict 2 / IssuesFound 3），不写裸 int。IssuesFound 只有 Audit 组用，表示跑完了但报告里有问题

## Log Category

| Category | 用于 |
| --- | --- |
| `LogUAssetWorkbenchCore` | 共享层 |
| `LogUAssetWorkbenchExporter` | Export 组 |
| `LogUAssetWorkbenchMigrator` | Migrate 组 |
| `LogUAssetWorkbenchImporter` | Import 组 |
| `LogUAssetWorkbenchEditor` | Edit 组 |
| `LogUAssetWorkbenchAuditor` | Audit 组 |

## Commandlet 分组规则

run 名后缀 `Export` 进 Export 组，后缀 `Import` 与前缀 `Create` 进 Import 组，前缀 `Edit` 进 Edit 组，前缀 `Audit` 进 Audit 组，其余是 Migrate 组。

这套规则在 C++ 的 `UAssetWorkbench::ResolveGroup` 与 `scripts/run_commandlet.sh` 的 `case` 分支各写了一遍，改一处必须改两处。

## Commandlet 添加流程

1. 按分组规则定组，在 `Public/<组>/` 创建 `{Name}Commandlet.h`，继承 `UCommandlet`
2. 在 `Private/<组>/` 创建对应 `.cpp`。引用自己的 header 必须带分组前缀，例 `#include "Export/LevelExportCommandlet.h"`，因为 UBT 只把 `Public/` 根加进 include path，子目录不递归
3. Include `UAssetWorkbenchModule.h`（log category 与 exit type）和 `UAssetWorkbenchVersion.h`
4. `Main()` 开头打印版本号，返回值走 `ToExitCode(EUAssetWorkbenchExitType::...)`
5. Export 组输出路径统一 `Intermediate/UAssetExport/`，JSON 必须包含 `ExporterVersion` 和 `ExportType` 字段
6. 如需新的 Engine module 依赖，添加到 `Build.cs`；如需引擎插件依赖，同步添加到 `.uplugin` 的 `Plugins` 数组
7. 更新 `README.md` 与 `README_CN.md` 的对应组表格，以及 `Docs/` 的对应文件
