# shya 设计决策记录

记录设计稿（`docs/design-draft.txt`，即 I0002 设计稿的纯文本副本）中**有歧义、自相矛盾或完全没写**的地方，以及实现
（`src/`、`tests/`）实际采取的做法。每条都尽量给出可核对的依据（文件、诊断码、
`tests/expected/` 下的黄金输出）。分类标记：

- **忠实**：设计稿写清楚了，实现照做；
- **补齐**：设计稿没写或只写了一半，实现自行决定；
- **修正**：设计稿自相矛盾或有错，实现选了一种解释；
- **未实现**：设计稿提到但实现里没有。

## A. 语法与"省略"的边界

### A1. `if` / `elif` 的括号（**补齐 / 忠实**）

设计稿只写 `if(exr1){ … }elif(expr2){ … }else{ … }`，示例里全带括号，但"未做说明的和
js 保持一致"又暗示可选。实现两条都收：`Parser::parseParenExpr()` 是"有 `(` 就吃掉、
没有就直接解析表达式"，所以 `if(1 < 2){}` 与 `if 1 < 2 {}` 等价
（`tests/cases/01-core.shya` 两种都用了）。`case`、`for`、`try/catch` 同理：
`case (expr){}`、`case expr{}`、无主语的 `case{}` 三种都支持；`catch (e){}` 与
`catch e{}` 都支持。

### A2. 后缀宏的裸参数只能是"调用片段"（**补齐**）

设计稿的例子 `player nextSeat @share _p recover(2) draw(2)` 没写清楚
`recover(2)` 与 `draw(2)` 的边界。实现规定：无括号的后缀宏按 `parseSlotFragment()`
收参数，每个参数是"一个 `parsePrimary` + 可选的 `(...)` / `?(...)` / `[...]` 尾缀"，
**不接成员链**。于是上面那行是**四个实参**：
`Member(player,nextSeat)`、`Ident(_p)`、`Call(recover)`、`Call(draw)`。
更复杂的表达式必须写括号形式
`@macro(a + b)`。只有靠"逐个片段"这一条，`@safe` / `@share` 这类链式宏才可能成立。

### A3. `@safe` / `@safe_share` 到底插入几个 `?.`（**修正**）

设计稿第 140–142 行给了 `d @safe say("hello") say("nice")` →
`d?.say("hello")?.say("nice")`（每个链段恰好一个 `?.`），但第 162–166 行又写
`_p.?recover?.(2)` 与 `_p?.draw?.(2)` —— 同一份文档里两种写法，且 `_p.?recover?.(2)`
是**非法 JS**（`?.` 之后不能跟 `.`）。实现选择统一：**每条链恰好一个 `?.`**，
`Codegen::genFragment()` 里 `optional ? "?." : "."` 只发一次，`Call::flag`（`f?(…)`）
再多一个 `?.`。所以 `tests/expected/02-macros.js` 里是：

```js
d?.say("hello")?.say("nice");
_q?.recover(2);
_q?.draw(2);
```

`src/stdlib.cpp` 的 `kStdlibSource` 就是把设计稿的宏文本抄进来，去掉 `@when` 分支里
多余的 `,` 分隔符；文件头注释明确写了这两处归一化。

### A4. `~degN` 的方向（**补齐**）

设计稿只列了 `~deg2` / `~deg~e`，没说结果含义。实现把它定义为
"**N 度转成弧度**"（`x * π / 180`），并额外提供反向的 `~radN`（弧度→角度）。
依据：`tests/expected/01-core.js` 里 `~deg2` 生成 `0.03490658503988659`（= 2° 的弧度），
`~ln~deg360` 生成 `1.8378770664093453`（= ln(2π)）。常量表里额外还有 `~tau` 与 `~inf`，
设计稿未提。

### A5. 换行当语句分隔符、`;` 可选且兼任空语句（**补齐**）

设计稿"未做说明的和 js 保持一致"，从未规定语句终止符。实现的选择是**换行分隔语句**
（`Token::newlineBefore`），`;` 可有可无；`;` 本身又是一个空语句（`NK::Empty`），
所以设计稿里的 `1,2:;` 能解析成"空分支体"。由此带来三处"不跨行"的硬规则：成员访问、
后缀宏的裸参数、`@each`/`@when` 拼接都不跨行；否则 `console log(1)` 换行
`console log(2)` 会被并成一条链。

### A6. `case` 分支体恰好一条语句（**修正**）

