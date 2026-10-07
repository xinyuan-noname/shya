# shya 设计稿 v2 —— 修订说明

> 本文件记录 **v2 相对设计稿原文（`docs/design-draft-v1.txt`）的全部改动**，逐条给出
> 「原文 → 现在 → 依据」。修订后的正文见 `docs/design-draft.txt`；
> 实现层面的取舍细节见 `docs/decisions.md`。
>
> 修订日期：第二轮开发。所有条目都在 `tests/cases/` 里有对应用例。

## 0. 总览

v2 只做了三类改动：

| 类别 | 条目 |
| --- | --- |
| **删减** | 移除 `null` / `undefined`；移除"未做说明的和 js 保持一致"里关于空值的部分 |
| **新增** | `declare` 外置对象声明；`.shya` 宏文件导入；AST 节点作为宏插槽类型；Python 用户宏库；`fn` / 异常 / 模板字符串等（v1 遗漏的函数与宿主语法） |
| **明确** | 关键字表、`@when` 判定顺序、插槽类型全表、诊断码表、`void` 的唯一性 |

## A. 删减

### A1. 移除 `null` 与 `undefined`，只保留 `void`

- **原文**：「控制字面量：统一化为 undefined 和 null 统一为 void」。
  原文既说"统一化"，又保留了两个词；实现上两者都当关键字，容易让人以为可以写。
- **现在**：**只有 `void`**。`null` / `undefined` 不再是关键字，写出来直接报错：

  ```
  error: shya 只有 `void`：`null` 已从语言中移除，请改写为 `void` [LEX009]
  ```

  类型标注里也没有 `null` / `undefined` 别名；`x is void` 编译成 `x === undefined`。
  `@ts{...}` 里的原始 JavaScript 不受限制，那段代码里仍然可以写 `null`。
- **依据**：`src/lexer.cpp` 的 `LEX009`；`tests/cases/01-core.shya`、`08-doc-check.shya`、
  `10-typeops.shya` 全部改用 `void`。

### A2. 移除"位运算"与"三段式 for"的暗示

- **原文**：同时写着"位运算：没有位运算"和循环示例 `for(let x = 1; x<100; x+=2)`。
- **现在**：确认不做位运算（`^` 是乘方、`|` 只用于类型联合）；也**不做**三段式循环头，
  等价写法是 `for x of @range 1:100,2`。复合赋值 `+= -= *= /= %= ^=` 本身是支持的，
  但它们只能作为语句，不能构成表达式。
- **依据**：`tests/cases/09-compound.shya`。

## B. 新增

### B1. `declare` —— 外置（宿主）对象的声明

- **原文**：「无法自定义类，但可以将标注类以对接游戏系统」——只说了能对接，没说什么叫对接。
- **现在**：新增 `declare` 语句，把宿主对象的形状写清楚，**不产出任何 JavaScript**：

  ```shya
  declare Player {
    name: string              // 字段 -> 属性读取 p.name
    hp: number
    hand: array<Card>
    judge(): Card             // 方法 -> 调用 p.judge()
    recover(n: number): void
  }

  declare fn hostRandom(max: number): number
  ```

  带来的效果：

  - **字段读属性、方法发调用**：`p name` → `p.name`；`p judge` → `p.judge()`。
    这是 v1 里"一切皆是函数"规则的必要补充——否则宿主对象的字段根本读不出来。
  - 成员类型参与检查：`p recover("oops")` → `TC003`；`hostRandom()` → `TC006`。
  - 访问未声明成员 → 警告 `TC014`，并在消息里给出补声明的位置。
  - 宏展开期的启发式类型推断也会读 `declare`，所以 `p hand @len` 能正常解析。
- **依据**：`tests/cases/14-declare.shya`（正向）、`15-declare-errors.shya`（诊断）。

### B2. 宏文件的导入方式与其它模块一致

- **原文**：完全没有提到宏能不能跨文件复用。
- **现在**：宏文件和普通模块**共用同一套 import 语法**，只是路径以 `.shya` 结尾：

  ```shya
  import "./pystd.shya"                            // 全部宏
  import { @enumerate, @zip } from "./pystd.shya"  // 具名
  ```

  规则：

  - 相对路径按**当前源文件**解析；也可以放在 `-I <dir>` 指定的搜索根下。
  - 具名导入会**自动带上被依赖的宏**（依赖闭包），所以 `import { @assertAny }` 不用再写 `@any`。
  - 被导入文件的 `define` / `declare` 一并进入导入方。
  - 宏导入**不产出 JavaScript**；被导入文件可以继续导入别的 `.shya`；循环导入报 `MOD002`。
  - 宏文件里不能导入 `.js` / `.mjs`（`MOD004`）；普通 JS 模块原样透传，行为不变。
- **依据**：`src/modules.cpp`；`tests/cases/12-py-macros.shya`、`13-module-errors.shya`。

### B3. AST 节点作为宏插槽类型

- **原文**：「变量需标注语句类型或表达式类型，或者变量的属性，否则默认为 expr」——
  只列了 `stmt, expr, type` 三个类别，以及 `callExpr` / `expr[]` 这样的特例。
