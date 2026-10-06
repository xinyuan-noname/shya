# shya AST 节点与宏插槽类型

> 本文面向**写 shya 宏的人**。权威来源是编译器源码，本文与它一一对应：
> `src/shya.h` 的 `enum class NK`、`src/parser.cpp` 的 `nodeKindName()`、
> `src/macro.cpp` 的 `astKindTable()` / `astSlotTypeNames()` / `slotTypeFromName()` / `evalWhen()`。
> 相关文档：[`shya-language-reference.md`](shya-language-reference.md)、
> [`python-users.md`](python-users.md)。

---

## 1. AST 节点总览

每个节点有一个 `NK::` 枚举名（编译器内部用的），以及一个由 `nodeKindName()` 返回的
**CamelCase 字符串**（`ast` / `core` 命令打印的就是它，诊断信息里出现的也是它）。

三列分类的含义：

| 分类 | 含义 |
| --- | --- |
| 表达式 | 有值，能出现在 `x = <这里>` 的右侧；可作 `expr` 插槽 |
| 语句 | 占据整行，不能当值用；可作 `stmt` 插槽 |
| 模板内部 | 只存在于宏定义体内，展开期被消费；不能作插槽类型（见 §3） |

### 1.1 字面量

| `NK::` | `nodeKindName()` | 分类 | 说明 | shya 例子 |
| --- | --- | --- | --- | --- |
| `NK::Num` | `Num` | 表达式 | 十进制/小数/科学计数/`0x` `0b` `0o` 数字。`text` 保留原文，`num` 是值 | `42`，`0xff`，`1_000` |
| `NK::MathConst` | `MathConst` | 表达式 | `~...` 数学字面量，**词法期已折叠成数**，所以 AST 里只剩一个 `num` | `~pi`，`~ln4`，`~deg~e` |
| `NK::Str` | `Str` | 表达式 | 单/双引号字符串。**转义不解析**，`"a\nb"` 的 `text` 是四个字符 `a \ n b` | `"红"`，`'black'` |
| `NK::Tpl` | `Tpl` | 表达式 | 反引号模板字符串。`names` 存字面片段、`list` 存 `${...}` 里的 shya 表达式 | `` `血 ${hp}` `` |
| `NK::Bool` | `Bool` | 表达式 | 布尔字面量，值在 `flag` 里（`true` → `flag = true`） | `true`，`false` |
| `NK::Void` | `Void` | 表达式 | 唯一空值，产物是 `undefined`；`null` / `undefined` 已从语言移除（`LEX009`） | `void` |
| `NK::Ident` | `Ident` | 表达式 | 标识符，名字在 `text`。`_` 也是 `Ident`，但 `flag = true` 表示「空占位」 | `hp`，`player`，`_` |

### 1.2 表达式

