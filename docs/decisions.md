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
`any`/`unknown` 让所有检查静默通过（`isAssignable` 双向放行）；成员访问与调用结果**默认**
是 `unknown`；`Class→Class` 赋值一律不通过（不做名义子类型）；**未声明的标识符只是警告**
（`TC010`），比较两侧类型不同只是警告（`TC005`），`is` 右侧用非内建类型名只是警告
（`TC012`，按宿主 `instanceof` 处理）。真正报错的是：初始化/赋值不兼容（`TC003`）、
调用实参个数（`TC006`）、运算符操作数（`TC003`）、`const` 重赋值（`TC013`）、
`const` 无初值（`TC007`）、`is` 右侧不是标识符（`TC011`）。宿主类型（如设计稿里的
`Player`）通过 `tNamed` + `TK::Class` 参与检查，**没有 `declare` 时**只按名字比较、
没有任何结构信息；写了 `declare` 之后成员才有类型、未知成员才有 `TC014` 警告
（`TC014` 仍是警告，不改变这一节的"渐进"结论），但赋值规则不变——声明过的两个不同类型
之间依旧不可赋值。

### C3. `is` 右侧的类型名与关键字冲突（**修正**）

`knownTypeName()` 收 `array/string/number/boolean/object/map/set/fn/void/null/undefined/
any/unknown/never/range/rangeExpr/expr/stmt/type/callExpr/safeCallExpr` 等名字，类型检查器
要求右侧节点是 `NK::Ident`。但 `void`/`null`/`undefined`/`fn`/`number` 等词法上是
**关键字**：`parsePrimary` 会把 `void` 解析成 `NK::Void`、把 `fn` 当成函数声明的开头，
走普通表达式规则会解析失败或误报 `TC011`（"`is` 的右侧必须是类型名"）。
实现因此让 `is` / `is not` 的右侧单独走 `Parser::parseTypeNameRHS()`：标识符与类型名
白名单里的关键字都收成 `NK::Ident`，其余情况回落到普通关系表达式。
于是 `x is void` / `x is fn` / `x is number` 都正常（`tests/cases/08-doc-check.shya`
覆盖了其中三个；`10-typeops.shya` 覆盖 `x is void` / `x is not void`）。
**`x is null` / `x is undefined` 已经写不出来了**：`parseTypeNameRHS()` 的白名单里仍留着
`null`/`undefined`，但这两个词在词法阶段就报 `LEX009`（见 G1），所以白名单里实际可用的
只剩 `fn void number string boolean object` 六个。设计稿的"undefined 与 null 统一为 void"
如今只剩**输出侧**成立：`void` 生成 `undefined`，`is void` 生成 `x === undefined`。

**两种词序都支持**：`parseEquality()` 用一个 `readCompareOp()` lambda 统一识别比较运算符，
`is not`（`u is not number`）与设计稿里出现过另一词序 `not is` / `not instanceof`
都编译成取反形式（`!(typeof u === "number")`），
覆盖率见 `tests/cases/10-typeops.shya`。
`readCompareOp()` 认 `== != === !==`、`is [not]`、`instanceof`、`not instanceof`、
`not is`，返回要消费的 token 数（两词形式返回 2）；`is` / `is not` 的右侧用
`parseTypeNameRHS()`，其它比较的右侧用 `parseRelational()`。

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
`import …` 与 `export {…}` / `export *` 的 token 文本被原样拼回 `Node::raw` 透传到输出
（`export default expr`、`export fn/const` 走结构化路径）。

这条在 v2 里被**部分推翻**：现在有 `.shya` 宏文件这一层编译期模块（见 G3），但它不是
命名空间——宏名进入同一个全局表，被导入文件的 `define`/`declare` 直接拼进导入方，
`.shya` 文件里也不能导入 `.js`。所以"没有真正的模块系统、没有跨文件类型隔离"依然成立，
"没有导入解析"已经不成立。

