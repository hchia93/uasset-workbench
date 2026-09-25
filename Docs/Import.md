# Import

读 JSON spec，写回或创建 uasset。

| RunName | 作用 |
| --- | --- |
| `WidgetLayoutImport` | 从 spec 重建 Widget Blueprint 的控件树 |
| `DataAssetImport` | 把 JSON 写进 DataAsset 的属性 |
| `CreateAsset` | 从 spec 创建任意类型的资产 |

三个都吃同一个参数: `-spec="<spec.json 绝对路径>"`。

命名上 `Import` 与 `Export` 是名词做后缀，其余动词在前，`Create` / `Audit` / `Sanitize` / `Redirect` / `Reparent` / `Resave`。前缀 `Create` 归 Import 组，与后缀 `Import` 同属写入侧。

## WidgetLayoutImport

RunName: `WidgetLayoutImport`
参数: `-spec="<spec.json 绝对路径>"`

### spec 格式

顶层字段。

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

属性值字符串就是 Export 导出的那种形式，例 `(Value=1.000000,SizeRule=Fill)`、`(Right=48.000000)`、`HAlign_Fill`。

最小示例。

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

### ClassDefaults

`EditDefaultsOnly` 那类属性住在 generated class 的 CDO 上，不在控件树里，没有这个块就只能在编辑器里手工设。

```json
{
  "AssetPath": "/Game/UI/WBP_Foo",
  "ClassDefaults": {
    "SomeEditDefaultsOnlyProperty": "..."
  },
  "WidgetTree": { }
}
```

应用时机在编译之后，因为编译会重建 CDO。

### 调用

spec 走 `EXTRA_ARGS`，`AssetList` 传空串。目标写在 spec 里，位置参数里再写一遍只会让摘要冒认一个 run 没碰过的资产。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    WidgetLayoutImport \
    "/Game/UI/WBP_Foo" \
    10 600 \
    '-spec="C:/temp/WBP_Foo.spec.json"'
```

### round-trip

`WidgetLayoutExport` 的产物可以直接当 Import 的输入，Import 只读它认识的字段，其余原样忽略。

改布局的常规做法: 先 Export 拿到当前树，改 JSON，再 Import 写回。

### 行为约定

| 约定 | 说明 |
| --- | --- |
| 整树替换 | 不是增量合并，spec 必须描述完整的树，旧根节点在保存时脱落 |
| 跳过 export-only 字段 | `Slots` 是 `AddChild` 重建的活对象列表，`SlotClass` 是导出侧的元数据，喂回去会破坏树，Import 直接跳过 |
| 编译检查 | 导入后编译 Blueprint 并检查状态，`BindWidget` 的名字或类型对不上会在这里报错 |
| 属性失败不致命 | 单个属性 `ImportText` 失败只给 warning，其余照常写入，日志在 `LogUAssetWorkbenchImporter` |

## DataAssetImport

RunName: `DataAssetImport`
参数: `-spec="<spec.json 绝对路径>"`

`DataAssetExport` 的反向操作。

### spec 格式

| 字段 | 含义 |
| --- | --- |
| `AssetPath` | 目标 DataAsset 的资产路径 |
| `Properties` | 属性名到值，值有两种形态 |

```json
{
  "AssetPath": "/Game/Path/DA_Foo",
  "Properties": {
    "Scalar": "12.0",
    "Nested": [ { "Name": "A" }, { "Name": "B" } ]
  }
}
```

### 属性值两形态

| 形态 | 走什么 | 用在哪 |
| --- | --- | --- |
| 字符串 | 反射 `ImportText` | `DataAssetExport` 导出的那种结构字面量，例 `(A=1,B=2)`，所以导出的资产能 round-trip 回去 |
| 对象或数组 | json 转换器 | 手写 spec 时的嵌套结构与数组，可读性好得多。instanced 子对象例外，见下节 |

### instanced 子对象

属性带 `Instanced`，或类带 `EditInlineNew` 加 `DefaultToInstanced` 时，值写成 `{ "Class": ..., "Properties": {...} }`。

| 键 | 含义 |
| --- | --- |
| `Class` | 给了就新建实例，flag 与 Details 面板的类选择器一致，被替换的旧实例移到 transient 包。不给就改现有实例，属性为空时报错 |
| `Properties` | 可选，规则同顶层：字符串走 `ImportText`，支持点路径，嵌套的 instanced 子对象递归 |

`Class` 收完整类路径 `/Script/Module.ClassName`、Blueprint 资产路径，或裸类名（在属性声明类的子类里匹配）。抽象类或非子类报错。

instanced 子对象数组写成这种对象的数组。数组按 spec 长度重设，截掉的尾部元素移到 transient 包，第 N 项带 `Class` 新建第 N 个元素，不带则改它。`[]` 清空。

```json
{
  "AssetPath": "/Game/Path/DA_Foo",
  "Properties": {
    "Shape": { "Class": "/Script/MyGame.MyShape_Box", "Properties": { "Extent": "(X=10.0,Y=10.0,Z=10.0)" } },
    "Modifiers": [
      { "Properties": { "Factor": "0.5" } },
      { "Class": "MyModifier_Scale", "Properties": { "Factor": "2.0" } }
    ]
  }
}
```

JSON 对象键序不保证，子对象的值放进它的 `Properties`，不要在同一对象里另写同级的 `"Shape.Extent"` 键。

### 调用

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    DataAssetImport \
    "/Game/Path/DA_Foo" \
    10 600 \
    '-spec="C:/temp/DA_Foo.spec.json"'
```

