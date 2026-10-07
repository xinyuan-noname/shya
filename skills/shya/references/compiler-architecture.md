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
  │  ModuleLoader src/modules.cpp    -> AST（宏文件导入消费 + define/declare 拼接）
  │  MacroExpander src/macro.cpp     -> 核心 AST（宏展开 + 脱糖 + define 内联）
  │  TypeChecker  src/typecheck.cpp  -> 诊断（+ exprTypes_ / fieldAccesses_ 标注）
  │  Codegen      src/codegen.cpp    -> ES2026 文本
```

驱动函数是 `compileSource()`（声明于 `src/shya.h`，定义在 `src/main.cpp`），它按顺序做
六件事，任何一步产生 error 就提前返回：

1. `Lexer lexer(source, filename); lexer.tokenize(bag)`；`opt.dumpTokens` 时输出 token 转储；
2. `Parser parser(tokens, bag); parser.parseProgram()`；`opt.dumpAst` 时输出 `dumpAst()`；
3. `MacroExpander expander(bag); expander.installStdlib();`，随后
   `loadShyaModules(program, filename, opt.includePaths, expander, bag)` 消费所有
   `import "*.shya"`（见 §7）；
4. `expander.expand(resolved)`；得 `core`，`opt.dumpCoreAst` 时转储；
5. 除非 `opt.noTypecheck`，`TypeChecker checker(bag); checker.check(core)`；
6. `Codegen codegen(bag, copt); codegen.setFieldAccesses(&checker.fieldAccesses());
   result.code = codegen.generate(core, filename)`。

`opt.warningsAsErrors` 在最后把 warning 追加为 error（`"-Werror"`）。`CompileResult` 携带
`ok / code / tokensDump / astDump / coreAstDump / diags`。CLI 子命令
(`build / check / tokens / ast / core / run`) 与选项 (`-o --out`、`-I --include`、
`--no-typecheck`、`--keep-types`、`--warnings-as-errors`/`-Werror`、`-q --quiet`) 都在
`src/main.cpp`；`-I` 把目录追加进 `CompileOptions::includePaths`，是 `import "*.shya"`
的额外搜索根；`--keep-types` 目前只被解析、并未接线到 `CodegenOptions::stripTypes`
（`(void)keepTypes`）。

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
`instExpr` 会同时搬运 `b` 与 `list`、并原样复制 `names`。代码生成见 10.4。

## 2. 词法分析（`src/lexer.cpp`）

`Tok` 枚举把若干"语法上有正字法特殊性"的东西单独成类：
`Identifier / Number / String / TemplateString / Punct / Keyword / At / Hash / Under /
MathLit / TsBlock / End`。

- **关键字表** `keywords()` 是静态集合，含 `if elif else case default fallthrough for of
  break continue let const define macro import from export as fn async await return throw
  try catch finally is not instanceof void true false this typeof new null undefined`。
  注意 `number`/`string`/`boolean`/`object` 不是关键字，但 `void`/`null`/`undefined` 是
  ——这个不对称会在 `is` 右侧暴露出来（见 `decisions.md`）。`_` 单独产生 `Tok::Under`。
  **`null`/`undefined` 仍在关键字表里，但语言里已经不存在**：`tokenize()` 打出
  `Tok::Keyword` 后立刻报 `LEX009`（"shya 只有 `void`：`null` 已从语言中移除，请改写为
  `void`"，消息里的词按实写的 `null`/`undefined` 拼接）。保留词法识别是为了让解析器继续
  把该位置读成 `NK::Void`（表达式位置）或类型名（标注位置）从而恢复解析，但
  `compileSource()` 在第 1 步之后就以 error 提前返回，所以后面的 `null`/`undefined` 分支
  实际不可达。`@ts{...}` 载荷走 `Tok::TsBlock` 整段透传，里面的 `null` 不受影响。
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
  有意义。**复合赋值**在 `=` 之前单独试一遍 `kCompound[] = {"+=", "-=", "*=", "/=", "%=",
  "^=", "**="}`，且只在目标列表恰好一项时尝试：命中就构造 `NK::Assign`，`text` 记去掉 `=`
  的运算符（`std::strlen(op) - 1`），`targets` 是那个唯一目标、`values[0] =
  parseAssignment()`（右结合）。所以 `x ^= 2` 的 `text` 是 `"^"`，代码生成再补成 `**=`。
  它和 `++`/`--` 一样**只能作语句**：`parseAssignment()` 也被 `parseExpression()`（即括号
  表达式）调用，所以 `y = (x += 1)` 里的 `x += 1` 确实会构造出一个复合赋值节点，但它待在
  表达式位置、没有被 `genStatement` 消费，实测报 `CGN004`（"代码生成阶段遇到无法处理的
  节点 `Assign`"）。也就是说这条"只能作语句"的约束由**生成阶段**兜底，不是解析阶段拒绝
  （见 10.6 的 `NK::Assign` 分支）。
- `parsePostfix` 吃成员访问（`player getHp`）、`#slot` 形式成员、调用 `(...)`、安全调用
  `?(...)`、下标 `[...]`、后缀宏 `@op`、`++/--`。
