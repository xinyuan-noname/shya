# shya 编译器架构

本文档描述 `D:\project\shya` 中 shya 编译器（C++20，MSVC 构建，入口 `build.bat`）的真实实现。
所有符号名均取自源码；代码位置以 `src/` 下的文件为单位。

## 0. 定位与总体流水线

shya 是一个强类型 DSL，编译目标为 ES2026 JavaScript。设计稿
(`docs/design-draft.txt`) 规定的步骤在实现中一一对应：

```
源码文本
  │  Lexer        src/lexer.cpp      -> std::vector<Token>
  │  Parser       src/parser.cpp     -> AST（NK::* 通用节点）
  │  MacroExpander src/macro.cpp     -> 核心 AST（宏展开 + 脱糖 + define 内联）
  │  TypeChecker  src/typecheck.cpp  -> 诊断（+ exprTypes_ 标注）
  │  Codegen      src/codegen.cpp    -> ES2026 文本
```

驱动函数是 `compileSource()`（声明于 `src/shya.h`，定义在 `src/main.cpp`），它按顺序做
五件事，任何一步产生 error 就提前返回：

1. `Lexer lexer(source, filename); lexer.tokenize(bag)`；`opt.dumpTokens` 时输出 token 转储；
2. `Parser parser(tokens, bag); parser.parseProgram()`；`opt.dumpAst` 时输出 `dumpAst()`；
3. `MacroExpander expander(bag); expander.expand(program)`；得 `core`，`opt.dumpCoreAst` 时转储；
4. 除非 `opt.noTypecheck`，`TypeChecker checker(bag); checker.check(core)`；
5. `Codegen codegen(bag, copt); result.code = codegen.generate(core, filename)`。

`opt.warningsAsErrors` 在最后把 warning 追加为 error（`"-Werror"`）。`CompileResult` 携带
`ok / code / tokensDump / astDump / coreAstDump / diags`。CLI 子命令
(`build / check / tokens / ast / core / run`) 与选项 (`-o --out`、`--no-typecheck`、
`--keep-types`、`--warnings-as-errors`/`-Werror`、`-q --quiet`) 都在 `src/main.cpp`；
`--keep-types` 目前只被解析、并未接线到 `CodegenOptions::stripTypes`（`(void)keepTypes`）。

标准库是编译进二进制的 shya 源码字符串 `kStdlibSource`（`src/stdlib.cpp`），由
`MacroExpander::installStdlib()` 用同一套 `Lexer`/`Parser` 解析后注册为 `system` 宏。

## 1. AST：一个通用 `Node`

`src/shya.h` 里没有继承层次，只有

```cpp
enum class NK { /* 表达式 + 语句共 50 余种 */ };

struct Node {
    NK kind;
    Pos pos;
    std::string text;      // 名字 / 运算符 / 关键字 / 原始载荷
    std::string raw;       // 逐字源码（模板字符串、@ts 块）
    double num;
    bool flag, flag2;      // 主/次布尔属性
    std::string typeAnn, typeAnn2;
    NodePtr a, b, c, d;
    std::vector<NodePtr> list, targets, values, patterns, defaults;
    std::vector<std::string> names, typeAnns;
    std::vector<bool> flags;
    std::string resolved;
    bool constant;
};
```

各字段的实际含义（按节点种类约定，不按类型系统约束）：

- `text`：标识符名、二元/一元运算符、关键字、`@` 宏名、`try` 的 catch 变量名；
- `raw`：模板字符串原文（`NK::Tpl`）、`@ts{}` 载荷（`NK::TsRaw`）、`import`/`export {…}` 原样文本；
- `num`：数值字面量与已折叠的数学字面量；
- `flag` / `flag2`：主/次布尔属性——`Bool` 的值、`Member` 是否带调用、`Call` 是否安全
  调用、`CaseArm` 是否 `default` / 是否 `fallthrough`、`ArrayLit` 是否为逗号序列、`_`
  占位符、`FnDecl` 是否 `export`、链片段标识（见 5.3）、展开产生的可内联 `Block`；
- `a..d`：子节点，如 `If` 的 `a`=条件 `b`=then `c`=else/elif，`ForRange` 的
  `a`=start `b`=end `c`=step `d`=集合；`list`：语句表、参数表、实参表、`case` 分支表、模板体；
- `targets`/`values`：`NK::Assign` 的左右两侧（`x, y = 1, 2`）；`patterns`：`CaseArm` 的
  模式列表（逗号分隔的多个 `case` 标签）；
- `names`：成员链名 / 参数名 / 循环变量名，`Tpl` 的静态片段，`TsRaw` 的插槽名；
- `typeAnns`/`flags`/`defaults`：逐参数的标注 / 是否变参 / 默认值（`MacroDecl` 的
  `flags` 即 `variadic`）；`resolved`/`constant` 预留给后续阶段（现由 `exprTypes_` 承担）。