类仍然不能自定义：`class` 不是关键字，`tNamed()` 出来的 `TK::Class` 在语言层面只是名字
占位，没有继承、没有构造、不能 `new` 自己的类型；`Class→Class` 赋值一律 false
（`isAssignable` 里那条 `to->kind == TK::Class && from->kind == TK::Class → false`）。
唯一给 `TK::Class` 装上结构的是 `declare`（见 G2）——那是宿主形状的**声明**，不是定义，
而且只用于成员解析与成员检查，不改赋值规则。

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
  因此**默认没有属性读取**，想取属性只能走 `@ts`、`a[i]`，或先用 `declare` 把宿主字段
  声明出来（G2：只有 `declare` 过的**字段**才会渲染成 `p.name`）；`hasSideEffects()` 据此
  把 `Member` 一律视为有副作用。
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

## G. v2 增补：`declare`、宏文件、AST 节点类型

### G1. 删减 `null` / `undefined`（**修正**）

设计稿原文的"统一化为 void"在 v1 里被实现成"两个词都保留、处处等价于 `void`"，于是
`let a = null`、`let x: undefined`、`x is null` 全都合法。v2 收紧了这一条：**语言里只留
`void`**，写 `null`/`undefined` 直接在词法阶段报错：

```
let x = null
        ^
shya 只有 `void`：`null` 已从语言中移除，请改写为 `void` [LEX009]
```

- `lexer.cpp` 的 `keywords()` 里**仍然留着** `null`/`undefined`，命中后照旧产生
  `Tok::Keyword`，然后立刻 `bag.error(start, "LEX009", …)`。保留词法识别是为了让解析器
  把该位置继续读成 `NK::Void`（表达式位置，`parsePrimary`）或一个类型名
  （标注位置、`parseTypeNameRHS()` 白名单），从而**在同一个文件里继续报出后面的错误**
  而不是被一个未知 token 卡住。但 `compileSource()` 在第 1 步之后就因 error 返回，
  所以"解析器接受 `null`"这件事在真实编译里不可达——它是恢复用的死代码。
- **`@ts{…}` 内不受限**：`@ts{null}`、`@ts{ x === null }` 都是合法载荷，因为 `@ts` 走
  `lexTsBlock()` 整段透传，`Tok::TsBlock` 的 `raw` 不经过关键字分类。本地实测
  `console log(@ts{null}, y)` 输出 `null undefined`。
- **标注层面的别名也失效**：`Parser::parseTypeAnnotation()` 里那行
  `if (name == "null" || name == "undefined") name = "void";` 和
  `parseTypeString()` 里 `void/null/undefined/nil` 归 `TK::Void` 的分支都还在，但
  `let y: undefined` 会在词法阶段先报 `LEX009`（本地实测），所以那个别名路径同样不可达。
  `nil` 不是关键字，是**唯一**还能用的 `void` 别名。
- 输出侧没有变：`void` 依旧生成 `undefined`（`Codegen` 的 `NK::Void` 分支与
  `genCompare` 的 `is void` 分支都没改），设计稿"最终都落到 undefined"这半边仍然成立。
- 依据：`src/lexer.cpp`（`LEX009`）、`src/parser.cpp`（两个残留分支）、
  `tests/cases/10-typeops.shya`（`x is void` / `x is not void`）。

### G2. `declare` 的语义边界（**补齐**）

设计稿只说"无法自定义类，但可以将标注类以对接游戏系统"，至于是**标注**一个类型名还是
**声明**这个类型有什么成员，完全没写。v1 选了"只按名字比较"（`tNamed` + `TK::Class`，
无结构），于是宿主给过来的字段一律被当成方法调用——"一切皆是函数"这条规则在宿主对象
上直接失效。v2 引入 `declare`（`NK::Declare`）作为这条规则的**必要例外**：

- **字段与方法的区分会改变代码生成**。`TypeChecker::memberType()` 查
  `Type::fields` / `Type::methods`：命中**字段**时把这个 `NK::Member` 节点的指针记进
  `fieldAccesses_`，`compileSource()` 再把它交给
  `Codegen::setFieldAccesses(&checker.fieldAccesses())`；`genExprP` 的 `NK::Member` 分支
  先查这个集合，命中就发 `p.name`（属性读取），未命中才发 `p.name(...)`（调用）。所以
  `declare Player { name: string  judge(): Card }` 之下 `p name` → `p.name`、
  `p judge` → `p.judge()`（`tests/expected/14-declare.js` 实证）。