| `NK::` | `nodeKindName()` | 分类 | 说明 | shya 例子 |
| --- | --- | --- | --- | --- |
| `NK::ArrayLit` | `ArrayLit` | 表达式 | 数组字面量，元素在 `list` | `[1, 2, 3]` |
| `NK::ObjectLit` | `ObjectLit` | 表达式 | 对象字面量，成员是 `Prop` 节点 | `{ x: 1, y: 2 }` |
| `NK::Prop` | `Prop` | 表达式 | 一个 `名字: 值` 对。两种来源：对象字面量的成员；宏调用的**具名插槽** `#a:` | `{ x: 1 }` 里的 `x: 1`；`@m { #a: ... }` 里的 `#a` |
| `NK::Unary` | `Unary` | 表达式 | 一元运算，`text` 是运算符，`a` 是操作数 | `!ok`，`-n`，`typeof x`，`new Map()` |
| `NK::Binary` | `Binary` | 表达式 | 二元运算（`+ - * / % ^ ~/ +/ -/ \/ && \|\|`）。`@ts` 桥的链式拼接内部用一个 `text == "concat"` 的 `Binary` | `a + b`，`a ^ 2` |
| `NK::Compare` | `Compare` | 表达式 | 比较/相等/类型判断。链式比较时中间操作数放在 `list` 里 | `a < b`，`x is array`，`x is not void` |
| `NK::Ternary` | `Ternary` | 表达式 | 三元，`a` 条件、`b` 真值、`c` 假值 | `c ? a : b` |
| `NK::Call` | `Call` | 表达式 | 调用，`a` 是被调用者、`list` 是实参；`flag = true` 表示安全调用 `?.()` | `draw(2)`，`f?(x)` |
| `NK::Member` | `Member` | 表达式 | 成员访问 `a.name`（`text` 是名字）。`flag = true` 表示写法上带了参数表 `a.name(...)`；`b` 非空时表示链式片段 | `player hp`，`console log(1)` |
| `NK::Index` | `Index` | 表达式 | 下标，`a` 容器、`b` 下标 | `arr[0]`，`m[key]` |
| `NK::Spread` | `Spread` | 表达式 | 展开 `...x`，可用在数组字面量与实参里 | `[...xs]`，`f(...args)` |
| `NK::Await` | `Await` | 表达式 | `await`，`a` 是等待的表达式 | `await p` |
| `NK::MacroApply` | `MacroApply` | 表达式 | 宏调用节点；宏展开阶段被消费掉，展开后的核心 AST 里只应出现在宏声明体内 | `arr @len`，`@zip(a, b)` |
| `NK::TsRaw` | `TsRaw` | 表达式 | `@ts{ ... }` 原样透传块，`raw` 是原文本、`list` 是被替换进来的插槽节点 | `@ts{arr.length}` |
| `NK::RangeExpr` | `RangeExpr` | 表达式 | `start:end,step`，**只能出现在 `@range` 里**，别处用会报 `CGN003` | `@range 0:100,2` |
| `NK::Assign` | `Assign` | 表达式 | 赋值（表达式形式，供 `for` 头部等多目标场景）。`targets` / `values` 是两侧列表 | `x = 1`，`a, b = b, a` |
| `NK::IncDec` | `IncDec` | 表达式 | `++` / `--`。**只作为语句成立**，不构成更大的表达式 | `i++`，`--n` |
| `NK::SlotRef` | `SlotRef` | 模板内部 | 宏模板里的 `#名字`，`text` 是插槽名 | 宏体内的 `#cond` |
| `NK::SlotList` | `SlotList` | 模板内部 | 不定项插槽 `...#名字` 的配套节点 | `macro @m(#x, ...#rest: expr[])` 里的 `#rest` |
| `NK::Optionalize` | `Optionalize` | 模板内部 | 模板里的 `?` 前缀，展开时把带它的调用片段变成 `?.` 调用 | `@safe` 里的 `?#slot` |
| `NK::TypeRef` | `TypeRef` | 表达式 | 类型标注写成节点的情况（`type` 插槽的实参） | `fn f(t: type)` 的实参 |
| `NK::Empty` | `Empty` | 模板内部 | 空节点：展开期被删掉的 `@when` 分支、缺失的 `stmt` 插槽都会落到它 | —— |

### 1.3 语句

| `NK::` | `nodeKindName()` | 分类 | 说明 | shya 例子 |
| --- | --- | --- | --- | --- |
| `NK::Program` | `Program` | 模板内部 | 整个文件；也是 `@when` 分支体的容器类型 | —— |
| `NK::Block` | `Block` | 语句 | 花括号块，语句在 `list` | `{ console log(1) }` |
| `NK::ExprStmt` | `ExprStmt` | 语句 | 表达式语句：`a` 是那个表达式 | `console log("x")` |
| `NK::Decl` | `Decl` | 语句 | 变量声明，`text` 是 `let`/`const`（省略时推断后写回），`names` 名字，`list` 初值，`typeAnn` 标注 | `let hp = 4` |
| `NK::If` | `If` | 语句 | 条件分支，`a` 条件、`b` 真分支、`c` 是 `elif`/`else`（又是一个 `If`） | `if hp < 1 { } else { }` |
| `NK::Case` | `Case` | 语句 | `case` 分派，`list` 是 `CaseArm` | `case suit { ... }` |
| `NK::CaseArm` | `CaseArm` | 语句 | 一个分支：`patterns` 匹配模式、`list` 分支体（恰好一条语句）、`flag` 表示 `fallthrough` | `"red": console log("红")` |
| `NK::ForWhile` | `ForWhile` | 语句 | 条件循环，产物是 `while` | `for n < 3 { n++ }` |
| `NK::ForOf` | `ForOf` | 语句 | 遍历循环，`names` 循环变量（两个表示解构）、`a` 被遍历的集合 | `for k v of @entries(m) { }` |
| `NK::ForRange` | `ForRange` | 语句 | `@range` 循环，`a` 是那个 `RangeExpr` | `for i of @range 0:9,2 { }` |
| `NK::FnDecl` | `FnDecl` | 语句 | 函数声明/表达式，`text` 名字、`names` 参数名、`list` 函数体；`flag` 表示 `async` | `fn f(a: number): void { }` |
| `NK::Return` / `NK::Throw` / `NK::Break` / `NK::Continue` | 同左 | 语句 | 跳转语句，值放在 `a` | `return out`，`throw new Error("boom")` |
| `NK::Try` | `Try` | 语句 | `a` 是 try 体、`b` 是 catch 体（名字在 `text`）、`c` 是 finally 体 | `try { } catch e { }` |
| `NK::Import` | `Import` | 语句 | `spec` 是模块路径。`.shya` 结尾走宏导入（不产物），其它原样透传 | `import { readFile } from "node:fs"` |
| `NK::Export` | `Export` | 语句 | `export`，原样透传 | `export default main` |
| `NK::Declare` | `Declare` | 语句 | `declare` 声明宿主对象/宿主函数，**不产出任何 JavaScript** | `declare Player { name: string }` |
| `NK::Empty` | `Empty` | 模板内部 | 空语句：`_`、缺失的 `stmt` 插槽、被丢弃的 `@when` 分支 | `_` |