`mk(kind, pos)` 是唯一的构造助手；`nodeKindName(NK)` 与 `dumpAst(root)`（`src/parser.cpp`）
用于 `ast`/`core` 子命令的可读转储。

### 1.1 比较链的约定（重要）

`1 < 2 < 3` 不是嵌套的 `Binary`，而是**一个** `NK::Compare`：`a` = 最左操作数，
`list` = 右侧操作数序列 `[2, 3]`，`names` = 运算符序列 `["<", "<"]`，
`text` = 第一个运算符的镜像（`names[0]`），便于只关心"是不是比较"的代码。

`parseRelational()` 与 `parseEquality()` 各自维护这么一条链（关系层与相等层彼此独立，
所以 `a == b < c` 不会合成一条链）。宏展开的 `evalWhen` 因此需要
`NodePtr rhs = n->b ? n->b : (n->list.empty() ? nullptr : n->list[0]);` 这种兼容读法；
`instExpr` 会同时搬运 `b` 与 `list`、并原样复制 `names`。代码生成见 9.4。

## 2. 词法分析（`src/lexer.cpp`）

`Tok` 枚举把若干"语法上有正字法特殊性"的东西单独成类：
`Identifier / Number / String / TemplateString / Punct / Keyword / At / Hash / Under /
MathLit / TsBlock / End`。

- **关键字表** `keywords()` 是静态集合，含 `if elif else case default fallthrough for of
  break continue let const define macro import from export as fn async await return throw
  try catch finally is not instanceof void true false this typeof new null undefined`。
  注意 `number`/`string`/`boolean`/`object` 不是关键字，但 `void`/`null`/`undefined` 是
  ——这个不对称会在 `is` 右侧暴露出来（见 `decisions.md`）。`_` 单独产生 `Tok::Under`。
- 标点表 `kPuncts[]` **按最长优先**匹配。`~/` 在进入 `lexMath` 之前就被截获：`~` 后跟
  `/` 时是截断除法运算符，不是数学字面量。`@` 只有 `@ts` 紧跟 `{`（允许空格）时走
  `lexTsBlock()`，其余产生单个 `Tok::At`；`#` 产生 `Tok::Hash`。
- **换行感知**：`tokenize()` 在跳过空白/注释之前记下 `line_`，跳完之后若 `line_` 变了
  就置 `tok.newlineBefore = true`。这是"换行即语句分隔符"的唯一机制。

字面量与"怪字面量"：数字支持 `_` 分隔符、`0x/0X/0b/0B/0o/0O` 前缀
（`std::stoull(text, nullptr, 0)` 求值）、小数、`1.`、指数（指数回退时恢复
`i_/line_/col_`）；字符串用 `'` 或 `"`，转义按两字符原样保留进 `Token::value`，遇换行或
EOF 报 `LEX004`；模板字符串 `lexTemplate()` 整段存进 `Token::text/value`，内部不解析，
`${…}` 的切分留给解析器。

**数学字面量** `lexMath()` 是 `~` + 符号 + 数值参数，在**词法阶段就折叠成 `double`**。
常量表 `consts[]`：`π/pi`、`ℯ/e`、`τ/tau`、`∞/inf`（同时接受 UTF-8 与 ASCII 拼写）；
函数表 `fns[]`：`lg`、`ln`、`db`（10·lg）、`deg`（角度→弧度）、`rad`（弧度→角度）、
`sqrt`。参数可以是普通数字、`-数字`，或另一个 `~…` 字面量（`~ln~deg360` 递归下降）。
符号名只吃字母，所以 `~lg10` 天然是 `lg` 作用于 `10`；`~dege` 之类报 `LEX006` 并带
"若要自然常数请写 `~e`"的提示，未知符号报 `LEX007`。
`lexTsBlock()` 手写花括号配平、跳过字符串与注释，`raw` 是 `{` 与 `}` 之间的**逐字载荷**；
`@ts` 未闭合报 `LEX008`，块注释未闭合报 `LEX003`。

## 3. 语法分析（`src/parser.cpp`）

递归下降 + 优先级爬升，表达式层级自上而下：`parseExpression → parseAssignment →
parseTernary → parseLogicalOr → parseLogicalAnd → parseEquality → parseRelational →
parseAdditive → parseMultiplicative → parsePower → parseUnary → parsePostfix →
parsePrimary`。

- `parseAssignment` 先解析逗号分隔的目标列表，只有看到 `=` 才构造 `NK::Assign`；否则单
  元素直接返回，多元素退化成一个 `ArrayLit(flag=true)`——即**逗号序列**，只对 `for` 头
  有意义。
- `parsePostfix` 吃成员访问（`player getHp`）、`#slot` 形式成员、调用 `(...)`、安全调用
  `?(...)`、下标 `[...]`、后缀宏 `@op`、`++/--`。