- `parseStatement()` 分派 `if/case/for/try/fn/async fn/macro/define/declare/let/const/
  import/return/throw/break/continue/export`，并处理 `x: Type = expr` 形式的带标注声明
  （内部 `NK::Decl` 的 `text` 记为 `"infer"`）。`parseProgram`/`parseBlock` 跳过裸 `;`，
  并保证每个循环至少前进一个 token，否则报 `SYN002`。
- `parseCase()` 支持 `case (expr) {…}` / `case expr {…}` / 无主语 `case {…}`；分支体
  **恰好一条语句**（`fallthrough` 算作该语句），`arm->flag2` = 落空，`arm->list` 最多一项，
  `default` 后面是否有 `:` 都能解析。
- `parseFor()` 用回退策略区分 `for x of …` / `for a b of …` / `for (cond)`：先尝试吃掉
  1~2 个标识符再看有没有 `of`，没有就 `i_ = save` 回退。`parseTry()` 接受
  `catch (e) {…}` 与 `catch e {…}`。
- `parseImport()` 是**结构化**解析（不再只拼裸文本）。按形状置
  `Node::text = sideeffect | named | namespace | default`；`spec` 是模块路径字符串，
  `names` 是导入的名字——宏名带前导 `@`（`import { @a, @b } from "./m.shya"`），
  `as` 后的别名会被吃掉但不记录；`flag` = 名字里出现过 `@`（有宏），
  `flag2` = 路径以 `.shya` 结尾。`raw` 仍按 token 逐字拼回（`isWord()` 决定要不要补空格），
  `export {…}` / `export *` 走的是原来的原样文本路径。`flag2` 的 import 由 §7 的模块
  加载器消费，`parseImport` 自己不做任何文件 IO。
  普通（非 `.shya`）import 依旧把 `raw` 透传给代码生成，所以 JS/TS 的 import 语法照旧。
  `import` 形状错误各有诊断：缺路径字符串 `SYN027`、`as` 后缺名字 `SYN028`、
  `{ }` 里或 `import` 后缺名字 `SYN029`。
- `parseDeclare()` 解析宿主/外置对象声明，产出 `NK::Declare`，两种形状：
  - **块形式** `declare Name { field: T  opt?: T  method(a: T): R }`：每个成员是一个
    `NK::Prop`，`flag` = 是否方法（后面跟 `(`），`flag2` = 是否可选字段（`?`），
    `text` = 成员名，`typeAnn` = 字段类型或方法返回类型；方法的参数用
    `names`/`typeAnns`/`defaults` 三组平行数组（`typeAnns[i]` 的 `...` 前缀表示变参），
    没有返回标注时 `typeAnn` 取 `"void"`。成员之间用 `;` 或 `,` 分隔都行。
  - **函数形式** `declare fn name(params): Ret`：`flag = true`，`text` = 函数名，
    `typeAnn2` = 返回类型（缺省 `"void"`）。
  诊断：`SYN023`（`declare fn` 缺函数名）、`SYN024`（`declare` 后既不是名字也不是 `fn`）、
  `SYN025`（写了泛型参数 `<…>`："`declare` 暂不支持泛型参数"，参数 token 被跳到 `>`）、
  `SYN026`（成员缺名字）。
  `declare` 是关键字，核心 AST 里由类型检查器和代码生成各自处理：前者收集成宿主类型，
  后者直接不产出任何代码（见 9.2、10.7）。
- 类型标注 `parseTypeAnnotation()` 只做**规格化字符串**：`bool`→`boolean`，
  `X[]`→`array<X>`（连续 `[]` 递归包裹）、尾部 `?` 原样拼上、`A|B` 拼接，泛型
  `array<map<string,number>>` 递归。它仍会把 `null`/`undefined` 写成 `void`，但这两个词
  是关键字、在词法阶段已经报 `LEX009`，所以这条别名路径不可达（`nil` 不是关键字，
  仍是干净可用的 `void` 别名）。真正的树由类型检查器用 `parseTypeString` 重新解析。
  `isTypeStart()` 的类型起始关键字白名单里同样还留着 `null`/`undefined`。

## 4. 换行即分隔符（设计上的"硬约束"）

设计稿没有语句终止符。实现选择：**换行分隔语句，`;` 可选**。由此产生三处显式的
"不跨行"规则，全部读 `Token::newlineBefore`：

1. **成员访问不跨行**：`parsePostfix()` 里 `cur().kind == Tok::Identifier &&
   !cur().newlineBefore` 才继续接成员。原因：`console log(1)` 下一行 `console log(2)`
   必须解析成两条语句，而不是 `console.log(1).console.log(2)`。