### 1.4 宏模板内部

| `NK::` | `nodeKindName()` | 说明 |
| --- | --- | --- |
| `NK::MacroDecl` | `MacroDecl` | `macro @name(#a, #b) { ... }` 整体。`names` 参数名、`typeAnns` 参数标注、`flags` 是否不定项、`list` 模板体 |
| `NK::When` | `When` | `@when(条件)`，`a` 是条件表达式，`list` 是 `WhenArm` |
| `NK::WhenArm` | `WhenArm` | 一个 `@when` 分支。`text` 是 `y` / `n`（具名分支），`list` 是分支体 |
| `NK::Each` | `Each` | `@each(#项 of #列表) { ... }`，`text` 是项名，`names[0]` 是列表名 |
| `NK::SlotRef` / `NK::SlotList` / `NK::Optionalize` | 同左 | 见 §1.2 |
| `NK::Program` / `NK::Empty` / `NK::TypeRef` | 同左 | 见 §1.2 与 §1.3 |

---

## 2. 可作为宏插槽类型的 AST 节点

插槽类型写在宏参数上：

```shya
macro @swapIf(#cond: compare, #yes: stmt, #no: stmt) { ... }
```

`astKindTable()` 是完整名单。**表里的名字全部是小写**，而 `nodeKindName()` 打印的是
CamelCase，两者不是同一个东西：`NK::Binary` 的节点种类名是 `binary`，诊断里显示 `Binary`。

### 2.1 六个非 AST 的基础/语义插槽类型

这六个由 `slotTypeFromName()` 处理，`astKindsForTypeName()` 对它们返回空表——
它们描述的是**插槽的用法类别**，不是某个具体节点种类。

| 类型名 | 接受的实参 | 含义 | 例子 |
| --- | --- | --- | --- |
| `expr` | 任何表达式（默认值，不写标注就是这个） | 需要一个值 | `macro @twice(#x) { #x + #x }` |
| `stmt` | 任何语句；**表达式会自动包成 `ExprStmt`**；可以缺省（渲染成空） | 需要一条语句 | `@swapIf(a < 2, console log("y"), console log("n"))` |
| `type` | 类型标注（`TypeRef` 或写成型名的表达式） | 需要一个类型名 | `macro @cast(#t: type) { ... }` |
| `expr[]` | 只接受不定项插槽 `...#name` 收集到的列表 | 变长参数 | `macro @m(#x, ...#rest: expr[])` |
| `callExpr` | 调用片段：`NK::Call`，或写成 `@safe` / `@safe_share` 的 `MacroApply` | 需要 `f(args)` 形状 | `@share` 的 `...#slots: callExpr` |
| `safeCallExpr` | 带 `?.` 的调用片段 | 需要安全调用 | `@safe` 的 `...#slots: callExpr` 配合 `@when(#slot is safeCallExpr)` |