- `parseStatement()` 分派 `if/case/for/try/fn/async fn/macro/define/let/const/import/
  return/throw/break/continue/export`，并处理 `x: Type = expr` 形式的带标注声明（内部
  `NK::Decl` 的 `text` 记为 `"infer"`）。`parseProgram`/`parseBlock` 跳过裸 `;`，并保证
  每个循环至少前进一个 token，否则报 `SYN002`。
- `parseCase()` 支持 `case (expr) {…}` / `case expr {…}` / 无主语 `case {…}`；分支体
  **恰好一条语句**（`fallthrough` 算作该语句），`arm->flag2` = 落空，`arm->list` 最多一项，
  `default` 后面是否有 `:` 都能解析。
- `parseFor()` 用回退策略区分 `for x of …` / `for a b of …` / `for (cond)`：先尝试吃掉
  1~2 个标识符再看有没有 `of`，没有就 `i_ = save` 回退。`parseTry()` 接受
  `catch (e) {…}` 与 `catch e {…}`。
- `parseImport()`/`export {…}`/`export *` 走"原样文本"路径：把 token 文本拼回 `Node::raw`
  由代码生成直接透传（所以 import 语法基本等于 JS/TS）。
- 类型标注 `parseTypeAnnotation()` 只做**规格化字符串**：`bool`→`boolean`，
  `null`/`undefined`→`void`，`X[]`→`array<X>`（连续 `[]` 递归包裹），`A|B` 拼接，泛型
  `array<map<string,number>>` 递归。真正的树由类型检查器用 `parseTypeString` 重新解析。

## 4. 换行即分隔符（设计上的"硬约束"）

设计稿没有语句终止符。实现选择：**换行分隔语句，`;` 可选**。由此产生三处显式的
"不跨行"规则，全部读 `Token::newlineBefore`：

1. **成员访问不跨行**：`parsePostfix()` 里 `cur().kind == Tok::Identifier &&
   !cur().newlineBefore` 才继续接成员。原因：`console log(1)` 下一行 `console log(2)`
   必须解析成两条语句，而不是 `console.log(1).console.log(2)`。
2. **后缀宏的裸 slot 实参不跨行**：`parseMacroApply()` 的 `if (!prefix)` 分支里
   `if (c.newlineBefore) break;`——下一行开始的是新语句，不是本宏的又一个参数。
3. **`@each` / `@when` 拼接不跨行**：`parseStatement()` 只在 `cur().kind == Tok::At &&
   !cur().newlineBefore` 时才把行尾的 `@each/@when` 续接到同一语句。

`;` 有两种身份：可选终止符（几乎每个 `parseXxx` 末尾都 `acceptPunct(";")`）与**空语句**
（`parseStatement()` 遇到 `;` 直接返回 `NK::Empty`）。`_` 单独成句同样返回 `NK::Empty`
（"待填"占位，相当于 Python 的 `pass`）；只有当 `_` 后面紧跟 `@` 时才不走"空语句"这条
捷径，而是按表达式解析，于是 `_ @macro { … }` 成为"后缀 target 为 `_`"的宏调用
（`_` 绑成空节点，`tests/cases/02-macros.shya` 的 `@banner` 就是这个用法）。

## 5. 宏系统（`src/macro.cpp`）

### 5.1 定义与注册

```cpp
struct MacroDef { std::string name; std::vector<std::string> params;
                 std::vector<SlotType> paramTypes; std::vector<bool> variadic;
                 std::vector<NodePtr> body; bool system; Pos pos; };
enum class SlotType { Expr, Stmt, Type, ExprList, CallExpr, SafeCallExpr, RangeExpr, Unknown };
```

`slotTypeFromName()` 把源码里的 `expr / stmt / type / expr[] / expr... / callExpr /
safeCallExpr / rangeExpr` 映射到 `SlotType`；缺省（即"未标注"）是 `Expr`，与设计稿
"不标注默认为 expr"一致。注册顺序是 `installStdlib()` → `collectMacroDecls(program)`；
`registerMacro` 的规则是：与 stdlib 同名时用户宏覆盖它，与用户宏重名报 `MAC001`。
宏不需要预先声明，**可以在使用之后定义**（`collectMacroDecls` 是独立的整树扫描）。

### 5.2 三种应用形式

`parseMacroApply(const Token& atToken, bool prefix)` 覆盖三种形式：

1. **前缀式** `@op(x, y)`：`prefix == true`，实参进 `n->list`；`@range` 有专属形状
   （`@range 0:10,2` 直接吃一个范围表达式）。
2. **后缀式** `x @op`：`parsePostfix` 先把接收者解析成表达式，再调
   `parseMacroApply(cur(), false)` 并把接收者塞进 `app->a`。所以**后缀目标会被当成宏的
   第一个位置实参**（`bindArguments` 里 `if (call->a) positional.push_back(call->a);`）。