- **现在**：**大部分安全的 AST 节点种类都能直接当插槽类型用**，一共 40+ 个名字：

  ```shya
  macro @swapIf(#cond: Compare, #yes: stmt, #no: stmt) { if #cond { #yes } else { #no } }
  macro @echoLit(#x: Str) { console log("literal is " + #x) }
  macro @inBlock(#b: Block) { console log("start")  #b  console log("end") }
  ```

  传错会给出精确诊断：`宏 `@swapIf` 的参数 `#cond` 需要 AST 节点 `compare`，但传入的是 `Num` [MAC015]`。
  `@when` 的判定同样支持节点种类：`@when(#x is Binary)`、`@when(#x is Call)`、`@when(#x is Str)`。

  宏模板的内部节点（`when` / `whenArm` / `each` / `slotRef` / `macroDecl` / `program` /
  `empty` / `optionalize` / `typeRef`）**刻意不能**作为插槽类型——把它们替换进模板会改写模板自身。
- **依据**：`src/macro.cpp` 的 `astKindTable()`；`tests/cases/11-ast-types.shya`；
  完整表见 `docs/ast-nodes.md`。

### B4. 面向 Python 用户的标准库宏

- **原文**：没有标准库清单，只有 `@keys` / `@values` / `@entries` / `@len` 四个示例。
- **现在**：内置宏之外，项目附带一份 Python 风格的宏库 `lib/pystd.shya`：
  `@enumerate` `@zip` `@items` `@count` `@first` `@last` `@sum` `@max` `@min` `@sorted`
  `@reversed` `@unique` `@any` `@all` `@join` `@upper` `@lower` `@strip` `@split` `@replace`
  `@startswith` `@endswith` `@find` `@abs` `@round` `@int` `@float` `@pow` `@mod` `@clamp`
  `@str` `@bool` `@typeOf` `@repr` `@assertAny` `@assertAll`。

  其中 `@mod` 刻意采用 **Python 的 `%` 语义**（结果符号跟随除数），以区别于 shya/JS 的 `%`。
  逐条对照与三个语义陷阱见 `docs/python-users.md`。
- **依据**：`lib/pystd.shya`；`tests/cases/12-py-macros.shya`。

### B5. 补上 v1 遗漏的宿主语法

原文只写了"未做说明的和 js 保持一致"，没有定义任何函数、异常、模块语法。v2 明确：

| 语法 | 形式 |
| --- | --- |
| 函数声明 | `fn name(a: T, b: T = 默认值, ...rest: T[]): Ret { }`、`async fn` |
| 异常 | `try { } catch e { } finally { }`、`throw` |
| 控制 | `return` / `break` / `continue` / `await` |
| 字面量 | 数组 `[]`、对象 `{}`（含方法简写）、模板字符串 `` `a${x}` ``、`a[i]` 下标 |
| 运算 | 三元 `?:`、复合赋值 `+= -= *= /= %= ^=`、`++` / `--`（语句） |
| 模块 | `import` / `export` 透传；`.shya` 走宏加载 |
| 其它 | 数字分隔符 `1_000`、`0x` / `0b` / `0o`、可嵌套块注释、`x: T = v` 带标注声明 |

## C. 明确

### C1. 关键字表

v1 没有给出完整关键字表。v2 明确为：

```
if elif else case default fallthrough
for of break continue
let const define declare macro
fn async await return throw try catch finally
import from export as
is not instanceof typeof new
true false this void
```

### C2. `@when` 的判定顺序

v1 只说"编译期分支"。v2 明确判定顺序为
**插槽形态 → 值类型 → AST 节点种类**，且判定是三值的（`unknown` 不命中任何分支）。
为避免冲突，AST 里的字面量节点写作 `numLit` / `strLit` / `boolLit` / `voidLit` / `tplLit` /
`mathLit`，而 `number` / `string` / `boolean` 始终是**值类型**。

### C3. 插槽类型全表与诊断码表

v1 只有零散示例。v2 给出：

- `docs/ast-nodes.md`：全部 AST 节点 + 全部可用插槽类型（含"不能当类型"的清单）；
- 语言参考第 9 节：`LEX` / `SYN` / `MOD` / `MAC` / `TC` / `CGN` 六个诊断族与代表码。

### C4. 空值与成员访问的两条硬规则

- `void` 是唯一空值（见 A1）；
- 成员访问**默认就是调用**（v1 的"一切皆是函数"），宿主字段要靠 `declare` 才能按属性读（见 B1）。

---

## 附：v2 与 v1 的条目对照

| 设计稿原文位置 | 处理 |
| --- | --- |
| 「类型标注和 ts 规则一致…无法自定义类，但可以…对接游戏系统」 | 保留，并由 B1 的 `declare` 落实 |
| 「控制字面量：统一化为 undefined 和 null 统一为 void」 | **改为只保留 `void`**（A1） |
| 「位运算：没有位运算」 | 保留 |
| 「只有 `for(condition)` 循环、只有 `for of` 遍历」 | 保留，并明确不支持三段式循环头（A2） |
| 「宏函数 / target @操作符 / @ts / @when / @each / @range」 | 保留，展开语义与判定顺序在 C2、C3 明确 |
| 「变量需标注语句类型或表达式类型…否则默认为 expr」 | 扩展为完整 AST 插槽类型表（B3） |
| 「导入：规则与 ts 一致」 | 保留，并新增 `.shya` 宏文件导入（B2） |
| 「未做说明的和 js 保持一致」 | 由 B5 逐项落实 |