设计稿的例子是 `1,2:;` / `5:fallthrough`，看着像"多条语句也行"。实现规定分支体
**恰好一条语句**，`fallthrough` 关键字算作那条语句，想要多条就用 `{ }` 包成块
（`tests/expected/01-core.js` 里 `case "other": // fallthrough` 就是这个形状）。
分支默认 `break`，`fallthrough` 才落空。

### A7. `let`/`const` 可省与 const 重赋值（**忠实 + 补齐**）

设计稿"let 和 const 可省，由最终编译器推断，可变为 let，不可变为 const"是明确的，
但没定义"不可变"的判定标准。实现用**静态赋值计数**：名字在整个词法块里只被绑定一次
→ `const`，否则 `let`（`Codegen::countAssignments`/`scanAssignments`，`assignCounts_`
按块入栈）。显式 `const` 又被重新赋值 → 报 `CGN006`，类型检查阶段更早报 `TC013`
（`tests/expected/06-typecheck.err`）。设计稿没写 `const` 必须有初值，实现要求必须
（`TC007`）。

## B. 宏与编译期常量

### B1. `define` 的合法初始值（**补齐**）

设计稿只给了 `define a = 100`。实现要求初始值必须是**编译期常量**：字面量（数字 /
字符串 / 布尔 / `void`）、数学字面量、其它 `define`、以及它们的常量算术，否则报
`MAC019`。`foldConstant()` 支持的运算与运行时一致（含 `~/ +/ -/ \/` 四种除法，其中
`~/` 按"向零截断"折叠）。展开后 `define` 变量从产物中消失（`tests/expected/01-core.js`
里没有 `a`/`half`）。

### B2. `_` 占位符的两种身份（**补齐**）

设计稿说 `_` 既是"宏里表示无 target"又是"待完成（等于 Python 的 pass）"。实现按位置
区分：单独成句 → `NK::Empty`（不生成任何代码）；作为宏实参 → `isNilNode()` 为真、
绑定时 `b.nil = true`、`makeConcat` 时整个片段被丢掉（`tests/expected/02-macros.js` 中
`_ @banner { #text: … }` 只产出 `console.log("---")` 与那个块）。
**注意**：`_` 只在"拼接"路径上被干净地丢弃。如果模板里把 `#slot` 直接当接收者用
（例如 `#target judge` 而调用处传 `_`），`genFragment`/`genExprP` 会产出一个空基址，
得到非法的 `.judge()`（本地用 `_ @judge_color { … }` 实测可复现）。设计稿的
"没有target时，需写为_"只保证了宏匹配，没保证生成的 JS 合法。

### B3. `@ts{...}` 的嵌套与内容（**修正**）

设计稿说"`@ts {}` 只能嵌套 `@ts`"。实现走更宽松也更简单的路：`lexTsBlock()` 把 `{…}`
之间的内容**逐字**收下（跳过字符串与注释、做花括号配平），除 `#slot` 占位替换外完全
不解析；因此 `@ts` 里什么 JS 都能写，也可以再出现 `@ts`（不会被识别）。
替换机制见架构文档 5.5（`\x01<index>\x01` 标记 + `TsRaw::list`，
在 `Codegen::genTsRaw` 里回填）。

### B4. 范围字面量只能出现在 `@range`（**忠实**）

设计稿"范围表达式 `start:end,step` … 只用于宏 range 遍历"。实现只在
`parseRangeOrExpr(allowRange)` 打开时才解析范围（`@range` 参数位置），其它位置的 `:`
是三元运算符；`NK::RangeExpr` 若漏到代码生成阶段报 `CGN003`。

### B5. `@when` 没有匹配分支（**补齐**）

设计稿没写这种情况怎么办。实现**报错** `MAC020`（"@when 分支一个都没有匹配：无法静态
确定参数类型"），而不是静默产出空代码。`tests/expected/03-macro-errors.err` 里
`opaque @len`（`opaque` 来自 `@ts`，静态类型未知）就是这个错误。
### B6. 参数类型与 slot 类型的匹配规则（**补齐**）