3. **命名插槽块** `{ #name: … }`：`isNamedSlotBlock()` 判定（要求紧跟 `{` `#` 标识符 `:`），
   `parseMacroApply` 把它解析成一串 `NK::Prop{ text = 槽名, a = Block }` 进 `n->list`；
   `bindArguments` 见到 `Prop` 就按名字放进 `named` 映射。插槽可以只给一部分（`stmt` 槽
   允许缺省），命名槽与位置参数可以混用：绑定分两趟——先按名字填，再把剩下的位置实参
   按声明顺序填进"没被具名占掉"的参数。

### 5.3 裸 slot 实参 = 单个"调用片段"

后缀式宏在没有括号时按 `parseSlotFragment()` 收集实参，规则刻意收得很窄：可选前缀
`- + ! not` 或 `...`；一个 `parsePrimary()`，其后只允许 `(...)`、`?(...)`、`[...]` 尾缀；
**不接成员链**。所以 `player nextSeat @share _p recover(2) draw(2)` 一共得到**四个**实参：
后缀目标 `Member(player,nextSeat)`、`Ident(_p)`、`Call(recover,2)`、`Call(draw,2)`——而不是
把 `_p.recover(2).draw(2)` 当成一个。片段若最终是 `Ident` 或 `Ident` 打头的 `Call` 会打上
`flag2 = true`，表示"这是接收者的成员名，不是自由标识符"，类型检查器据此不发 `TC010`
（`console log(...)` 里的 `log` 走 `Member`，也不经过标识符查表）。更复杂的表达式必须写
括号形式 `@macro(a + b)`。

### 5.4 模板实例化

模板体在解析期就以普通 `NK` 语句保存（`parseMacroTemplateBody`），`macroDepth_ > 0` 时
`#name` 才合法。实例化在 `InstCtx { self, binds, depth, bag }` 上做双向遍历：

- `instStmts(tpl, ctx, out)` 逐条语句实例化，`instExpr(tpl, ctx)` 逐表达式实例化。两者都
  显式列举节点种类（而不是"默认深拷贝"），只有少数兜底分支调 `cloneNodePublic`——这是
  "宏展开不遗漏子节点"的保证方式。
- `@when`：`chooseWhenArms()` 求值条件，按 `arm->text` 是 `y`/`n` 选分支（只识别这两个
  名字）；无名分支（`@when(){…}` 或 `@when(cond) stmt`）在条件为真时取 `list[0]`。
- `@each(#slot of #slots)`：`bindArguments` 把变参打包成一个 `ArrayLit(flag = true)`
  （"变参列表"），`instEachStmts/instEachExpr` 遍历这个 `list`；若绑定不是该形状则视为
  单元素列表。每轮为循环变量 `each->text` 建一层新 `Bindings`（浅拷贝外层）后递归实例化。
- 语句位置与表达式位置是**分开的**：`instWhenExpr`/`instEachExpr` 把结果拼成
  `NK::Binary{ text = "concat" }`（`makeConcat` 顺带压平嵌套 concat、丢弃 `_`/空节点），
  语句位置的 `@when`/`@each` 则把选中分支的语句直接展开进当前列表。
- 深度保护 `kMaxMacroDepth = 64`，超限报 `MAC012`。

实例化完成后 `expandMacroStatements()` 把展开结果**再喂回** `expandNode/expandStmts`，
所以宏可以嵌套、产出里还能有 `@宏`；展开为多条语句时只能作语句用（`MAC013`）。

### 5.5 `@ts` 的标记（marker）技巧

`@ts` 是"逐字 JS"，但设计稿要求其中的 `#slot` 在展开期替换。`makeTsRaw(tpl, ctx)` 用
二段式解决：

1. 扫描 `tpl->raw`，把每个 `#name`（`#` 后跟字母或 `_`）替换成
   `'\x01' + 十进制下标 + '\x01'` 的**标记串**，并把对应节点追加进新建 `TsRaw` 节点的
   `list`（`names` 记录原槽名，用于报错）；
2. 生成阶段 `Codegen::genTsRaw(n)` 再扫一遍 `n->raw`，遇到 `\x01…\x01` 就解析下标、
   `genExpr(n->list[idx], 0)` 填回去。

好处是展开期不需要懂 JS 语法（不必操心优先级或括号），只做最终文本拼接；`kMarker` 常量
在 `macro.cpp` 与 `codegen.cpp` 各定义一份。引用未绑定槽报 `MAC003`，直接引用不定项插槽
报 `MAC004`，下标损坏报 `CGN001`。`@ts` 里除 `#slot` 外的一切都不解析。

### 5.6 `#slot` 机制小结