`callExpr` 与 `safeCallExpr` 在绑定检查上互相放宽（`typeMatches()` 允许二者互换，
`expr` 也总是通过），真正区分它们的是**宏体里的 `@when`**：`@safe` 就是靠
`@when(#slot is safeCallExpr)` 决定要不要插 `?`。

### 2.2 AST 节点种类名（`astKindTable()` 全表）

| 插槽类型名 | 接受的 `NK` | 含义 | 例子（实参写法） |
| --- | --- | --- | --- |
| `numLit` | `Num` | 数字字面量 | `42` |
| `mathLit` | `MathConst` | 数学字面量（折叠后） | `~pi` |
| `strLit` | `Str` | 字符串字面量（必须是字面量，变量不算） | `"hello"` |
| `tplLit` | `Tpl` | 模板字符串字面量 | `` `x${1}` `` |
| `boolLit` | `Bool` | 布尔字面量 | `true` |
| `voidLit` | `Void` | `void` | `void` |
| `ident` | `Ident` | 标识符 | `hp` |
| `arrayLit` | `ArrayLit` | 数组字面量 | `[1, 2]` |
| `objectLit` | `ObjectLit` | 对象字面量 | `{ x: 1 }` |
| `prop` | `Prop` | 属性对 / 具名插槽实参 | `x: 1` |
| `unary` | `Unary` | 一元表达式 | `!ok` |
| `binary` | `Binary` | 二元表达式 | `a + 1` |
| `compare` | `Compare` | 比较 / `is` 判断 | `a < 2` |
| `ternary` | `Ternary` | 三元表达式 | `c ? a : b` |
| `call` | `Call` | 调用 | `draw(2)` |
| `member` | `Member` | 成员访问 | `player hp` |
| `index` | `Index` | 下标 | `arr[0]` |
| `spread` | `Spread` | 展开 | `...xs` |
| `await` | `Await` | `await` | `await p` |
| `macroApply` | `MacroApply` | 宏调用（展开前） | `@zip(a, b)` |
| `slotRef` | `SlotRef` | `#名字`（只在模板体内，作为实参出现的情况极罕见） | —— |
| `tsRaw` | `TsRaw` | `@ts{ ... }` 块 | `@ts{1 + 1}` |
| `rangeExpr` | `RangeExpr` | 范围表达式（**也是基础语义类型之一**） | `0:10,2` |
| `assign` | `Assign` | 赋值 | `x = 1` |
| `decl` | `Decl` | 变量声明 | `let x = 1` |
| `incDec` | `IncDec` | 自增/自减 | `i++` |
| `ifStmt` | `If` | `if` 语句 | `if a { }` |
| `caseStmt` | `Case` | `case` 语句 | `case x { }` |
| `caseArm` | `CaseArm` | case 的单个分支 | `"r": console log(1)` |
| `whileStmt` | `ForWhile` | 条件循环（**名字是 `whileStmt`，不是 `forStmt`**） | `for n < 3 { n++ }` |
| `forOf` | `ForOf` | 遍历循环 | `for c of hand { }` |
| `forRange` | `ForRange` | `@range` 循环 | `for i of @range 0:9 { }` |
| `fnDecl` | `FnDecl` | 函数声明 | `fn f() { }` |
| `returnStmt` | `Return` | `return` | `return 1` |
| `throwStmt` | `Throw` | `throw` | `throw new Error("x")` |
| `tryStmt` | `Try` | `try/catch/finally` | `try { } catch e { }` |
| `breakStmt` | `Break` | `break` | `break` |
| `continueStmt` | `Continue` | `continue` | `continue` |
| `importStmt` | `Import` | `import` | `import { a } from "./m.mjs"` |
| `exportStmt` | `Export` | `export` | `export fn pub() { }` |
| `block` | `Block` | 块 | `{ console log(1) }` |
| `declareStmt` | `Declare` | `declare` 声明 | `declare Player { }` |
| `exprStmt` | `ExprStmt` | 表达式语句 | `console log(1)` |

一个类型名可能对应多个 `NK`（表结构是 `name → vector<NK>`），当前表里每个名字恰好一个。

**实参面的限制**：类型名可标注不等于「随便怎么传都能传进去」。实参由
`parseRangeOrExpr` / `parseTernary` 解析，所以下面这些名字**在语法上无法提供实参**：