设计稿说"变量需标注语句类型或表达式类型 … 否则默认为 expr"，并给了
`stmt, expr, type` 和 `...#slots:callExpr`。实现把类型收敛到 `SlotType`
(`Expr/Stmt/Type/ExprList/CallExpr/SafeCallExpr/RangeExpr/Unknown`)，`typeMatches()`
的实际规则：`Expr`/`Unknown` 接受一切；`CallExpr` 与 `SafeCallExpr` 互相接受（还接受
`Expr`）；`Stmt` 只接受 `Stmt`，但**把表达式当 stmt 用会自动包成 `NK::ExprStmt`** 而
不报错，且 `Stmt` 槽允许整个缺省；`Type` 也接受 `Expr`。`@when(#x is callExpr)` 依据
`inferSlotType()`，`@when(#x is array)` 依据启发式静态类型（见 D）。

### B7. 命名插槽与位置参数的混用（**补齐**）

设计稿给了 `player @judge_color { #red: … }` 与 `player @judge_color(…)` 两种等价调用。
实现允许**混用**，绑定规则是两趟：

1. **具名优先**：`bindArguments` 先把 `NK::Prop`（`{ #name: … }` 里的项）收进 `named`
   映射，按名字填掉对应参数；
2. **位置补位**：剩下的位置参数（后缀 target 排第一，然后是后缀裸片段）按**声明顺序**
   填进"没被具名占掉"的参数；变参槽吃掉其余全部；`stmt` 槽允许整个缺省。

所以 `p2 @emit { #a: … }` 中 `p2` 会落到第一个没被具名的参数上（`tests/cases/02-macros.shya`
覆盖），而不是报错。所有参数都被具名占满还有多余位置参数 → `MAC017`，未知命名参数 →
`MAC018`，缺参 → `MAC016`。设计稿"若插槽均为表达式或全为不定项，可省 `{}`"
没有实现成通则——括号内**不**支持 `#name: value`（`#` 在宏调用点不是合法 token），
命名参数只能走 `{ #name: … }`；同理 `{ … }` 只有在真的以 `#名字:` 开头时才算插槽块
（`Parser::isNamedSlotBlock()`），否则那个 `{` 是 `for` / `if` 的语句体。

### B8. `_` 作为 target 与具名插槽（**补齐**）

设计稿"没有 target 时，需写为 `_`"。实现把 `_` 绑成"空节点"（`Binding::nil`），
`makeConcat` 会把空片段丢掉，所以 `_ @banner { #text: … }` 能正常工作
（`tests/cases/02-macros.shya`）。但要注意 `_` **只保证宏调用能匹配**：如果模板里把
这个空插槽当接收者用（如 `#target judge`），生成的就是非法的 `.judge()`。
设计稿没有说明这一点，实现也不做特殊处理——需要可选语义时请用 `@safe` / `@safe_share`。

## C. 类型系统

### C1. `object` 字面量不是 `map`（**修正**）

设计稿第 131–134 行要求 `array @len → .length`、`map @len → .size`，但 `{ x: 1 }`
这种字面量在实现里是 `TK::Object`（**不是** map），`@len` 的两个 `@when` 都不匹配，
于是报 `MAC020`；`Map`/`Set` 必须显式 `new Map()` / `new Set()`——`StaticTyper` 认识
`new Map/Set/Array` 这种字面形式。`tests/expected/01-core.js` 里
`m.size, st.size, label.length` 全部来自显式构造的 `new Map()`/`new Set()`。

### C2. 渐进类型的具体边界（**补齐**）

设计稿说"类型标注和 ts 规则一致，但只有基本的类型系统，无法自定义类"。实现的具体边界：
`any`/`unknown` 让所有检查静默通过（`isAssignable` 双向放行）；成员访问与调用结果是
`unknown`；`Class→Class` 赋值一律不通过（不做名义子类型）；**未声明的标识符只是警告**
（`TC010`），比较两侧类型不同只是警告（`TC005`），`is` 右侧用非内建类型名只是警告
（`TC012`，按宿主 `instanceof` 处理）。真正报错的是：初始化/赋值不兼容（`TC003`）、
调用实参个数（`TC006`）、运算符操作数（`TC003`）、`const` 重赋值（`TC013`）、
`const` 无初值（`TC007`）、`is` 右侧不是标识符（`TC011`）。宿主类型（如设计稿里的
`Player`）通过 `tNamed` + `TK::Class` 参与检查，但只按名字比较，没有任何结构信息。

### C3. `is` 右侧的类型名与关键字冲突（**修正**）