`#name` 出现的位置决定语义：宏模板体内的表达式位置 → `NK::SlotRef`（被 `instExpr` 换成
实参节点）；`#x #frag` → `NK::Member{ a=…, b=SlotRef }`（生成链片段）；`@ts{…}` 载荷内 →
标记串 + `list`（见 5.5）；`@when(#x is array)` 这类条件 → `NK::Compare`（编译期判定，
见 7）；调用点的 `{ #name: … }` → `NK::Prop`（命名插槽，按名绑定）；宏定义参数表的
`#name: type` → 名字进 `MacroDef::params`、类型进 `paramTypes`。

`bindArguments()` 分两趟绑定（先具名、后位置补位），其校验为：缺参数时 `Stmt` 槽允许
省略（记为 `nil`，渲染为空），其它槽报
`MAC016`；多余位置实参报 `MAC017`；未知命名参数 `MAC018`；类型不符 `MAC015`。特别地，
给 `Stmt` 槽传表达式时会被自动包成 `NK::ExprStmt`（`b.slot` 改成 `Stmt`），所以"表达式
当语句用"能成功而不是报错；`_`（`isNilNode`）作为实参表示"没有目标"，绑定里
`b.nil = true`。

## 6. 编译期常量

`define` 由 `collectDefines()` 整树收集，初始值先 `substituteDefines` 再 `foldConstant`；
折叠不成常量就报 `MAC019`："`define` 只接受字面量、数学字面量、其它 define 与它们的
常量运算"。`foldConstant` 能折叠数字/数学字面量、字符串（含 `+` 拼接）、布尔、`void`、
一元 `- + !`，以及 `+ - * / ~/ +/ -/ \/ % ^` 的数值运算（`~/` 用"向零截断"：
`a/b >= 0 ? floor : ceil`）。展开阶段 `substituteDefines(program)` 把 `NK::Ident`（非 `_`）
替换成折叠结果，`expandStmts` 跳过残留的 `NK::Define`，于是**编译产物的确不含该变量**；
若类型检查阶段还看到 `NK::Define` 报 `TC008`（编译器内部错误）。

数学字面量则在**词法阶段**就折成 `double`（`Lexer::lexMath`），所以 `~ln~deg360` 是
一次词法递归的结果；生成的 JS 里直接是数字字面量（`formatNumber` 保证最短往返表示），
而不是 `Math.log(...)` 调用。

## 7. 宏展开期的启发式类型环境

`@when(#x is array)` 必须在真正的类型检查器之前判定，因此 `macro.cpp` 里有一个私有的
`StaticTyper`（只依赖 `MacroExpander::env_`，`std::unordered_map<std::string, TypePtr>`）：

- `expand` 第 3 步扫全树建 `env_`：`NK::Decl` 有标注用 `parseTypeString(typeAnn)`，否则用
  `StaticTyper::type(初值)`；`NK::FnDecl` 的参数标注（去掉 `...` 前缀）也进 `env_`。
  **同名变量只记录第一次出现的类型**。
- `StaticTyper::type(n)` 能推：字面量、`ArrayLit`（元素类型取第 0 个）、`ObjectLit`、
  `Unary{!, typeof, -, +}`、`new Map()/new Set()/new Array()`、`&&`/`||`（两侧同型）、
  `+`（任一侧 string⇒string，两侧 number⇒number）、`Compare⇒boolean`、`Ternary`（两臂
  同型）、`Index`（array/map/string/range 的元素类型）。
- `evalWhen()` 是**三值**求值（`Tri::{True,False,Unknown}`）：先做 `SlotType` 判定
  （`#x is callExpr`/`safeCallExpr`/`rangeExpr`/`stmt`/`expr`），再做静态类型判定
  （`array/string/number/boolean/object/map/set/fn/void/rangeExpr/unknown`）。只有确定为
  `True` 才算命中——`Unknown` 一律不选，所以分支选不中会报 `MAC020` 而不是静默产出空代码。
  设 `SHYA_DEBUG_WHEN` 环境变量会打印判定过程。

**已知局限**（都源自"启发式"本质）：不支持函数返回值推断、不支持分支合流后的类型、
不支持成员/调用结果的类型（一律 `unknown`）、不支持重新赋值的类型变化（`env_` 里
"先到先得"）、`any`/`unknown` 无法区分。设计上这就是"够用即可"的静态环境，真正的判定权
在 `src/typecheck.cpp`。

## 8. 类型系统与类型检查（`src/typecheck.cpp`）

### 8.1 类型格

`enum class TK { Any, Unknown, Void, Null, Bool, Num, Str, Array, Object, Map, Set, Fn,
Union, TypeParam, Class, Literal, Range, Never }`，`struct Type` 用
`name/elem/key/parts/ret/optional/required/variadic/literal/numLit` 表达结构。
`Type::str()` 负责回显（`array<number>`、`map<string,number>`、`fn(number)->string`、
`a|b`）。构造助手 `tAny/tUnknown/tVoid/tBool/tNum/tStr/tArray/tMap/tSet/tObject/tFn/
tUnion/tNamed/tRange/tRangeOf`（其中 `tAny/tUnknown/…` 返回**静态共享实例**）。