### 行为约定

| 约定 | 说明 |
| --- | --- |
| 只写点名的属性 | spec 里没提到的属性保持原值 |
| 全成功才保存 | 任何一个属性写失败就整体放弃保存，不会留下写了一半的资产 |

## CreateAsset

RunName: `CreateAsset`
参数: `-spec="<spec.json 绝对路径>"`，且必须带 `-unattended`

`-unattended` 是必填不是可选。引擎的 `FMessageDialog` 不检查 commandlet 模式，某些创建路径会弹窗阻塞。

### spec 格式

顶层只有 `Assets`，条目数组，按顺序创建。

| 条目字段 | 含义 |
| --- | --- |
| `PackagePath` | 目标目录 |
| `AssetName` | 资产名 |
| `Class` | 资产类，短名或完整路径 |
| `FactoryProperties` | 创建前配到 factory 上的属性 |
| `Properties` | 创建后套到资产上的属性，规则同 `DataAssetImport` 的两形态 |

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

### FactoryProperties

两个属性块分开，是因为有些类型必须在创建前配好 factory，否则创建会失败甚至静默返回空。

| 资产类型 | 必填 | 不填的后果 |
| --- | --- | --- |
| DataTable | `Struct`，字段名是 `Struct` 不是 `RowStruct` | 返回空 |
| Blueprint | `ParentClass` 加 `bSkipClassPicker` | 弹窗阻塞 |
| Texture2D | `Width` / `Height`，且必须是 2 的幂 | 静默返回空 |
| MaterialInstanceConstant | `InitialParent`，可选 | 得到无 parent 的空实例 |

### 调用

`-unattended` 必填，`FMessageDialog` 不检查 commandlet 模式，有些路径会卡住。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    CreateAsset \
    "" \
    10 600 \
    '-spec="C:/temp/create.spec.json" -unattended'
```

### 行为约定

| 约定 | 说明 |
| --- | --- |
| `Class` 推荐完整路径 | 例 `/Script/MediaAssets.MediaPlayer`。短名在引擎里是模糊查找，会打警告，Blueprint 生成类这种要加载包的必须写完整路径 |
| 顺序创建可互相引用 | 后面的条目能在 `Properties` 里引用前面刚创建出来的资产路径，一条 spec 就能把互相引用的一组资产接好 |
| 永不覆盖 | 同名资产已存在时跳过并报告 |

### 能力边界

建不了材质的节点图。`CreateAsset` 能建出空材质，但往里加 `TextureSample` 之类的表达式并连线，走 Python 的 `unreal.MaterialEditingLibrary` 更直接。

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 成功 |
| 1 | spec 解析失败、写入或编译报错，或参数错误 |
| 2 | 编辑器在运行 |

Import 组不会返回 3，3 是 Audit 组专用。

`WidgetLayoutImport` 的进程退出码偶尔是 1 但其实成功了，引擎会改写 commandlet 的退出码。判断成败以日志里的 `Imported layout into ...` 为准，别只看退出码。