`knownTypeName()` 收 `array/string/number/boolean/object/map/set/fn/void/null/undefined/
any/unknown/never/range/rangeExpr/expr/stmt/type/callExpr/safeCallExpr` 等名字，类型检查器
要求右侧节点是 `NK::Ident`。但 `void`/`null`/`undefined`/`fn`/`number` 等词法上是
**关键字**：`parsePrimary` 会把 `void` 解析成 `NK::Void`、把 `fn` 当成函数声明的开头，
走普通表达式规则会解析失败或误报 `TC011`（"`is` 的右侧必须是类型名"）。
实现因此让 `is` / `is not` 的右侧单独走 `Parser::parseTypeNameRHS()`：标识符与类型名
白名单里的关键字都收成 `NK::Ident`，其余情况回落到普通关系表达式。
于是 `x is void` / `x is null` / `x is fn` / `x is number` 都正常
（`tests/cases/08-doc-check.shya` 覆盖了其中三个）。设计稿的"undefined 与 null 统一为
void"因此在**标注**（`parseTypeString` 归到 `TK::Void`）与 **`is` 表达式**
（编译成 `x === undefined`）里都成立。

**两种词序都支持**：`parseEquality()` 用一个 `readCompareOp()` lambda 统一识别比较运算符，
`is not`（`u is not number`）与设计稿里出现过另一词序 `not is` / `not instanceof`
都编译成取反形式（`!(typeof u === "number")`），
覆盖率见 `tests/cases/10-typeops.shya`。
`parseTypeNameRHS()` 的白名单是 `fn void null undefined number string boolean object`；
`array`/`map`/`set` 等不是关键字，走普通标识符分支即可。

### C4. 函数语法（**补齐**）

设计稿完全没有定义函数声明。实现引入关键字 `fn`：
`fn name(a: T, b: T = 默认值, ...rest: T[]): Ret { … }`，`async fn` 支持异步，
`fn` 也能当匿名函数表达式；对象字面量里有方法简写 `key(params) { … }`。
默认值用 `parseTernary()` 解析（不带逗号表达式），因此
**默认参数不要求放在最后**：`fn f(a: number = 1, b: number)` 合法
（`parseFnDecl()` 用 `flag` 记 `async`、`flag2` 记 `export`；参数表用 `names`/`typeAnns`/
`defaults` 三组平行数组；`typeAnns[i]` 以 `...` 前缀表示变参）。

## D. 宏展开期的启发式类型环境

设计稿要求 `@when(#x is array)` 这样的分支在**宏展开期**就选好，但类型检查器在那之后才
跑。实现因此维护一个私有 `StaticTyper` + `MacroExpander::env_`：扫一遍声明与函数参数
标注建表，再对宏实参做保守推断。判定是三值的（`Tri::{True,False,Unknown}`）：只有确定
为真才算命中，未知一律不选（于是落到 B5 的 `MAC020`）。

`StaticTyper` 只认：字面量、数组/对象字面量、`new Map/Set/Array`、一元运算、
`&&`/`||`（两侧同型）、`+`、比较（→boolean）、三元（两臂同型）、下标取值；`env_` 里
同名变量**只记录第一次出现的类型**（不做重赋值导致的类型变化），不推断函数返回值，
不支持分支合流，有标注时直接信标注。`SHYA_DEBUG_WHEN=1` 会打印每次判定的
`want / staticType / 结果`，方便排查"为什么 `@when` 没选中"。

## E. 明确"没写但也不做"的东西

### E1. 模块与类（**忠实**）

设计稿"导入：规则与 ts 一致"、"无法自定义类，但可以将标注类以对接游戏系统"。
实现把 `import …` 与 `export {…}` / `export *` 的 token 文本原样拼回 `Node::raw` 再透传
到输出，等于**不做任何解析**（`export default expr`、`export fn/const` 走结构化路径），
所以 shya 没有模块系统、没有导入解析、没有跨文件类型共享。`class` 不是关键字，
`tNamed()` 出来的 `TK::Class` 只是名字占位，无字段、无方法、无继承，`Class→Class`
赋值直接判 false。

### E3. 未实现的、设计稿提到或容易被误认为存在的东西

以下几条在实现里**不存在**（我 grep 过源码，且这些语法会在解析阶段报 `SYN0xx`）：

1. **位运算**。设计稿"位运算：没有位运算"是明确不做——`& | ^ << >> >>>` 都不作为
   位运算符：`^` 被复用于乘方（编译成 `**`），`|` 只用于类型联合，`&` 词法可识别但
   没有任何语法使用它。