- `typeEquals`：结构比较，union 顺序无关，`Fn` 比较参数表与返回。
- `isAssignable(to, from)`：`any`/`unknown` 双向放行，`never` 是底，union 两侧按成员展开，
  `array/set/map` 递归，`Class→Class` 一律 false（不假装做名义子类型），`Class→object` 放行。
- `parseTypeString(ann)`：`TypeParser` 解析**已被解析器规格化**的字符串，识别
  `number/int/float/double`、`string`、`boolean/bool`、`void/null/undefined/nil`、
  `any/unknown/never`、`object/obj`、`array/list<…>`、`set<…>`、`map/record<…>`、
  `fn/function`、`range/rangeExpr`，以及宏槽类型 `stmt/expr/type/callExpr/safeCallExpr/
  expr[]`（映射成 `tNamed`）。字符串与数字字面量产生 `TK::Literal`；其余名字是
  `TK::Class`（宿主类型，只按名字相等比较）。

### 8.2 作用域与检查项

`TypeChecker` 持有 `scopes_`（`std::vector<std::shared_ptr<Scope>>`，`Scope { vars,
consts }`）与 `exprTypes_`（`std::unordered_map<const Node*, TypePtr>`）。`checkStatements()`
先 `hoistFunctions()`（**函数声明提升**，并把 `required` 参数个数、是否变参记进 `Fn`
类型），再顺序检查：

- **声明与初始化**：`Decl` 有标注就 `checkAssignable(标注, 初值)`（`TC003`）；`const`
  无初值报 `TC007`；变量类型有标注取标注、否则取初值类型。
- **赋值**：`Assign` 的目标若已在作用域内按既有类型检查（`TC003`），不在则**隐式声明**
  （对应"let/const 可省"）；目标是 `const` 报 `TC013`。
- **调用**：被调用者类型是 `Fn` 时检查实参个数（`minArgs = required < 0 ? parts.size() :
  required`，变参时 `maxArgs = -1`），不符报 `TC006`；逐个实参 `checkAssignable` 报
  `TC003`，变参槽把 `array<T>` 拆成 `T`。被调用者不是 `Fn`（含 `unknown`）则不检查
  ——这是渐进类型的关键；链片段调用（`flag2`）**不查表**，不会误报未声明标识符。
- **运算符**：`+` 任一侧是 string 则结果 string（另一侧不是 string/unknown/any 时给
  `TC004` 警告），否则两侧都要求 `number`；`- * / % ~/ +/ -/ \/ ^` 同样要求 `number`，
  `&&`/`||` 取 `tUnion({l, r})`；一元 `- +`、`++/--` 要求 `number`；错误码都是 `TC003`。
- **比较**：`is` / `is not` 的右侧必须是类型名——不是 `NK::Ident` 报 `TC011`，是标识符
  但不属于 `knownTypeName()` 报 `TC012` 警告并说明"将按宿主类型 `instanceof` 处理"；
  普通比较两侧类型互不兼容时给 `TC005` 警告（不是错误）。
- **标识符**：查不到且不属于 `builtinGlobals()`（`Math/Object/Array/JSON/console/Map/
  Set/Promise/Date/…`）时给 `TC010` **警告**，类型记为 `unknown`。
- **`try/catch`**：catch 变量在专属作用域里声明（`unknown`）；`for` 的循环变量按被遍历
  值的元素类型声明（array/set→elem，map→`array<key>`，string→string，range→elem）。
- **未展开残留**：`MacroApply/SlotRef/When/Each/MacroDecl` 还活着报 `TC009`（编译器内部
  错误），`Define` 残留报 `TC008`，`RangeExpr` 到代码生成阶段报 `CGN003`。

## 9. 代码生成（`src/codegen.cpp`）

### 9.1 带优先级的渲染

表达式渲染走一对函数：

```cpp
struct Rendered { std::string text; int prec = 11; };
Rendered genExprP(const NodePtr& n);                   // 产文本 + 自身优先级
std::string genExpr(const NodePtr& n, int parentPrec); // prec < parentPrec 时补括号
```

`precedence(op)`：`|| 1`、`&& 2`、相等/`is`/`instanceof 3`、关系 `4`、加减 `5`、乘除模与
四个除法变体 `6`、`^ 8`，未知 `11`（"原子"）。语句渲染走 `line(str)`（自动缩进 + 换行）、
`genStatements`、`genStatement`、`genBlockOf`、`captureStatement`。

### 9.2 运算符映射