- **默认值只描述宿主契约，不注入调用点**。`declare fn draw(n: number = 1): void` 里的
  `= 1` 只用于算 `Type::required`（决定实参个数下界，`TC006`），编译产物里
  `p.draw()` **不会**被补成 `p.draw(1)`——默认值属于宿主实现。
- **`declare` 不产出任何 JS**。`Codegen::genStatement` 里 `MacroDecl`/`Define`/`Declare`
  共用一个直接 `return` 的 case，`tests/expected/14-declare.js` 里完全看不到 `declare`。
  类型检查侧 `checkNode(NK::Declare)` 也直接返回 `tVoid()`。
- **同名 `declare` 合并成员**：`TypeChecker::collectDeclares` 遇到已存在的 `TK::Class`
  就把新声明的 `fields`/`methods` 逐项并入（`MacroExpander` 里那份副本同样处理），
  于是宿主形状可以分几处写，不会报重复声明。
- **不做结构化匹配（名义类型）**：`Class→Class` 赋值依然是 false（`isAssignable()` 里
  那条直接 return false 的分支没动）。`declare Player { hp: number }` 与
  `declare Enemy { hp: number }` 形状相同但**不互相兼容**。`declare` 只让"成员查得到"
  和"成员有类型"，不让"形状决定类型"。
- **不支持泛型参数**：`declare Foo<T> { … }` 报 `SYN025`（`parseDeclare()` 里
  `acceptPunct("<")` 之后立刻 `errorHere`，然后把参数 token 跳到 `>`），这是有意的——
  没有泛型实例化机制，收下参数只会让成员类型无从展开。
- 未知成员是**警告**不是错误：`TC014`（带"可用 `declare …` 补充声明"的建议），因为宿主
  对象往往还有编译器不知道的动态成员（`tests/expected/15-declare-errors.err`）。
- 编译期那份副本：`MacroExpander` 自己也扫一遍 `declare` 建 `declaredTypes_`，
  让 `StaticTyper` 能推 `p hand` 的类型，于是 `@when(#p hand is array)` 能在宏展开期选中
  分支（两份表都用 `buildDeclaredType()` 建，语义一致，但**不是同一份数据**）。

### G3. 宏文件导入的语义（**补齐**）

设计稿"导入：规则与 ts 一致"在 v1 里被实现成"把 token 原样透传，不做任何解析"。v2 给
`.shya` 结尾的 import 加了一层**编译期**语义（`src/modules.cpp`，`loadShyaModules()`，
流水线第 3 步），但刻意没有做成模块系统：

- **宏名是全局的**。被导入的宏经 `MacroExpander::registerMacroFromDecl()` 注册进
  `macros_` 这一张表，`@len` 就是 `@len`，没有 `m.len` 这种限定名，也没有"导入前不可见"
  的作用域。两个文件导出同名宏时，**先注册者胜**，不报 `MAC001`（加载器用 `registered`
  集合去重，同名的第二次注册根本不会发生）。
- **具名导入是"按需注册 + 依赖闭包"，不是命名空间隔离**。
  `import { @assertAny } from "…"` 只注册 `@assertAny` 和它模板体里 `@` 用到的宏
  （`collectMacroUses()` 求传递闭包，所以 `@any` 会一起进来，
  `tests/cases/12-py-macros.shya` 就是靠这条）；side-effect 形式 `import "./m.shya"`
  则注册该文件全部宏。名字不存在报 `MOD005`。
- **被导入文件的 `define` / `declare` 会进入导入方**。加载器把这两个节点拷贝进导入方
  程序（`ModuleSet::others`），所以宏文件里声明的宿主类型与编译期常量对导入方可见；
  反过来，导入方的 `define` 对被导入文件**不可见**（宏展开是单向的）。
- **宏文件内部不能导入 `.js`**：解析到非 `.shya` 的 specifier 报 `MOD004`。理由很直接
  ——`.shya` 文件在编译期被吃掉，它导出的东西没有运行时承载，放一个 JS import 进去只会
  产生一条永远不会被展开的语句。
- **相对路径以导入方文件为基准**，不是以工程根或进程工作目录为基准：
  `tests/cases/12-py-macros.shya` 里的 `../../lib/pystd.shya` 相对该用例文件解析；
  裸名字才会去 `-I/--include` 给的目录里找。找不到报 `MOD001`。
