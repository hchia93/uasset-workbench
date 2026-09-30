# uasset-workbench

**English** | [中文](README_CN.md) | [Version](Version.md)

![Claude Code](https://img.shields.io/badge/Claude_Code-black?style=flat&logo=anthropic&logoColor=white)
![Unreal Engine 5](https://img.shields.io/badge/Unreal_Engine-5.7-blue?logo=unrealengine&logoColor=white)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)

An editor plugin that lets scripts and AI agents read, write, edit and audit Unreal Engine 5 uassets.

## What it solves

<div align="center">

| Problem | Today | Group |
| :-- | :-- | :-: |
| Can't read | `.uasset` is binary, graphs, timelines, widget trees and parameters only show in the editor | Export |
| Can't write | UMG layout is hand-dragged in the editor, no versionable or replayable write path | Import |
| Can't edit in place | Add a component, wire a pin, set a default: three manual edits, repeated per Blueprint | Edit |
| Renames break refs | CoreRedirects miss Blueprint implementers and consumers, and old paths in levels | Migrate |
| Can't audit | Broken refs, texture compression, material usage flags, level budgets, no batch query | Audit |

</div>

## Five groups

<p align="center">
  <img src="assets/five-groups.svg" alt="Export, Import, Edit, Migrate, Audit loop around the uasset" width="720">
</p>

- **Closed loop**: most Export output doubles as an Import or Edit spec, Audit `Spec` blocks feed Edit directly
- **Editor open**: runs inside the editor process, one Message Log page per run, no need to close the editor
- **Editor closed**: runs as a commandlet, read-only Export and Audit work as usual, writes wait for the editor
- **Writes**: all or nothing per asset, a Blueprint that fails to compile is never saved, Edit defaults to dry run

## Supported assets

<div align="center">

| Asset | Export | Import | Edit | Migrate | Audit |
| :-- | :-: | :-: | :-: | :-: | :-: |
| Blueprint | ✓ | | ✓ | ✓ | |
| Widget Blueprint | ✓ | ✓ | ✓ | ✓ | |
| Anim Blueprint | ✓ | | ✓ | ✓ | |
| AnimSequence / AnimMontage | ✓ | | ✓ | | |
| DataTable | ✓ | | ✓ | | |
| DataAsset | ✓ | ✓ | | | |
| Material / MaterialInstance | ✓ | | ✓ | | ✓ |
| Texture | ✓ | | ✓ | | ✓ |
| Niagara System | ✓ | | | | |
| Behavior Tree | ✓ | | | | |
| Level | ✓ | | ✓ | ✓ | ✓ |
| PCG Graph | ✓ | | ✓ | | ✓ |
| Any asset | | ✓ | | ✓ | |

</div>

Any asset: `CreateAsset` creates from a spec, `RenameAsset` / `DuplicateAsset` / `ResaveAsset` rename, duplicate, resave

RunName per cell: decision table in [Docs/AI-Guide.md](Docs/AI-Guide.md)

## Quick start

**1. Add to your project**

Copy `src/` into your project's `Plugins/UAssetWorkbench/`, add one entry to the `Plugins` array in `.uproject`, then regenerate project files and build.

```json
{ "Name": "UAssetWorkbench", "Enabled": true }
```

Requires: Unreal Engine 5.7, plugin built with the project

**2. Run**

One wrapper, editor open or closed.

```bash
UE="<UE_PATH>"
PROJECT="<PROJECT_DIR>/MyProject.uproject"
RUN="Plugins/UAssetWorkbench/scripts/run_commandlet.sh"
export MSYS_NO_PATHCONV=1   # Git Bash: keeps /Game/... from turning into a Windows path

# Export: Blueprint graph to JSON
bash "$RUN" "$UE" "$PROJECT" BlueprintEdGraphExport "/Game/Blueprints/BP_Foo"

# Import: rebuild a widget tree from a spec
bash "$RUN" "$UE" "$PROJECT" WidgetLayoutImport "" 10 600 '-spec="C:/temp/WBP_Foo.spec.json"'

# Edit: change a Blueprint from a spec, -apply writes to disk
bash "$RUN" "$UE" "$PROJECT" EditBlueprint "" 10 600 '-spec="C:/temp/BP_Foo.edit.json" -apply'

# Migrate: after a C++ event rename, rewire BP overrides to the new event
bash "$RUN" "$UE" "$PROJECT" RedirectBlueprintEvent "/Game/Blueprints/BP_Foo" 10 600 \
    '-OwnerClass="/Script/MyModule.MyActor" -OldEvent="OnPickedUp" -NewEvent="HandlePickedUp"'

# Audit: check build settings of every texture
bash "$RUN" "$UE" "$PROJECT" AuditTexture "" 10 600 '-scandir="/Game"'
```

Arguments: header of `run_commandlet.sh`

Export output: `Intermediate/UAssetExport/<AssetPath>_r<revision>_<timestamp>.json`

## For AI agents

<div align="center">

| Doc | Content |
| :-- | :-- |
| [Docs/AI-Guide.md](Docs/AI-Guide.md) | Entry point, decision table, call templates, pitfalls |
| [Docs/Export.md](Docs/Export.md) | Every exported JSON field |
| [Docs/Import.md](Docs/Import.md) | Spec format |
| [Docs/Edit.md](Docs/Edit.md) | Every spec key and op |
| [Docs/Migrate.md](Docs/Migrate.md) | Repair steps after a rename |
| [Docs/Audit.md](Docs/Audit.md) | Rule tables and the stream metric workflow |

</div>

Docs language: Chinese

## Why not the official toolchain

The official entry points for AI and automation, MCP, Remote Control and the Python API, evolve with each engine release, with capability gaps and regressions in some editor subsystems.

The workbench uses only commandlets and stable engine APIs, and its output is JSON you can version, diff and replay. When the engine lacks a mechanism, one more commandlet adds it.

Live interactive tools solve a different problem, scene building, PIE debugging, live tuning. The two coexist.

## How it generalizes

UE is only the proving ground, the three reusable parts do not depend on it.

| Reusable part | What it is | Transfers to |
| --- | --- | --- |
| Pattern | Two-way bridge between opaque binary and AI-readable structured text | Any GUI-locked proprietary format, DCC / CAD / BIM / EDA / simulation |
| Architecture | Heartbeat-routed adaptive dual pipeline, live in-process and headless paths produce identical output | Any heavy host with both an interactive and a headless mode, Houdini / Maya / Blender / Revit / MATLAB |
| Serialization discipline | Token-aware export, delta from archetype, cap and sample on overflow, a grep plus range-read contract | Context engineering for any LLM data pipeline |

## License

[MIT](LICENSE) - Hyrex Chia