| shya | ES2026 |
| --- | --- |
| `a ~/ b` | `Math.trunc(a / b)` |
| `a +/ b` | `Math.ceil(a / b)` |
| `a -/ b` | `Math.floor(a / b)` |
| `a \/ b` | `Math.round(a / b)` |
| `a ^ b` | `a ** b`（右结合） |
| `a == b` / `a != b` | `a === b` / `a !== b` |
| `a is array` | `Array.isArray(a)` |
| `a is string/number/boolean/fn` | `typeof a === "..."` |
| `a is map/set` | `a instanceof Map` / `a instanceof Set` |
| `a is void/null/undefined` | `a === undefined` |
| `a is object` | `typeof a === "object" && a !== null` |
| `a is 其它名` | `a instanceof 其它名` |
| `a is not T` / `a not instanceof T` | `!(…)` / `!(a instanceof T)` |

四个除法变体的括号：左操作数按 `6`、右操作数按 `7` 渲染，所以嵌套不会串味。

### 9.3 "一切皆是函数"与链片段

设计稿的"没有属性，一切皆是函数，无参括号可省略"落在两处：

1. `NK::Member` 的常规渲染**总是发一次调用**：
   `genExpr(n->a, 11) + "." + n->text + "(" + genArguments(n->list) + ")"`，所以
   `player getHp` → `player.getHp()`，`arr slice(1,3)` → `arr.slice(1, 3)`。
   `hasSideEffects()` 也据此把 `Member` 一律视为有副作用（用于比较链的临时变量提升）。
2. 链片段 `genFragment(n, optional)` 把表达式渲染成"接在接收者后面的一段"：调用片段渲染
   成 `.name(args)` 或 `?.name(args)`；`Call.flag`（`f?(...)`）在参数表前再补一个 `?.`；
   `Member.b`（`#slot` 形式）递归成 `genFragment(n->b, optional)`；裸标识符渲染成
   `.name()`；`NK::Optionalize` 只是把 `optional` 置真。**每条链恰好插入一个 `?.`**：
   `d @safe say("hello") say("nice")` → `d?.say("hello")?.say("nice")`，
   `@safe_share` → `_q?.recover(2)`。

`NK::Binary{ text = "concat" }` 是"头表达式 + 一串链片段"的容器：`genExprP` 先 `walk`
压平，第 0 个按完整表达式渲染，其余逐个 `genFragment`。

### 9.4 链式比较

`genCompare(n)`：`n->list.size() <= 1` 时渲染单个比较（`one(op, l, r)`）；多段时把
`n->a` 与 `n->list` 拼成操作数数组 `o0…on`，逐段渲染 `o0 op0 o1 && o1 op1 o2 && …`。
**内部操作数（`o1…o(n-1)`）在链里出现两次**，因此凡是 `hasSideEffects()` 为真的内部
操作数都被收集成箭头函数的形参，整条链包一层：

```js
1 < tick() < 5
// -> ((__shya_cmp1) => 1 < __shya_cmp1 && __shya_cmp1 < 5)(tick())
```

这样每个内部操作数**只求值一次**（`tests/expected/08-doc-check.js` 就是这个形状，
运行时计数为 1）。`is` 的左侧有副作用时同理，用 `__shya_is` 作形参。整条链的优先级
记作 `2`（等价 `&&`），单条比较记 `3`。

### 9.5 `@range` 脱糖

`MacroExpander::expandNode` 在 `NK::ForOf` 上特判 `n->a` 是 `@range` 宏调用：范围必须是
`NK::RangeExpr`（否则 `MAC010`），循环变量只能一个（否则 `MAC011`），随后生成
`NK::ForRange{ text = 变量名, a = start, b = end, c = step, d = 集合? }`。代码生成时：

- 没有 `d`（纯区间）→ `for (let i = start; i < end; i += step) {…}`；步长在编译期可确定
  为负数时比较号改 `>`（所以 `@range 3:0,-1` 会倒着走），步长非字面量时保守用 `<`；
- 有 `d`（`for v of arr @range 1:3,1`）→ 先 `const __shya_seq = Array.from(coll);`，
  再用 `__shya_i` 计数、循环体内 `const v = __shya_seq[__shya_i];`。
  变量名缺省用 `__shya_i` / `__shya_unused`。

### 9.6 `const/let` 推断

推断由 `countAssignments()` / `scanAssignments()` 驱动，配合两级栈 `scopes_`（名字是否已
声明）与 `assignCounts_`（每个名字在**当前块**里被赋值几次）：

- `scanAssignments` 递归统计：`Assign` 的每个目标 `Ident` +1；`IncDec` +1；`Decl` 本身也
  算一次绑定（`out[name]++`）——所以"声明 + 之后一次赋值"计数为 2；遇到 `NK::FnDecl`
  直接 return（函数体是自己的块，生成函数体时会重新扫）。