2. **后缀宏的裸 slot 实参不跨行**：`parseMacroApply()` 的 `if (!prefix)` 分支里
   `if (c.newlineBefore) break;`——下一行开始的是新语句，不是本宏的又一个参数。
   **后缀 `@macro` 本身也不跨行**：`parsePostfix()` 接后缀宏的条件是
   `cur().kind == Tok::At && !cur().newlineBefore`。少了后半个条件，
   `let a = 1` 换行后写 `@ifElse(...)` 会被解析成"把 `@ifElse` 后缀作用在 `1` 上"，
   于是下一行的宏调用凭空变成上一行的一部分。
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
`installStdlib()` 带 `stdlibInstalled_` 去重，而 `MacroExpander::expand()` 内部还会再调一次
——`compileSource` 里那次显式调用是幂等的，只是为了让 stdlib 宏在模块加载（§7）之前就位。
模块加载器用 `registerMacroFromDecl()` 注册被导入文件的宏，走的是同一条 `registerMacro`
通道，所以对 stdlib 的覆盖关系与写在同一文件里完全一致。

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
见 8）；调用点的 `{ #name: … }` → `NK::Prop`（命名插槽，按名绑定）；宏定义参数表的
`#name: type` → 名字进 `MacroDef::params`、类型进 `paramTypes`；`#name: 节点种类`
→ 名字进 `params`、种类名进 `paramTypeNames`、`NK` 集合进 `paramKinds`（见 5.7）。

`bindArguments()` 分两趟绑定（先具名、后位置补位），其校验为：缺参数时 `Stmt` 槽允许
省略（记为 `nil`，渲染为空），其它槽报
`MAC016`；多余位置实参报 `MAC017`；未知命名参数 `MAC018`；类型不符 `MAC015`。特别地，
给 `Stmt` 槽传表达式时会被自动包成 `NK::ExprStmt`（`b.slot` 改成 `Stmt`），所以"表达式
当语句用"能成功而不是报错；`_`（`isNilNode`）作为实参表示"没有目标"，绑定里
`b.nil = true`。

### 5.7 AST 节点种类作为插槽类型

`MacroDef` 除了 `paramTypes`（`SlotType`）还有两组与它平行的新字段：

```cpp
std::vector<std::string> paramTypeNames;  // 源码里怎么写就怎么记，用于诊断
std::vector<std::vector<NK>> paramKinds;  // 这个槽精确接受的 NK 种类（空 = 任意）
```

`slotTypeFromName()` 认不出的类型名（返回 `SlotType::Unknown`）才是"节点种类"路线：
`astKindsForTypeName(name)` 查 `astKindTable()`，命中就得到它接受的 `NK` 集合。
`astSlotTypeNames()` 是"值类型/槽类型名 + 全部节点种类名"的合并清单，供文档与诊断使用。
于是 `macro @swapIf(#cond: Compare, #yes: stmt)` 能要求第一个实参**必须**是比较节点。
每种节点各自的 `text`/`flag`/`list` 约定见 `docs/ast-nodes.md`，本节的表只负责
"名字 ↔ `NK`"的映射（权威定义是 `src/shya.h` 的 `NK` 枚举 + `macro.cpp` 的 `astKindTable()`）。

| 类型名 | 接受的 NK |
| --- | --- |
| `numLit` | `Num` |
| `mathLit` | `MathConst` |
| `strLit` | `Str` |
| `tplLit` | `Tpl` |
| `boolLit` | `Bool` |
| `voidLit` | `Void` |
| `ident` | `Ident` |
| `arrayLit` | `ArrayLit` |
| `objectLit` | `ObjectLit` |
| `prop` | `Prop` |
| `unary` | `Unary` |
| `binary` | `Binary` |
| `compare` | `Compare` |
| `ternary` | `Ternary` |
| `call` | `Call` |
| `member` | `Member` |
| `index` | `Index` |
| `spread` | `Spread` |
| `await` | `Await` |
| `macroApply` | `MacroApply` |
| `slotRef` | `SlotRef` |
| `tsRaw` | `TsRaw` |
| `rangeExpr` | `RangeExpr` |
| `assign` | `Assign` |
| `decl` | `Decl` |
| `incDec` | `IncDec` |
| `ifStmt` | `If` |
| `caseStmt` | `Case` |
| `caseArm` | `CaseArm` |
| `whileStmt` | `ForWhile` |
| `forOf` | `ForOf` |
| `forRange` | `ForRange` |
| `fnDecl` | `FnDecl` |
| `returnStmt` | `Return` |
| `throwStmt` | `Throw` |
| `tryStmt` | `Try` |
| `breakStmt` | `Break` |
| `continueStmt` | `Continue` |
| `importStmt` | `Import` |
| `exportStmt` | `Export` |
| `block` | `Block` |
| `declareStmt` | `Declare` |
| `exprStmt` | `ExprStmt` |

表里共 42 项，每项只映射一个 `NK`（`astKindTable()` 用 `std::vector<NK>` 是为了以后能收
多个）。**宏模板内部节点故意不可寻址**，共九个名字：`program`、`empty`、`macroDecl`、
`when`、`whenArm`、`each`、`slotList`、`optionalize`、`typeRef` ——把一个模板节点替换进
模板就是在改写模板自身，`astKindTable()` 上方的注释写明了这条理由。

表里 `rangeExpr` 一项是**不可达的**：`slotTypeFromName("rangeExpr")` 先返回
`SlotType::RangeExpr`，`@when` 的槽类型分支也先于节点分支；而且
`parseRangeOrExpr(allowRange)` 只在 `allowRange` 为真时建 `NK::RangeExpr`，那只发生在宏名
字面是 `range` 的时候（`parseMacroApply` 里 `if (n->text == "range" && prefix)` 与
`parseRangeOrExpr(n->text == "range")`），所以普通宏的 `#x: rangeExpr` 参数永远收不到
范围实参。