| 类型名 | 为什么不给传 |
| --- | --- |
| `declareStmt` | `declare` 不以表达式开头，括号实参里直接 `SYN001` |
| `importStmt` / `exportStmt` | 同上 |
| `exprStmt` | `ExprStmt` 是语句层的包装，只由 `stmt` 插槽自动包出来（`parseStatement` 的产物），没有对应写法 |
| `decl` | 变量声明只能整行出现；`let x = 1` 在实参位置解析成 `Ident` |
| `caseArm` | 分支只能在 `case { }` 里出现；具名插槽给的是整个 `Block` |
| `incDec` | `++` / `--` 只能作为语句，不构成表达式 |

需要它们时改用 `stmt` 插槽：`stmt` 会自动把表达式包成 `ExprStmt`，
也可以用具名插槽块传多行语句。`declare` 走具名插槽是可行的：

```shya
macro @b(#body: stmt) {
  #body
}

@b {
  #body: declare Card { suit: string }
}
```

具名插槽块里的 `declare` 会正常生效：`collectDeclares()` 在类型检查前遍历整棵调用树
（包括宏调用节点的 `Prop` 具名插槽值），所以 `Card` 会被注册，
后续 `c suit` 编译成属性读取 `c.suit` 而不是调用 `c.suit()`。
反过来，`macroApply` 这类以表达式开头的名字可以正常传：`@idM(@zip([1], [2]))` 通过绑定检查。

---

## 3. 哪些节点不能当插槽类型，为什么

`astKindTable()` 刻意排除了 10 个节点种类：

| 节点 | 为什么不能标注 |
| --- | --- |
| `When` | 它就是「决定保留哪一支」的机制本身。把一支 `@when` 当成实参替换进来，等于在实例化过程中改写正在执行的模板 |
| `WhenArm` | 分支是模板的结构件，不是模板的输入 |
| `Each` | `@each` 是编译期遍历，展开时被消费；把它当实参替换会让展开器去遍历一个尚未绑定的列表 |
| `SlotRef` | `#名字` 是**引用插槽的语法**，不是值。允许替换它就能让模板重写自己的引用图 |
| `SlotList` | `...#名字` 的不定项容器，绑定由 `bindArguments()` 生成，不能由实参提供 |
| `MacroDecl` | 宏定义是模板的载体；替换它等于运行中替换模板 |
| `Program` | 整个文件是展开的最高层输入，不能作为某个插槽的内容 |
| `Empty` | 「什么都没有」不是可传递的值；它由展开期生成（缺失的 `stmt`、被丢弃的分支） |
| `Optionalize` | `?` 前缀是模板内的修饰语法，展开期翻译成 `?.`；由实参提供会让 `@safe` 的语义被外部改写 |
| `TypeRef` | 类型标注节点走 `type` 插槽这条独立通道，不参与 AST 种类匹配 |

`src/macro.cpp` 的注释把理由压缩成一句：

> The macro-template internals (program, empty, macroDecl, when, whenArm, each,
> slotList, optionalize, typeRef) are deliberately NOT addressable:
> **substituting one of them into a template would rewrite the template itself.**

换句话说：**能标注的节点是模板的「材料」，不能标注的节点是模板的「机器」。**
想让宏接受一段 `if`、一个块、一个调用片段、一个字符串字面量，都有对应的种类名；
想让宏接受「一个 `@when`」或「一个 `#引用`」在概念上就不成立。

另外有两个节点虽然也没进表，但原因不同——它们在宏展开阶段就被消解掉了，
根本活不到绑定检查：`NK::Define`（`define` 常量，展开时就地替换）与
`NK::ForEach`（`@each` 展开后留下的遍历节点）。

---

## 4. `@when(#x is <类型名>)` 的判定顺序

`evalWhen()` 读的是 `Compare` 节点，`text` 为 `is` / `is not`，右侧必须是一个
`Ident`（否则 `want` 为空字符串）。顺序是**严格的三段回退**：

```
want = 右侧标识符文本
① 插槽形态    stmt / callExpr / safeCallExpr / rangeExpr / expr
② 值类型      array string number boolean object map set fn void rangeExpr unknown
③ AST 种类    astKindTable() 里的任何名字（binary / call / member / strLit / …）
都不是        →  Unknown
```

逐段说明：