- `genStatement(NK::Decl)`：显式 `let` → `let`；显式 `const` 且计数 > 1 → `CGN006` 报错；
  `text == "infer"`（省略了 let/const）→ 计数 > 1 用 `let`，否则 `const`。
- `genStatement(NK::Assign)`：如果目标全是"未声明过的标识符"且左右个数相等，就整体转成
  声明（`const name = …;` / 计数 >1 时 `let`），这正是设计稿
  `player nextSeat @share _p …` 能产出 `const _p = player.nextSeat()` 的原因；否则按普通
  赋值/数组解构（`[a, b] = [x, y];`）渲染。
- 每个块（程序、`{}`、函数体、分支体、`case` 分支、循环体）进入时 `pushAssignCounts`、
  退出时 `popAssignCounts`，所以计数是按词法块局部化的。

### 9.7 其它生成细节

- 数字：`formatNumber` 处理 `NaN/Infinity/-0`，整数用 `%.0f`，其余用 `%.*g` 找最短往返
  表示；`0x/0b/0o` 字面量**原样**输出。字符串走 `escapeJsString`，模板字符串按 `names`
  （静态片段）+ `list`（`${…}` 表达式）重新拼回。
- `NK::Void` 一律输出 `undefined`（设计稿的"undefined 与 null 统一为 void"），`_` 输出空串。
- 对象字面量渲染成 `{ k: v }`，方法简写（`Prop` 的值是 `FnDecl`）渲染成
  `k(params) { … }`，非法标识符键自动加引号。
- 无主语的 `case` 生成 `if / else if / else` 链；有主语的生成 `switch`，非 `fallthrough`
  分支统一补 `break;`，`fallthrough` 分支输出 `// fallthrough` 注释。
- `tsRaw` 节点在语句位置按行重新缩进输出；`import`/`export {…}` 直接透传 `raw`；
  `export default` 与 `export fn` 用 `pendingExport_` 前缀机制。
- 顶部写死两行 banner（`// Generated by the shya compiler from <file>.` 与
  `// Target: ES2026. Do not edit by hand.`），最后统一折叠多余空行。

## 10. 诊断与退出码

`DiagBag`（`src/shya.h`）收集 `Diagnostic { severity, pos, code, message }`，提供
`error/warning/note/hasError/errorCount/warningCount/items/append/clear`。各阶段共用同一个
`bag`，所以一次编译能看到跨阶段的全部诊断。**诊断码按阶段分族**（号段连续）：

- `LEX0xx` 词法：`LEX001` 数值、`LEX002` 未知字符、`LEX003` 块注释未闭合、`LEX004`
  字符串/模板未闭合、`LEX005~007` 数学字面量、`LEX008` `@ts` 未闭合；
- `SYN0xx` 语法：`SYN001` 期望 token、`SYN002` 无法解析并跳过、`SYN003~008` 声明类、
  `SYN009` `async` 位置、`SYN010` `export`、`SYN011/012` `#` 使用位置、`SYN013` 对象键、
  `SYN014` `@ts` 后缺 `{`、`SYN015` 表达式、`SYN016/017` `@each`、`SYN018` `@when` 分支、
  `SYN019` 缺宏名、`SYN020` 命名插槽、`SYN021` 类型标注；
- `MAC0xx` 宏：`MAC001` 重名、`MAC002` 内建库解析失败、`MAC003~007` `@ts`/`@when`/`@each`、
  `MAC008~018` 绑定与展开、`MAC019` `define` 非常量、`MAC020` `@when` 无匹配分支；
- `TC0xx` 类型：`TC001` 重复声明、`TC002` 标注解析、`TC003` 赋值不兼容、`TC004` 字符串
  拼接、`TC005` 比较类型不同、`TC006` 实参个数、`TC007` `const` 无初值、`TC008/009` 阶段
  残留、`TC010` 未声明标识符、`TC011/012` `is` 右侧、`TC013` const 重赋值；
- `CGN0xx` 代码生成：`CGN001` `@ts` 占位损坏、`CGN002` 可选调用被忽略、`CGN003` 范围
  表达式位置错误、`CGN004` 无法处理的节点、`CGN005` `is` 右侧、`CGN006` `const` 重赋值。

`renderDiagnostics(bag, source, filename)`（`src/diag.cpp`）输出
`file:line:col: severity: message [CODE]`，再打印源码行与插入符：`sourceLine()` 取该行，
`utf8WidthAt()` 按 UTF-8 前导字节算出**终端显示宽度**（CJK/全角区段算 2 列），从而让 `^`
对齐中文标识符。

退出码（`main()`）：用法/未知命令/未知选项/读不到文件/写不出文件 → `1`；`help`、`version`、
成功 → `0`；`build`/`check` 有 error → `1`（并打印 `shya: <cmd> failed with N error(s)`）；
`run` 返回子进程 `node` 的退出码。`tokens/ast/core` 子命令只转储、不写文件。