绑定期的判定顺序（`bindArguments`）是：先是 `Stmt` 槽的"表达式自动包 `ExprStmt`"，
再是 `paramKinds[i]` 非空时的**精确种类**比对（`SafeCallExpr` 槽额外接受 `Call{flag}`），
最后才回落到 `typeMatches(SlotType, SlotType)`。种类不符的 `MAC015` 消息形状是：

```
宏 `@<宏名>` 的参数 `#<形参名>` 需要 AST 节点 `<paramTypeNames[i]>`，但传入的是 `<nodeKindName(实参)>`
```

即期望侧用源码里写的类型名（`paramTypeNames`），实际侧用 `nodeKindName(arg->kind)` 给出的
`NK` 名（`Num`/`Str`/`Call`/`Block` …）——两边刻意用不同的词表，免得把 `strLit` 和
`string` 混为一谈。`typeMatches` 回落分支的 `MAC015` 则两侧都用 `slotTypeName()`
（`expr`/`stmt`/`callExpr` …）。`tests/cases/11-ast-types.shya` 覆盖了
`compare`/`strLit`/`block` 三种正常路径。

`evalWhen()` 也认识这些名字，所以 `@when(#x is Binary)` 在编译期就能选分支：判定顺序是
**槽类型名（`stmt`/`callExpr`/`safeCallExpr`/`rangeExpr`/`expr`）→ 值类型名 →
AST 节点种类**，且保持三值语义——`want` 不在这些表里一律 `Unknown`，`Unknown` 不选任何
分支。命中节点种类表时结果是确定的 `True`/`False`（不再是 `Unknown`）。

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

## 7. 宏文件模块加载（`src/modules.cpp`）

流水线的第 3 步，在解析之后、宏展开之前：`loadShyaModules(program, sourcePath,
includePaths, expander, bag)` 把入口文件里所有**路径以 `.shya` 结尾**的 `import`
（`Parser::parseImport` 置的 `flag2`）在编译期消费掉——它读文件、注册宏、把
`define`/`declare` 拼进导入方，**不向 JS 产出任何东西**。

### 7.1 路径解析

`Loader::resolve(spec, fromDir)` 依次尝试：

1. 绝对路径（`isAbsolute()`：以 `/`、`\` 开头，或第二个字符是 `:`）——直接看文件在不在；
2. 相对路径（`isRelative()`：以 `./ ../ .\ ..\` 开头）——拼成 `fromDir + "/" + spec`；
3. 其余（裸名字）——先试 `fromDir + "/" + spec`，再逐个试 `-I/--include` 给的
   `includePaths_`。

`fromDir` 是**导入方文件所在目录**，不是进程的工作目录：入口文件用
`dirName(sourcePath)`（`sourcePath` 为空时用 `"."`），被导入文件的嵌套 import 用
`dirName(path)`。于是 `tests/cases/12-py-macros.shya` 里的
`import { … } from "../../lib/pystd.shya"` 相对该用例文件解析，而 `lib/pystd.shya`
自己再写 `./x.shya` 时相对 `lib/` 解析。文件不存在报 `MOD001`。

### 7.2 缓存 / 循环守卫与 `normalisePath`

`normalisePath(p)` 把路径规范成缓存与循环守卫的键：统一 `\`→`/`、统一小写、**折叠
`.`/`..` 段**（绝对路径回退到根为止，相对路径保留前导 `..`）。

- `cache_` 是 `键 -> ModuleSet`：同一个文件被第二次导入时直接复用上一次的
  `macros`/`others`，不再读盘。
- `loading_` 是正在加载中的键集合，命中即报 `MOD002`（"宏文件循环导入"）。另有一条
  `depth_ > 64` 的兜底 `MOD002`（"导入层数过深（可能存在循环导入）"）。
  `tests/cases/lib/loop-a.shya` 与 `loop-b.shya` 互相导入，走的正是这条守卫。

**必须折叠 `.`/`..`**：`./a.shya` 与 `././a.shya` 折叠前是两个不同的字符串。若不折叠，
同一个文件在 `loading_` 里会被反复当成新文件插入，循环守卫永远不命中，加载器会一路
递归到路径过长或栈溢出——所以这个看似多余的规范化是循环检测能工作的前提。

### 7.3 依赖闭包与注册

被导入文件用同一套 `Lexer`/`Parser` 解析（读文件时顺带剥掉 UTF-8 BOM），文件内部的
`.shya` import 先深度优先递归加载，然后 `collectDecls()` 收集本文件的 `macro` /
`define` / `declare`（`FnDecl` 不进入这个集合）。导入方决定注册哪些宏：

- `import { @a, @b } from "./m.shya"`（`text == "named"` 且 `flag` 为真）：以写出的名字
  为种子，用 `collectMacroUses()` 求**依赖闭包**——模板体里 `@name` 用到的宏也拉进来。
  这正是 `tests/cases/12-py-macros.shya` 里只写了 `{ @assertAny }` 却也能用 `@any` 的
  原因。种子名在文件里找不到 → `MOD005`。
- `import "./m.shya"`（side-effect 形式，`names` 为空）：注册该模块**全部**宏。
- 闭包里的宏逐个交给 `expander.registerMacroFromDecl()`，并记进 `registered` 集合去重，
  所以两个文件导出同名宏时先注册者胜，不会报 `MAC001`。

### 7.4 被拼接进导入方的东西

被导入文件的 `define` 与 `declare` 节点被 `cloneShallow()` 拷贝后并入导入方程序
（`ModuleSet::others`，按加载顺序）。`cloneShallow` 只递归复制 `a..d` 与 `list`，其余
平行数组（`names`/`typeAnns`/`flags`/`defaults`/`targets`/`values`/`patterns`）是
`std::make_shared<Node>(*n)` 带来的浅拷贝共享——对这些节点来说足够，因为它们要的就是
一份能进 `checkStatements` 的等价副本。于是导入方的类型检查器能看见被导入文件声明的宿主
类型，编译期常量也随文件一起进来。被消费掉的 `import` 语句本身不进 `out`；代码生成只在
它**没被**加载器消费时才会看到 `flag2` 的 import，那会打一条 `CGN007` 内部问题警告
（见 §11）。

### 7.5 诊断码

| 码 | 触发条件 |
| --- | --- |
| `MOD001` | 找不到宏文件，或找到但读不出来 |
| `MOD002` | 循环导入（或在 `loading_` 命中 / 递归深度 > 64） |
| `MOD003` | 宏文件内部有 error：转成导入点的一条诊断，消息里带 `文件:行:列`，**每个文件最多转 6 条** |
| `MOD004` | 宏文件里 import 了非 `.shya` 的路径（`.shya` 文件只能导入 `.shya`） |
| `MOD005` | 具名导入的名字在该文件里没有对应宏 |

`MOD003` 只搬运 `Severity::Error` 的诊断（宏文件里的 warning 会静默丢掉）。

## 8. 宏展开期的启发式类型环境

`@when(#x is array)` 必须在真正的类型检查器之前判定，因此 `macro.cpp` 里有一个私有的
`StaticTyper`（只依赖 `MacroExpander::env_`，`std::unordered_map<std::string, TypePtr>`，
外加一个指向 `declaredTypes_` 的只读指针）：

- `expand` 第 3 步扫全树建 `env_`：`NK::Decl` 有标注用 `parseTypeString(typeAnn)`，否则用
  `StaticTyper::type(初值)`；`NK::FnDecl` 的参数标注（去掉 `...` 前缀）也进 `env_`。
  **同名变量只记录第一次出现的类型**。同一步还扫 `NK::Declare`，把每个声明折成
  `TK::Class`（块形式，`fields`/`methods`）或 `TK::Fn`（`declare fn`）填进
  `MacroExpander::declaredTypes_`，同名块声明同样合并成员——这份表与类型检查器的
  `declaredTypes_` 是两份独立副本，语义一致（见 §9.3）。
- `StaticTyper::type(n)` 能推：字面量、`ArrayLit`（元素类型取第 0 个）、`ObjectLit`、
  `Unary{!, typeof, -, +}`、`new Map()/new Set()/new Array()`、`&&`/`||`（两侧同型）、
  `+`（任一侧 string⇒string，两侧 number⇒number）、`Compare⇒boolean`、`Ternary`（两臂
  同型）、`Index`（array/map/string/range 的元素类型）；
  `Member` 只在接收者的静态类型是**已声明宿主类型**（`TK::Class`）时才查 `fields`/
  `methods` 并返回成员类型，其余一律 `unknown`。
- `evalWhen()` 是**三值**求值（`Tri::{True,False,Unknown}`）：先做 `SlotType` 判定
  （`#x is callExpr`/`safeCallExpr`/`rangeExpr`/`stmt`/`expr`），再做静态类型判定
  （`array/string/number/boolean/object/map/set/fn/void/rangeExpr/unknown`），最后才查
  AST 节点种类（见 5.7）。只有确定为 `True` 才算命中——`Unknown` 一律不选，所以分支选不中
  会报 `MAC020` 而不是静默产出空代码。设 `SHYA_DEBUG_WHEN` 环境变量会打印判定过程。

**已知局限**（都源自"启发式"本质）：不支持函数返回值推断、不支持分支合流后的类型、
不支持成员/调用结果的类型（一律 `unknown`）、不支持重新赋值的类型变化（`env_` 里
"先到先得"）、`any`/`unknown` 无法区分。设计上这就是"够用即可"的静态环境，真正的判定权
在 `src/typecheck.cpp`。

## 9. 类型系统与类型检查（`src/typecheck.cpp`）

### 9.1 类型格

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
  后两个名字里 `null`/`undefined`/`nil` 仍归到 `TK::Void`，但只有 `nil` 能真的写出来
  （`null`/`undefined` 是关键字，词法阶段报 `LEX009`）。`range` 与 `rangeExpr` 都给出
  `TK::Range`，与 `knownTypeName()` 收 `rangeExpr` 却不收 `range` 的老口径已经不一致
  ——`is` 右侧写 `range` 仍然只走 `TC012` 警告 + `instanceof`，不会变成范围判定。

### 9.2 作用域与检查项

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
- **`declare`**：`check()` 在 `checkStatements()` 之前先 `collectDeclares(program)`
  （见 9.3），`checkNode(NK::Declare)` 本身只返回 `tVoid()`——声明没有运行时行为。

### 9.3 `declare`：宿主类型注册与成员解析

`declare` 是"一切皆是函数"的**必要例外**：宿主给过来的是真属性还是真方法，编译器必须
分清楚，否则字段会被渲染成一次调用。

- `buildDeclaredType(NodePtr)` 把一个 `NK::Declare` 折成 `TypePtr`：块形式得到
  `tNamed(name)`（`TK::Class`）+ `Type::fields` / `Type::methods` 两张成员表；函数形式
  得到 `TK::Fn`（参数表、`required` 个数、`variadic` 由 `makeDeclaredFn()` 从
  `typeAnns`/`defaults` 算出来，返回类型取 `typeAnn2`，缺省 `tVoid()`）。
- `TypeChecker::collectDeclares()` 是**整程序的前置扫描**（递归 `a..d` / `list` /
  `values` / `defaults`），把结果放进 `declaredTypes_`。同名块声明会**合并成员**
  （逐项覆盖 `fields`/`methods`），所以宿主形状可以分几处写；`declare fn` 走
  `declare(name, t, pos)` 直接进当前作用域，于是能被当普通函数调用检查。
- `memberType(obj, member)` 在**接收者是 `TK::Class`** 时按名字查这张表。命中方法是
  `TK::Fn`：走与普通调用同一套实参检查（个数 `TC006`、类型 `TC003`，消息里的前缀是
  "方法 `recover` 的第 1 个实参"）；命中字段则**把该 `NK::Member` 节点指针记进
  `fieldAccesses_`** 并把 `result` 设为字段类型。查不到但类型确实被 `declare` 过 → 打
  `TC014` **警告**（只警告，不阻断编译）：

  ```
  类型 `Player` 没有声明成员 `missingMember`（可用 `declare Player { missingMember: … }` 补充声明）
  ```

  （末尾由渲染器补 `[TC014]`。）

  接收者不是 `TK::Class`（比如 `any`/`unknown`/未声明类型）时什么都不查，也不报
  `TC014`——这正是"宿主全局量可以放心用"的那一半渐进性。
- 这张表和 §8 里 `StaticTyper` 的那张是**两份独立副本**（`MacroExpander::declaredTypes_`
  与 `TypeChecker::declaredTypes_`），因为宏展开早于类型检查。两份都用
  `buildDeclaredType()` 构建，所以 `@when(#p hand is array)` 里 `p hand` 的类型与
  类型检查器的结论一致。
- 类型检查器现在会**下潜到 `NK::TsRaw::list`**（`@ts{…}` 里的 `#slot` 替换结果）逐个
  `checkNode`：这些节点是真的会进产物的 shya 节点，所以它们的字段访问也会被记进
  `fieldAccesses_`，`@ts{#p.name}` 这类写法里的 `p name` 一样能拿到属性语义。
  `tests/cases/14-declare.shya` 覆盖正常路径，`15-declare-errors.shya` 覆盖
  `TC003`/`TC006`/`TC014` 三条诊断。

## 10. 代码生成（`src/codegen.cpp`）

### 10.1 带优先级的渲染

表达式渲染走一对函数：

```cpp
struct Rendered { std::string text; int prec = 11; };
Rendered genExprP(const NodePtr& n);                   // 产文本 + 自身优先级
std::string genExpr(const NodePtr& n, int parentPrec); // prec < parentPrec 时补括号
```

`precedence(op)`：`|| 1`、`&& 2`、相等/`is`/`instanceof 3`、关系 `4`、加减 `5`、乘除模与
四个除法变体 `6`、`^ 8`，未知 `11`（"原子"）。语句渲染走 `line(str)`（自动缩进 + 换行）、
`genStatements`、`genStatement`、`genBlockOf`、`captureStatement`。

### 10.2 运算符映射

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
| `a is void` | `a === undefined` |
| `a is object` | `typeof a === "object" && a !== null` |
| `a is 其它名` | `a instanceof 其它名` |
| `a is not T` / `a not instanceof T` | `!(…)` / `!(a instanceof T)` |

四个除法变体的括号：左操作数按 `6`、右操作数按 `7` 渲染，所以嵌套不会串味。

`genCompare()` 里"是不是空值判定"的分支仍写着 `typeName == "void" || typeName ==
"null" || typeName == "undefined"`，但后两个名字已经写不进源码（`LEX009`），
实际只有 `a is void` 能走到，产物就是 `a === undefined`。

### 10.3 "一切皆是函数"与 `declare` 字段的例外

设计稿的"没有属性，一切皆是函数，无参括号可省略"落在两处，外加一条 `declare` 开出来的
逃生通道：

1. `NK::Member` 的常规渲染**总是发一次调用**：
   `genExpr(n->a, 11) + "." + n->text + "(" + genArguments(n->list) + ")"`，所以
   `player getHp` → `player.getHp()`，`arr slice(1,3)` → `arr.slice(1, 3)`。
   `hasSideEffects()` 也据此把 `Member` 一律视为有副作用（用于比较链的临时变量提升）。
2. **属性 vs 调用由类型检查器决定**。`compileSource()` 在生成前调用
   `codegen.setFieldAccesses(&checker.fieldAccesses())`，把类型检查器记下的"这是字段"的
   `NK::Member` 节点指针集合交给代码生成（`Codegen::fieldAccesses_` 只是一个
   `const std::unordered_set<const Node*>*`，不拥有数据）。`genExprP` 的 `NK::Member`
   分支先查这个集合：命中就只发 `genExpr(n->a, 11) + "." + n->text`（`p.name`，**不加
   括号、也不看 `n->list`**），未命中才走上面那条调用路径。所以
   `declare Player { name: string  judge(): Card }` 之下 `p name` → `p.name`、
   `p judge` → `p.judge()`（`tests/expected/14-declare.js`）。这条通道只对**解析出的**
   `NK::Member` 生效：`n->b` 非空的链片段（`#x #frag` 形式）永远走 `genFragment`。
3. 链片段 `genFragment(n, optional)` 把表达式渲染成"接在接收者后面的一段"：调用片段渲染
   成 `.name(args)` 或 `?.name(args)`；`Call.flag`（`f?(...)`）在参数表前再补一个 `?.`；
   `Member.b`（`#slot` 形式）递归成 `genFragment(n->b, optional)`；裸标识符渲染成
   `.name()`；`NK::Optionalize` 只是把 `optional` 置真。**每条链恰好插入一个 `?.`**：
   `d @safe say("hello") say("nice")` → `d?.say("hello")?.say("nice")`，
   `@safe_share` → `_q?.recover(2)`。

`NK::Binary{ text = "concat" }` 是"头表达式 + 一串链片段"的容器：`genExprP` 先 `walk`
压平，第 0 个按完整表达式渲染，其余逐个 `genFragment`。

### 10.4 链式比较

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

### 10.5 `@range` 脱糖

`MacroExpander::expandNode` 在 `NK::ForOf` 上特判 `n->a` 是 `@range` 宏调用：范围必须是
`NK::RangeExpr`（否则 `MAC010`），循环变量只能一个（否则 `MAC011`），随后生成
`NK::ForRange{ text = 变量名, a = start, b = end, c = step, d = 集合? }`。代码生成时：

- 没有 `d`（纯区间）→ `for (let i = start; i < end; i += step) {…}`；步长在编译期可确定
  为负数时比较号改 `>`（所以 `@range 3:0,-1` 会倒着走），步长非字面量时保守用 `<`；
- 有 `d`（`for v of arr @range 1:3,1`）→ 先 `const __shya_seq = Array.from(coll);`，
  再用 `__shya_i` 计数、循环体内 `const v = __shya_seq[__shya_i];`。
  变量名缺省用 `__shya_i` / `__shya_unused`。

### 10.6 `const/let` 推断

推断由 `countAssignments()` / `scanAssignments()` 驱动，配合两级栈 `scopes_`（名字是否已
声明）与 `assignCounts_`（每个名字在**当前块**里被赋值几次）：

- `scanAssignments` 递归统计：`Assign` 的每个目标 `Ident` +1；`IncDec` +1；`Decl` 本身也
  算一次绑定（`out[name]++`）——所以"声明 + 之后一次赋值"计数为 2；遇到 `NK::FnDecl`
  直接 return（函数体是自己的块，生成函数体时会重新扫）。
- `genStatement(NK::Decl)`：显式 `let` → `let`；显式 `const` 且计数 > 1 → `CGN006` 报错；
  `text == "infer"`（省略了 let/const）→ 计数 > 1 用 `let`，否则 `const`。
- `genStatement(NK::Assign)` **先查复合赋值**：`!n->text.empty() && targets.size() == 1 &&
  values.size() == 1` 时直接渲染 `目标 op= 值;`，其中 `op` 取 `n->text`，只有 `"^"` 要换成
  `"**"`（所以 `x ^= 2` → `x **= 2;`）。这条分支不参与 `const/let` 推断——复合赋值的语义
  就是"已经存在的变量"，`tests/expected/09-compound.js` 里就是 `total += step;` 这类语句。
- 否则按普通赋值处理：如果目标全是"未声明过的标识符"且左右个数相等，就整体转成
  声明（`const name = …;` / 计数 >1 时 `let`），这正是设计稿
  `player nextSeat @share _p …` 能产出 `const _p = player.nextSeat()` 的原因；否则按普通
  赋值/数组解构（`[a, b] = [x, y];`）渲染。
- 每个块（程序、`{}`、函数体、分支体、`case` 分支、循环体）进入时 `pushAssignCounts`、
  退出时 `popAssignCounts`，所以计数是按词法块局部化的。

### 10.7 其它生成细节

- 数字：`formatNumber` 处理 `NaN/Infinity/-0`，整数用 `%.0f`，其余用 `%.*g` 找最短往返
  表示；`0x/0b/0o` 字面量**原样**输出。字符串走 `escapeJsString`，模板字符串按 `names`
  （静态片段）+ `list`（`${…}` 表达式）重新拼回。
- `NK::Void` 一律输出 `undefined`（`void` 的产物就是 `undefined`；`null` 已经不在语言里，
  见 §2），`_` 输出空串。
- 对象字面量渲染成 `{ k: v }`，方法简写（`Prop` 的值是 `FnDecl`）渲染成
  `k(params) { … }`，非法标识符键自动加引号。
- 无主语的 `case` 生成 `if / else if / else` 链；有主语的生成 `switch`，非 `fallthrough`
  分支统一补 `break;`，`fallthrough` 分支输出 `// fallthrough` 注释。
- `tsRaw` 节点在语句位置按行重新缩进输出；`import`/`export {…}` 直接透传 `raw`；
  `export default` 与 `export fn` 用 `pendingExport_` 前缀机制。**`flag2` 的 import
  （`.shya` 宏导入）不该走到这里**——它已在 §7 被加载器消费掉；真走到这里说明加载器漏了，
  代码生成会打 `CGN007` 警告并**静默丢弃**这条 import（不会把 `.shya` 路径写进 JS）。
- `NK::MacroDecl` / `NK::Define` / `NK::Declare` 在 `genStatement` 里共用一个
  `case`，直接 `return`：三者都是编译期产物，`declare` 因此**不产出任何 JS**
  （`tests/expected/14-declare.js` 里看不到它）。它们不会掉进 `default` 分支的
  `genExpr` → `CGN004`。
- 顶部写死两行 banner（`// Generated by the shya compiler from <file>.` 与
  `// Target: ES2026. Do not edit by hand.`），最后统一折叠多余空行。

## 11. 诊断与退出码

`DiagBag`（`src/shya.h`）收集 `Diagnostic { severity, pos, code, message }`，提供
`error/warning/note/hasError/errorCount/warningCount/items/append/clear`。各阶段共用同一个
`bag`，所以一次编译能看到跨阶段的全部诊断。**诊断码按阶段分族**（号段连续）：

- `LEX0xx` 词法：`LEX001` 数值、`LEX002` 未知字符、`LEX003` 块注释未闭合、`LEX004`
  字符串/模板未闭合、`LEX005~007` 数学字面量、`LEX008` `@ts` 未闭合、`LEX009`
  写了已移除的 `null`/`undefined`（错误，不是警告；见 §2）；
- `SYN0xx` 语法：`SYN001` 期望 token、`SYN002` 无法解析并跳过、`SYN003~008` 声明类、
  `SYN009` `async` 位置、`SYN010` `export`、`SYN011/012` `#` 使用位置、`SYN013` 对象键、
  `SYN014` `@ts` 后缺 `{`、`SYN015` 表达式、`SYN016/017` `@each`、`SYN018` `@when` 分支、
  `SYN019` 缺宏名、`SYN020` 命名插槽、`SYN021` 类型标注、`SYN023` `declare fn` 缺函数名、
  `SYN024` `declare` 后既不是名字也不是 `fn`、`SYN025` `declare` 不支持泛型参数、
  `SYN026` `declare` 成员缺名字、`SYN027` `import` 缺模块路径、`SYN028` `import … as`
  后缺名字、`SYN029` `import` 里缺名字（**没有 `SYN022`**，号段跳过）；
- `MAC0xx` 宏：`MAC001` 重名、`MAC002` 内建库解析失败、`MAC003~007` `@ts`/`@when`/`@each`、
  `MAC008~018` 绑定与展开、`MAC019` `define` 非常量、`MAC020` `@when` 无匹配分支。
  `MAC015` 现在承载两种消息：槽类型不符，以及**AST 节点种类不符**（见 5.7）；
- `MOD0xx` 宏文件模块（唯一由 `src/modules.cpp` 产生的族）：`MOD001` 找不到/读不出宏文件、
  `MOD002` 循环导入或层数过深、`MOD003` 宏文件内部有 error、`MOD004` 宏文件里导入了非
  `.shya`、`MOD005` 具名导入的宏不存在（见 §7.5）；
- `TC0xx` 类型：`TC001` 重复声明、`TC002` 标注解析、`TC003` 赋值不兼容、`TC004` 字符串
  拼接、`TC005` 比较类型不同、`TC006` 实参个数、`TC007` `const` 无初值、`TC008/009` 阶段
  残留、`TC010` 未声明标识符、`TC011/012` `is` 右侧、`TC013` const 重赋值、`TC014`
  `declare` 过的类型上没有这个成员（警告，带补充声明的建议）；
- `CGN0xx` 代码生成：`CGN001` `@ts` 占位损坏、`CGN002` 可选调用被忽略、`CGN003` 范围
  表达式位置错误、`CGN004` 无法处理的节点、`CGN005` `is` 右侧、`CGN006` `const` 重赋值、
  `CGN007` `.shya` 宏导入漏到了代码生成（警告，该 import 被丢弃；见 §7.4）。

`renderDiagnostics(bag, source, filename)`（`src/diag.cpp`）输出
`file:line:col: severity: message [CODE]`，再打印源码行与插入符：`sourceLine()` 取该行，
`utf8WidthAt()` 按 UTF-8 前导字节算出**终端显示宽度**（CJK/全角区段算 2 列），从而让 `^`
对齐中文标识符。

退出码（`main()`）：用法/未知命令/未知选项/读不到文件/写不出文件 → `1`；`help`、`version`、
成功 → `0`；`build`/`check` 有 error → `1`（并打印 `shya: <cmd> failed with N error(s)`）；
`run` 返回子进程 `node` 的退出码。`tokens/ast/core` 子命令只转储、不写文件。