1. **① 插槽形态**用的不是实参的节点种类，而是 `inferSlotType(arg)` 的结果。
   `expr` 在这一段判定为「不是 `stmt` 也不是 `type`」即真——它表达的是「这个实参能当值用」。
   `rangeExpr` 在这一段判定为 `SlotType::RangeExpr` 即为真；②里也列了 `rangeExpr`，
   但因为 ① 已经给出结论，实际以 ① 为准。
2. **② 值类型**走 `StaticTyper::type(arg)`，是一个**只看字面量初始化和显式标注的启发式**静态类型：
   `array` → `TK::Array`，`string` → `TK::Str`，`map` → `TK::Map`，以此类推。
   类型推不出来时是 `TK::Unknown`，**这一段的每个 `isKind` 都会返回 `Unknown`**，
   只有显式写 `#x is unknown` 才能命中 `TK::Unknown`。
3. **③ AST 种类**查 `astKindTable()`：查得到就一定是 `True` / `False`（逐个比较 `arg->kind`），
   查不到才返回 `Unknown`。所以写错一个种类名 **不会报错**，只会永远不匹配，
   最终以 `MAC020` 的形式暴露出来。

### 4.1 为什么字面量的种类名是 `strLit` / `numLit` / …

`string`、`number`、`boolean` 在 ② 里已经被**值类型**占用了，`void`、`array`、`object`、
`map`、`set`、`fn`、`unknown` 同理。它们的**节点种类**名必须换一套拼写，
否则 `@when(#x is string)` 就有两个互相冲突的含义。于是：

| 值类型（②） | 节点种类（③） | 对应的 `NK` |
| --- | --- | --- |
| `string` | `strLit` | `Str` |
| `number` | `numLit` | `Num` |
| `boolean` | `boolLit` | `Bool` |
| `void` | `voidLit` | `Void` |
| —— | `tplLit` | `Tpl` |
| —— | `mathLit` | `MathConst` |

`mathLit` 没有冲突对象，但它和 `strLit` / `numLit` 一样以 `Lit` 收尾，保持一族拼写。
判断的差别是实打实的：

```shya
macro @which(#x) {
  @when(#x is string) console log("静态类型是字符串（可能是变量、参数、字面量）")
  @when(#x is strLit) console log("实参就是一个字符串字面量")
}

let s: string = "hello"
@which(s)        // -> 静态类型是字符串
@which("hello")  // -> 静态类型是字符串 + 实参就是一个字符串字面量（两条都命中）
```

（`@when` 各分支是独立的，命中多条就保留多条。）

### 4.2 三值判定，以及 `Unknown` 为什么不会被选中

`evalWhen()` 内部用 `enum class Tri { True, False, Unknown }`，返回时只认 `True`：

```cpp
return ev(cond) == Tri::True;
```

`!` 由 `triNot()` 处理（`Unknown` 取反还是 `Unknown`）；
`&&` 只要有一侧 `False` 就是 `False`，两侧都 `True` 才是 `True`，其余 `Unknown`；
`||` 对偶。所以 `#x is array || #x is string` 在类型未知时整体是 `Unknown`，
`@when` 不收，该分支被丢掉。

由此得到一条重要的**编译期保护**：如果一个宏体里全是 `@when`、而所有分支都被丢掉
（一条都没匹配），展开结果为空，编译器直接报错而不是悄悄生成空代码：

```
error: 宏 `@len` 的 @when 分支一个都没有匹配：无法静态确定参数类型 [MAC020]
```

这就是「`Unknown` 不选分支」的用处：类型信息不足时宁可报错，也不猜。
`{a: 1} @len` 触发它（对象字面量不是 `map`），把结果标注成 `unknown` 的表达式也会触发它。
调试时设 `SHYA_DEBUG_WHEN=1`，每次判定会往 stderr 打印
`[when] want=… staticType=… -> true/false/unknown`。

---

## 5. 命名约定

| 形状 | 含义 | 是否做结构检查 |
| --- | --- | --- |
| 小写 | 插槽 / AST 类型：`expr` `stmt` `callExpr` `binary` `strLit` `ifStmt` … | 是。实参节点种类必须在接受集合里，否则 `MAC015` |
| 首字母大写 | 值类型 / 宿主类型：`Player` `Card` `Map` `Set`（以及 `array` 之外的泛型实参） | 否。**不检查真实结构**，只当作 `instanceof Player` 这类宿主契约 |