- **`normalisePath` 必须折叠 `.`/`..`**，否则循环守卫失效：键是先规范化再进 `loading_`
  的，`./a.shya` 与 `././a.shya` 折叠前是两个不同字符串，不折叠就会把同一个文件反复当成
  新文件递归下去，直到路径过长/爆栈，而 `MOD002` 一次都不会报。`tests/cases/lib/` 下的
  `loop-a.shya` ↔ `loop-b.shya` 覆盖的是正常路径（`tests/expected/13-module-errors.err`
  里能看到 `MOD001`/`MOD002`/`MOD005` 三条）。
- 宏文件内部的解析错误不会把宏文件自己的行号直接抛给用户：加载器把 error 转成导入点的
  `MOD003`，消息里带 `文件:行:列`，每个文件最多转 6 条。

### G4. AST 节点作为插槽类型（**补齐**）

设计稿只给了 `stmt, expr, type` 与 `...#slots:callExpr`，v2 把"插槽类型"扩到
**42 种 AST 节点种类**（`macro.cpp` 的 `astKindTable()`，完整表见架构文档 5.7）。于是
`macro @swapIf(#cond: compare, #yes: stmt)` 可以要求第一个实参必须是比较节点，不符报
`MAC015`：

```
宏 `@echoLit` 的参数 `#x` 需要 AST 节点 `strLit`，但传入的是 `Num`
```

- **字面量节点为什么要叫 `numLit`/`strLit`/`boolLit`/`voidLit`/`tplLit`**：`number`、
  `string`、`boolean`、`void` 这些名字在 `@when` 的判定里**已经被值类型占用了**
  （`@when(#x is number)` 是静态类型判定，`@when(#x is strLit)` 才是节点判定）。
  如果节点名也叫 `number`，`@when(#x is number)` 就会有歧义——而判定顺序是
  槽类型名 → **值类型名** → 节点种类名，值类型那一层先命中，节点判定永远轮不到。
  加 `Lit` 后缀是让两个命名空间不撞名的唯一办法（`boolLit`/`voidLit` 同理，
  否则会和 `boolean`/`void` 的值类型判定混淆）。
- **哪些节点被排除、为什么**：`program`、`empty`、`macroDecl`、`when`、`whenArm`、
  `each`、`slotList`、`optionalize`、`typeRef` **故意不在表里**。它们都是宏模板自身的
  结构标记，把其中一个替换进模板就是在改写模板本身（`astKindTable()` 上方的注释原话），
  所以 `#x: when` 这类写法查不到类型、`astKindsForTypeName()` 返回空、绑定会回落到
  普通 `SlotType` 判定。
- **`@when(#x is X)` 的判定顺序与三值语义**：`evalWhen()` 先比槽类型名
  （`expr/stmt/type/callExpr/safeCallExpr/rangeExpr`），再比值类型名
  （`array/string/number/boolean/object/map/set/fn/void/unknown`），最后才查
  `astKindsForTypeName()`；命中节点表就返回确定的 `True`/`False`（不再是 `Unknown`），
  两者都不命中才 `Unknown`，而 `Unknown` 不选任何分支。保持三值的原因是：
  "这个实参不是 `binary`"和"我不知道它是什么"必须区分开，否则 `@when(#x is binary)`
  会对着一个未知实参误判成 `False` 而静默走掉。
- 表里唯一的"死角"是 `rangeExpr`：`astKindTable()` 里有 `{"rangeExpr", {NK::RangeExpr}}`，
  但 `slotTypeFromName("rangeExpr")` **先**把它认成 `SlotType::RangeExpr`，`@when` 的槽类型
  分支也排在节点分支之前。更彻底的是，`parseRangeOrExpr(allowRange)` 只在 `allowRange`
  为真时建 `NK::RangeExpr`，而 `parseMacroApply()` 只在宏名**字面上是 `range`** 时传
  `true`（`@probe(1:5)` 实测直接报 `SYN001`/`SYN015`）。所以 `#x: rangeExpr` 这个写法既
  拿不到节点判定、也永远收不到范围实参——节点表里那一条实际不可达，只是留着当文档。
- 依据：`tests/cases/11-ast-types.shya`（`compare`/`strLit`/`block` 正常路径）、
  `tests/expected/11-ast-types.js`（`@kindOf` 的分支各命中一次）。

### G5. 其它已落地的小决策（**补齐 / 修正**）

这一批都是实现里已经定死、但设计稿没写的小口径，逐条记下来免得后人重新考古：

- **复合赋值只作语句**：`+= -= *= /= %= ^=`（外加 `**=`）由 `parseAssignment()` 在 `=`
  之前识别，且只在目标列表恰好一项时尝试。产物里 `op=` 直接照抄，唯一特殊的是 `^=` →
  `**=`（因为 shya 的 `^` 是乘方、`**` 才是 JS 的幂算子），`tests/expected/09-compound.js`
  里就是 `total **= 2;`。它和 `++`/`--` 一样**不能当表达式**：`y = (x += 1)` 实测报
  `CGN004`（"代码生成阶段遇到无法处理的节点 `Assign`"）——括号里那个复合赋值节点没有被
  任何 `genStatement` 消费掉，说明"只能作语句"这条约束是靠**生成阶段**兜底而不是靠
  解析阶段拒绝的。
- **`is not` / `not is` 两种词序都收**：`parseEquality()` 里的 `readCompareOp()` lambda
  统一识别 `== != === !==`、`is [not]`、`instanceof`、`not instanceof`、`not is`，两词形式
  返回"消费 2 个 token"。两种词序都编译成取反形式，`tests/cases/10-typeops.shya` 里
  `3 not is string` 与 `x is not array` 并存。
- **后缀宏不跨行**：`parsePostfix()` 接后缀宏的条件是
  `cur().kind == Tok::At && !cur().newlineBefore`。少了后半个条件，`let a = 1` 换行后写
  `@ifElse(...)` 会被解析成"把 `@ifElse` 后缀作用在 `1` 上"，下一行的语句凭空变成上一行
  的一部分。这与"成员访问不跨行""裸插槽实参不跨行""`@each`/`@when` 拼接不跨行"是同一条
  换行规则的四次应用（见 A5）。
- **裸插槽是"调用片段"**：`parseSlotFragment()` = 可选前缀 `- + ! not` / `...` + 一个
  `parsePrimary()` + 任意个 `(...)` / `?(...)` / `[...]` 尾缀，**不接成员链**（与 A2 同）。
  `@share _p recover(2) draw(2)` 因此是四个参数而不是一个链。
- **关键字可以作插槽名**：`Parser::checkName()` / `checkNameAt()` 接受
  `Tok::Identifier || Tok::Keyword`，`takeName()` 直接取 `text`。所以 `#from`、`#default`、
  `@when(){ #y: … }` 这类写法能过；命名插槽块、`@each` 头、`#name:` 分支名都用这一套。
  `isNamedSlotBlock()` 也据此要求 `{` 后面真的跟着 `#` + 名字 + `:`（否则那个 `{` 是
  `for`/`if` 的语句体，见 B7）。
- **两趟参数绑定**：`bindArguments()` 先把 `NK::Prop`（`{ #name: … }`）按名字填进
  `bound[]`，再用剩下的位置实参（后缀 target 排第一）按声明顺序填"没被具名占掉"的槽。
  所以 `p2 @emit { #a: … }` 里的 `p2` 会落到第一个空位而不是报错（B7 的原文），
  变参槽吃光剩余位置实参。
- **比较链的右操作数存在 `list` 里**（`names` 存运算符、`text` 镜像 `names[0]`）——
  **这是 `@when` 曾经全判 `Unknown` 的根因**。`parseRelational()` 与 `parseEquality()`
  建链时把右操作数放进 `list`、只把第一个放进 `b`；`evalWhen()` 当初只读 `n->b`，
  于是 `@when(#x is array || #x is string)` 这种"`is` 出现在链里"的写法拿到
  `rhs == nullptr`、`want` 为空串、直接 `Unknown`，所有分支都不选、报 `MAC020`（或者在有
  `@when(#x is callExpr)` 这类兜底时悄悄错选）。现在 `evalWhen()` 用
  `NodePtr rhs = n->b ? n->b : (n->list.empty() ? nullptr : n->list[0]);` 兼容两处，
  同时 `instExpr` 也会搬运 `b` 与 `list` 并复制 `names`。