2. **`for(init;cond;step)` 三段式循环头**。设计稿的循环示例写的是
   `for(let x = 1; x<100; x+=2)`，但设计稿同时明确"**只有 `for(condition)` 循环**、
   只有 `for of` 遍历"，所以实现不支持三段式循环头；等价写法是
   `for x of @range 1:100,2`。复合赋值本身是支持的
   （`A7` 之外见 `tests/cases/09-compound.shya`）：`x += 1`、`x -= 1`、`x *= 2`、
   `x /= 2`、`x %= 2`、`x ^= 2`（产物 `x **= 2`）都能写，但和 `++`/`--` 一样
   **只能作为语句**，不构成表达式。
3. **命名参数在括号内** `@macro(#a: …)`。设计稿的"参数具名化"只在 `{ #name: … }`
   命名插槽块里实现；括号里写 `#name:` 会报 `SYN011`/`SYN001`。
4. **`@when()` 省略条件**。设计稿"只填第一个的话，可不具名：`@when(){}`"——无名分支
   确实支持（条件为真取第一个分支），但**条件不能省**：`@when()` 被解析成恒真
   （`parseTemplateWhen` 直接塞一个 `Bool(true)`），省略不会被当成错误。

### E4. 设计稿之外新增的语法（**补齐**）

下面这些设计稿里没有，是实现加上去的：三元 `?:`、数组 `[]` 与对象 `{}` 字面量
（含方法简写与简写属性）、下标访问 `a[i]`、复合赋值
（`+= -= *= /= %= ^=`，其中 `^=` 产物是 `**=`）、`try / catch / finally`、`throw`、
`return`、`await`、展开 `...`、带 `${}` 的模板字符串（`${}` 内按 shya 表达式解析，见
`parsePrimary` 的 `NK::Tpl` 分支）、`import`/`export` 透传、数字分隔符 `1_000`、
`0x`/`0b`/`0o` 字面量、`typeof`/`new`/`instanceof`/`not instanceof`、行注释与
**可嵌套的块注释**（`LEX003`）、`x: Type = expr` 形式的带标注声明、`~tau`/`~inf` 与
`~radN`（设计稿只提了 `~degN`）。

## F. 设计稿内部的其余含糊之处

- **"一切皆是函数，无参括号可省略"**：实现把它做成"成员访问永远发一次调用"——
  `player getHp` → `player.getHp()`，`arr slice(1,3)` → `arr.slice(1, 3)`。
  因此**没有属性读取**，想取属性只能走 `@ts` 或 `a[i]`；`hasSideEffects()` 据此把
  `Member` 一律视为有副作用。
- **`@keys` / `@values` / `@entries` 的语义**：实现照抄成 `Object.keys/values/entries`。
  设计稿同时说"你能像操作数组一样操作所有迭代器，最终仍为原始类型"——实现没有迭代器协议
  （产物里就是普通数组，`for … of` 是 JS 的 `for…of`），所以这句话只有"结果是数组"
  这一半成立。
- **`for player of players @range 0:3,2`**：实现支持集合 + 区间（`for v of arr @range
  1:3,1` 生成 `Array.from` + 计数循环），但**区间不是切片**：它按下标取值，`start`/`end`
  与 `@range` 一致（含首不含尾）。
- **`case x { 1,2:; }`**：设计稿的 `;` 语义不明。实现里 `;` 是空语句，即"两个 `case` 标签 +
  空分支体"，代码生成会给该分支补 `break`（**除非**写了 `fallthrough`），所以它是
  `case 1: case 2: break;`——**不会**贯穿到下一个分支。设计稿若想表达"贯穿"，需要写
  `fallthrough`（本地实测：`case x {1,2:; 3:…}` 传入 `x = 1` 时直接跳出 `switch`）。
- **`@when(){ #y: #n: }` 里的 `#`**：实现同时接受 `#y:` 与 `y:` 两种分支名
  （`parseTemplateWhen` 里 `if (cur().kind == Tok::Hash) take();`）；设计稿两种写法都
  出现过，这属于兼容而非选择。
- **"参数表达式 `func(expr,expr)` / 安全访问 `func?(expr,expr)` 且参数具名化"**：
  前半段忠实实现（`f?(a, b)` → `f?.(a, b)`），"参数具名化"没有对应实现（见 E3.3）。
- **`@each(#slot of #slots)`**：设计稿只给了这一个形状；实现要求 `#slots` 是变参插槽
  （`bindArguments` 把它打包成 `ArrayLit(flag=true)`），非变参绑定会被当成单元素列表
  处理（不报错）。