大写名字直接交给类型检查器当宿主类型用：`#target: Player` 的宏参数会被当作 `Player`，
成员访问按宿主类型规则处理（需要 `declare Player` 才会生成属性读取）。
它**不会**校验你传进来的对象是不是真的 `Player` —— 这一点和 `array` / `string`
这些有 `isKind` 检查的值类型完全不同。小写名字则一定参与判定，
所以 `#x: compare` 传一个 `Ident` 就是 `MAC015`。

唯一的中间地带是那六个基础插槽类型：`expr` / `stmt` / `type` / `expr[]` /
`callExpr` / `safeCallExpr` 都是小写、都参与检查，但它们不是 AST 节点种类。

---

## 6. 怎么写一个用 AST 节点做参数的宏

三个来自 `tests/cases/11-ast-types.shya` 的真实宏。

### 6.1 `@swapIf`：按节点形状挑参数

```shya
macro @swapIf(#cond: compare, #yes: stmt, #no: stmt) {
  if #cond {
    #yes
  } else {
    #no
  }
}
```

- `#cond: compare` —— 只接受 `NK::Compare`。传 `Ident`、`Binary` 都会被绑定期拦下。
- `#yes: stmt` —— 表达式实参会自动被包成 `ExprStmt`（见 `bindArguments()`），
  所以 `console log("yes")` 这种调用能直接当语句传。
- 缺省：`stmt` 插槽可以不传（渲染成 `Empty`），末尾参数省略不会报 `MAC016`。

```shya
let a = 1
@swapIf(a < 2, console log("  yes"), console log("  no"))
@swapIf(a > 2, console log("  yes"), console log("  no"))
```

### 6.2 `@kindOf`：用 `@when` 在展开期分派

```shya
macro @kindOf(#x) {
  @when(#x is binary) console log("  -> binary")
  @when(#x is call) console log("  -> call")
  @when(#x is member) console log("  -> member")
  @when(#x is strLit) console log("  -> string literal")
  @when(#x is numLit) console log("  -> number literal")
  @when(#x is arrayLit) console log("  -> array literal")
  @when(#x is tsRaw) console log("  -> @ts block")
  @when(#x is ident) console log("  -> identifier")
}
```

这些名字都走 §4 的 ③ 段（①② 不认识它们），逐个实测的命中结果：
`@kindOf(a + 1)` → `binary`；`@kindOf(a toStr)` → `member`；`@kindOf("text")` → `strLit`；
`@kindOf(42)` → `numLit`；`@kindOf([1, 2])` → `arrayLit`；`@kindOf(@ts{1 + 1})` → `tsRaw`；
`@kindOf(a)` → `ident`。

容易踩的坑：**`console log(1)` 是 `member`，不是 `call`**。`a b(args)` 在 shya 里是
「成员链上的一个带参访问」，AST 里是 `Member(flag=true)`；只有 `f(args)` 这种
**纯括号调用**才生成 `NK::Call`。要匹配调用请写 `@kindOf((f(1)))`。

### 6.3 `@inBlock`：把整块语句夹起来

```shya
macro @inBlock(#b: block) {
  console log("block start")
  #b
  console log("block end")
}
```

`#b: block` 只接受 `NK::Block`，所以调用必须用具名插槽块的形式
（`#b:` 后面解析出来的正好是一个 `Block`）：

```shya
@inBlock {
  #b: console log("  inside the block")
}
```

### 6.4 不匹配时报什么

种类不对会在**绑定参数时**立刻报 `MAC015`，消息里同时给出要求的类型名和实参的真实
节点种类名（`nodeKindName()` 的输出，CamelCase）：

```
error: 宏 `@echoLit` 的参数 `#x` 需要 AST 节点 `strLit`，但传入的是 `Num` [MAC015]
error: 宏 `@swapIf` 的参数 `#cond` 需要 AST 节点 `compare`，但传入的是 `Ident` [MAC015]
```

消息模板：

```
宏 `@<宏名>` 的参数 `#<参数名>` 需要 AST 节点 `<要求的类型名>`，但传入的是 `<实际节点种类>`
```

下半句用的是**节点种类**名（`Num` / `Ident` / `Block`），不是插槽类型名——
拿它去 `astKindTable()` 里反查时记得转成小写加 `Lit`/`Stmt` 那套拼写。
