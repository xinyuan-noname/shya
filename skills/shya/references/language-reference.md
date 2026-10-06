# shya 语言参考手册

> shya 是一门**强类型**的规则脚本语言：用最接近自然语言的语法书写规则逻辑，用**宏**做模板与复用，
> 用 `@ts` 宿主桥拿底层能力，最终编译成 **ES2026** JavaScript 运行在任何地方。
>
> 本手册描述的是 `shya 1.0.0`（源码见 `src/`，测试见 `tests/`）。文末「已知限制」列出了当前未实现的部分。

## 目录

1. [快速开始](#1-快速开始)
2. [词法](#2-词法)
3. [类型系统](#3-类型系统)
4. [表达式](#4-表达式)
5. [语句](#5-语句)
6. [宏](#6-宏)
7. [标准库宏](#7-标准库宏)
8. [编译产物对照表](#8-编译产物对照表)
9. [诊断码表](#9-诊断码表)
10. [已知限制](#10-已知限制)

---

## 1. 快速开始

```sh
build.bat                     # 用 MSVC 构建 build\shya.exe
build\shya.exe build a.shya   # 编译成 a.mjs
build\shya.exe run a.shya     # 编译并立刻用 node 运行
build\shya.exe check a.shya   # 只做类型检查，不产出文件
build\shya.exe ast a.shya     # 打印语法树（调试用）
build\shya.exe core a.shya    # 打印宏展开后的核心语法树
build\shya.exe tokens a.shya  # 打印词法单元流
node tests\run.mjs            # 跑全部测试用例
```

选项：`-o/--out <路径>`、`--no-typecheck`、`--warnings-as-errors`、`-q/--quiet`。

一个最小的 shya 程序：

```shya
define MAX = 100

fn describe(name: string, times: number): string {
  let out = name
  for i of @range 0:times,1 {
    out = out + "!"
  }
  return out
}

fn main() {
  let label = "shya"
  console log(describe(label, 3))
  console log(MAX + MAX)
}

main()
```

编译结果：

```js
function describe(name, times) {
  let out = name;
  for (let i = 0; i < times; i += 1) {
    out = out + "!";
  }
  return out;
}
function main() {
  const label = "shya";
  console.log(describe(label, 3));
  console.log(100 + 100);
}
main();
```

注意 `define MAX` 在产物中**完全没有变量**：它被当作编译期常量就地替换。

---

## 2. 词法

### 2.1 注释

```shya
// 行注释
/* 块注释，支持嵌套 /* 像这样 */ 仍然闭合 */
```

### 2.2 标识符与关键字

标识符由字母、数字、`_`、`$` 及任意非 ASCII 字符组成，不能以数字开头。
`_` 单独出现是**占位符**，不是普通标识符（见 [6.10](#610--占位符)）。

关键字：

```
if  elif  else  case  default  fallthrough
for  of  break  continue
let  const  define  macro
fn  async  await  return  throw  try  catch  finally
import  from  export  as
is  not  instanceof  typeof  new
true  false  this  void  null  undefined
```

`is not`、`not instanceof` 是两个词构成一个运算符。
`null` 与 `undefined` 都写作 `void`（两者是等价的别名，产物统一为 `undefined`）。

### 2.3 数值字面量

十进制整数/小数/科学计数法，允许 `_` 作千分位分隔：`1_000_000`、`1.5e-3`。
也支持 `0x`、`0b`、`0o` 前缀。

### 2.4 数学字面量

以 `~` 开头的**编译期常量**，在词法阶段就折叠成一个数：

| 写法 | 含义 | 例 |
| --- | --- | --- |
| `~pi` / `~π` | 圆周率 | `3.141592653589793` |
| `~e` / `~ℯ` | 自然常数 | `2.718281828459045` |
| `~tau` / `~τ` | 2π | `6.283185307179586` |
| `~inf` / `~∞` | 无穷大 | `Infinity` |
| `~lgN` | log₁₀(N) | `~lg10` → `1` |
| `~lnN` | ln(N) | `~ln4` → `1.3862943611198906` |
| `~dbN` | 10·log₁₀(N)（分贝） | `~db10` → `10` |
| `~degN` | N 度 → 弧度 | `~deg2` → `0.03490658503988659` |
| `~radN` | N 弧度 → 度 | `~rad2` → `114.59155902616465` |
| `~sqrtN` | √N | `~sqrt2` → `1.4142135623730951` |

参数可以嵌套：`~ln~deg360`（= ln(2π)）、`~deg~e` 都合法。
**常量不需要参数，函数必须有参数**：`~dege` 会报 `LEX006`（并提示「若要自然常数请写 `~e`」）。

### 2.5 字符串

单引号、双引号、以及模板字符串。模板字符串里 `${...}` 中的内容是 **shya 表达式**，会被正常编译：

```shya
let name = "世界"
let s = `你好，${name}！${1 + 1}`
```

### 2.6 标点与运算符

```
( ) [ ] { } , : ; . ...
= += -= *= /= %= ^=
++ -- + - * / % ^
== != === !== < <= > >=
&& || ! ~/ +/ -/ \/ ? ... @ # ~
```

注意 `~/`（向零截断的除法）与 `~`（数学字面量）的区分由词法阶段完成：`~` 后面跟 `/` 一律视为除法运算符。

### 2.7 换行与语句分隔

**换行是语句分隔符**，分号可选：

```shya
console log(1)
console log(2)     // 两条语句，不是 console.log(1).log(2)
```

由此产生三条规则，编译器严格遵守：

1. **成员访问不跨行**。`a\n b` 是两条语句，不是 `a.b()`。
2. **宏的后置插槽参数不跨行**。`x @safe a(1)\n b(2)` 中第二行是新语句。
3. **`@each` / `@when` 拼接线不跨行**。宏模板里 `#x @each(...)` 必须在同一行；下一行开头的 `@when` 是新语句。

`;` 同时是**空语句**（`case` 分支里常用：`1,2:;`）。

---

## 3. 类型系统

shya 是强类型语言，类型标注语法与 TypeScript 一致，但**只有基本的类型系统**：不能自定义类，
标注中的类名（如 `Player`）被当作宿主提供的类型对待。

### 3.1 基础类型

| 类型 | 说明 |
| --- | --- |
| `number` / `int` / `float` / `double` | 数值 |
| `string` | 字符串 |
| `boolean` / `bool` | 布尔 |
| `void` / `null` / `undefined` | 空值 |
| `array<T>` / `T[]` / `list<T>` | 数组 |
| `map<K,V>` / `record<K,V>` | 映射 |
| `set<T>` | 集合 |
| `object` / `obj` | 普通对象字面量 |
| `fn` / `function` | 函数 |
| `rangeExpr` / `range` | 范围表达式 |
| `any` / `unknown` | 逃逸舱：`any` 关闭检查，`unknown` 表示「尚不知道」 |
| `never` | 永不返回 |
| 联合 `A \| B` | 联合类型 |
| 字面量类型 `"red"` / `3` | 字面量类型 |
| 其它标识符 | 宿主类型（`Player`、`Card`…） |

### 3.2 标注位置

```shya
let hp: number = 4          // 变量
const NAME: string = "x"    // 常量
fn f(a: number, b: string = "s", ...rest: number[]): boolean { ... }
```

`let` / `const` 可以省略，此时变量类型由初始化表达式推断：

```shya
hp: number = 4              // 显式标注的声明，等价于 let hp: number = 4
hp = 5                      // 普通赋值
```

### 3.3 类型判断

```shya
x is array          // Array.isArray(x)
x is string         // typeof x === "string"
x is number         // typeof x === "number"
x is boolean        // typeof x === "boolean"
x is fn             // typeof x === "function"
x is map            // x instanceof Map
x is set            // x instanceof Set
x is void           // x === undefined
x is object         // typeof x === "object" && x !== null
x is Player         // x instanceof Player（宿主类型）
x is not array      // 取反
x instanceof Date   // 原生 instanceof
x not instanceof Date
```

当 `x` 有副作用时，编译器会把它提升成一个临时函数参数，保证只求值一次：

```shya
player getHp is number
// -> ((__shya_is) => typeof __shya_is === "number")(player.getHp())
```

### 3.4 检查的宽严

类型检查是**渐进式**的：

- `any` 与 `unknown` 会静默通过所有检查；
- 未知的成员访问/调用结果一律推断为 `unknown`；
- 未声明的标识符只报 **warning**（`TC010`），不阻断编译（宿主的全局量可能来自外部）；
- 明确的错误才报 error：初始化/赋值类型不符、调用参数个数不符、运算符操作数类型不符、`const` 重赋值。

---

## 4. 表达式

### 4.1 字面量

```shya
1  1.5  0xff  1_000
"text"  'text'  `模板 ${1}`
true  false  void
[1, 2, 3]                     // 数组
{ x: 1, y: 2 }                // 对象
{ a: 1, m(x) { return x } }   // 对象方法简写
```

### 4.2 运算符与优先级（低 → 高）

| 优先级 | 运算符 | 说明 |
| --- | --- | --- |
| 1 | `? :` | 三元 |
| 2 | `\|\|` | 逻辑或 |
| 3 | `&&` | 逻辑与 |
| 4 | `==` `!=` `===` `!==` `is` `is not` `instanceof` `not instanceof` | 相等与类型判断 |
| 5 | `<` `<=` `>` `>=` | 关系（可与 4 级构成链式比较） |
| 6 | `+` `-` | 加、减 |
| 7 | `*` `/` `%` `~/` `+/` `-/` `\/` | 乘、除、取模、四种取整除法 |
| 8 | `^` | 乘方（右结合） |
| 9 | `!` `not` `-` `+` `typeof` `new` `await` `...` | 一元 |
| 10 | `f(x)` `a b`（成员访问） `a[i]` `a?(x)` `x @macro` | 后缀 |

**没有位运算**。`!` 与 `not` 等价。`^` 是乘方而不是异或。

四种除法：

```shya
7 / 2    // 3.5
7 ~/ 2   // 3   向零截断 -> Math.trunc(7 / 2)
7 +/ 2   // 4   向上取整 -> Math.ceil(7 / 2)
7 -/ 2   // 3   向下取整 -> Math.floor(7 / 2)
7 \/ 2   // 4   四舍五入 -> Math.round(7 / 2)
```

### 4.3 链式比较（与 Python 一致）

```shya
1 < 2 < 3        // (1 < 2) && (2 < 3)  -> true
a < b <= c
```

中间操作数有副作用时会只求值一次。

`==` **只有严格相等**一个含义，产物是 `===`。

### 4.4 成员访问就是调用

shya **没有属性这个概念，一切皆是函数**：不带参数的成员访问也会生成一次调用。

```shya
player getHp                       // player.getHp()
player nextSeat nextSeat recover(2) // player.nextSeat().nextSeat().recover(2)
array slice(0, -1) indexOf(5)      // array.slice(0, -1).indexOf(5)
```

需要真正的属性读取（如 `.length`）时使用标准库宏或 `@ts`：

```shya
arr @len          // arr.length
bag @keys         // Object.keys(bag)
```

也可以用下标访问：`arr[0]`、`map[key]`。

### 4.5 安全访问

```shya
f?(x, y)     // f?.(x, y)
```

链式安全访问由标准库宏 `@safe` 提供（见 [7](#7-标准库宏)）。

### 4.6 其它

```shya
cond ? a : b          // 三元
arr[0]                // 下标
...xs                 // 展开（数组、实参）
await p               // 等待
new Map()             // 构造
typeof x
```

---

## 5. 语句

### 5.1 变量声明与 let/const 推断

```shya
let a = 1        // 显式 let
const b = 2      // 显式 const
c = 3            // 省略：由编译器推断
d: number = 4    // 带类型标注的省略形式
```

推断规则：**同一个作用域内只被写入一次的名字生成 `const`，被再次赋值的名字生成 `let`**。

```shya
x = 1        // const x = 1;
y = 1
y = 2        // let y = 1;  y = 2;
```

显式 `const` 被重新赋值是错误（`TC013`）。

多重赋值：

```shya
x, y = 1, 2          // 两个都是新名字 -> const x = 1; const y = 2;
a, b = b, a          // 都已声明 -> [a, b] = [b, a];（同时赋值）
```

### 5.2 define —— 编译期常量

```shya
define a = 100
define half = a / 2
player draw(a + a)      // -> player.draw(100 + 100)
```

`define` 的值必须是编译期常量：字面量、数学字面量、其它 `define`、以及它们之间的常量运算
（`+ - * / % ^ ~/ +/ -/ \/`、字符串拼接、布尔取反）。否则报 `MAC019`。
产物中**不会出现该变量**。

### 5.3 赋值与自增

```shya
x = 1
x += 1
x -= 1
x *= 2
x /= 2
x %= 2
x ^= 2        // 注意：^ 是乘方，产物是 x **= 2
i++           // 自增只能作为语句，不构成表达式
i--
++i
--i
```

复合赋值支持 `+=` `-=` `*=` `/=` `%=` `^=`（以及词法上的 `**=`），与 `++`/`--` 一样**只能作为语句**。

### 5.4 分支

```shya
if cond {
  ...
} elif other {
  ...
} else {
  ...
}
```

圆括号可写可不写：`if (cond) { }` 与 `if cond { }` 等价。

### 5.5 case 分支

```shya
case label {
  "shya", "noname": console log("命中")
  "other": fallthrough
  default: console log("默认")
}
```

- 分支体**恰好一条语句**；要写多条请用块 `{ ... }`，`;` 表示空语句。
- 每个分支**默认 break**，写 `fallthrough` 才会贯穿到下一个分支。
- 省略主体时按条件匹配（等价于 if/elif 链）：

```shya
case {
  hp < 1: console log("濒死")
  hp < 3: console log("受伤")
  default: console log("健康")
}
```

### 5.6 循环

只有两种循环。

**条件循环**（`for(条件)`，产物是 `while`）：

```shya
for n < 3 {
  n++
}
```

**遍历**：

```shya
for player of players { }              // for (const player of players)
for attr of @keys(player) { }          // for (const attr of Object.keys(player))
for value of @values(m) { }
for attr value of @entries(m) { }      // for (const [attr, value] of Object.entries(m))
```

`break` / `continue` 保留。

**区间遍历 `@range`**，统一含首不含尾：

```shya
for i of @range 0:100,2 { }      // for (let i = 0; i < 100; i += 2)
for i of @range 3:0,-1 { }       // 倒序：for (let i = 3; i > 0; i += -1)
for v of arr @range 1:3,1 { }    // 取集合下标 [1,3) 的元素
```

步长可省（默认 1）。`@range` 只能出现在 `for ... of` 的遍历位置。

### 5.7 函数

```shya
fn name(a: number, b: string = "x", ...rest: number[]): boolean {
  return true
}

async fn later(): number {
  await @ts{new Promise((r) => setTimeout(r, 10))}
  return 1
}
```

默认参数不要求放在最后：`fn f(a: number = 1, b: number)` 合法（默认值只填在缺参时生效）。
`fn` 也可以作为表达式（匿名函数）。

### 5.8 return / throw / try

```shya
fn risky(flag: boolean): string {
  try {
    if flag {
      throw new Error("boom")
    }
    return "ok"
  } catch e {
    return "caught: " + @ts{e.message}
  } finally {
    console log("finally")
  }
}
```

`catch` 的名字可带圆括号（`catch (e)`）也可不带（`catch e`）。

### 5.9 import / export

与 TypeScript 规则一致，原样透传到产物：

```shya
import { helper } from "./helper.mjs"
import * as ns from "./ns.mjs"
import "./side-effect.mjs"

export fn pub(): number { return 1 }
export default main
export { a, b }
export * from "./other.mjs"
```

### 5.10 `_` 占位符

`_` 有两层含义：

1. **待完成的语句**——什么都不生成，相当于 Python 的 `pass`：

```shya
if false {
  _
}
```

2. **宏调用时表示「没有 target」**：

```shya
_ @judge_color {
  #red: console log("红")
}
```

`_` 作为普通变量名是不可用的。

---

## 6. 宏

宏是 shya 的模板与复用机制：宏定义是**模板**，调用时按插槽替换并展开，展开结果继续参与类型检查与代码生成。

### 6.1 定义

```shya
macro @keys(#x) {
  @ts{Object.keys(#x)}
}

macro @judge_color(#target: Player, #red: stmt, #black: stmt, #none: stmt) {
  card = #target judge
  case (card getSuit) {
    "red": #red;
    "black": #black;
    "none": #none;
  }
}
```

- 插槽参数写成 `#名字`，可变参数写成 `...#名字`（只能标注为 `expr[]` 或其子类型）。
- 插槽可标注类型；不标注默认为 `expr`。可用类型：

  | 插槽类型 | 接受 |
  | --- | --- |
  | `expr` | 任意表达式（默认） |
  | `stmt` | 语句；传表达式时会自动包成一条语句 |
  | `expr[]` | 不定项 |
  | `callExpr` | 调用片段，如 `recover(2)` |
  | `safeCallExpr` | 安全调用片段，如 `say?(x)` |
  | `rangeExpr` | 范围表达式 `start:end,step` |
  | `type` | 类型名 |

- 宏体本身也必须符合 shya 语法。

### 6.2 调用形式

| 形式 | 说明 |
| --- | --- |
| `@macro(a, b)` | 前缀，参数按位置绑定 |
| `x @macro` | 后缀，`x` 成为第一个参数；等价于 `@macro(x)` |
| `x @macro a(1) b(2)` | 后缀 + 无括号的插槽参数 |
| `@macro { #a: ...  #b: ... }` | 具名插槽 |
| `x @macro { #a: ... }` | 后缀 + 具名插槽 |

**无括号的插槽参数是单个调用片段，不是成员链**——这一点很关键：

```shya
@share(player nextSeat, _p, recover(2), draw(2))
player nextSeat @share _p recover(2) draw(2)    // 两种写法等价
// 三个参数： Member(player,nextSeat) / Ident(_p) / Call(recover) / Call(draw)
```

要传复杂表达式请用括号形式 `@macro(a + b)`。

具名插槽的 `#y:` / `#n:` 在 `@when` 中另有含义（见下）。

### 6.3 系统宏

#### `@ts{ ... }` —— 原样透传

块内的内容**按 TS/JS 语法原样输出**，编译器不做任何解析；只有 `#插槽` 会被替换：

```shya
macro @keys(#x) {
  @ts{Object.keys(#x)}
}
bag @keys        // -> Object.keys(bag)
```

`@ts{` 内的花括号会配对计数，所以可以直接写对象字面量：

```shya
let d = @ts{{ say: (m) => console.log(m) }}
```

#### `@when(条件) { ... }` —— 编译期分支

条件是关于**插槽静态类型/形态**的编译期判断，在展开时决定保留哪一支：

```shya
macro @len(#x) {
  @when(#x is array || #x is string) @ts{#x.length}
  @when(#x is map || #x is set) @ts{#x.size}
}
```

- 分支只有一支时可省略花括号、直接接一条语句/表达式。
- 两支时写成 `{ y: ...  n: ... }`（`#y:` / `#n:` 也可以）。
- 条件可用 `is`（插槽形态 `expr`/`stmt`/`callExpr`/`safeCallExpr`/`rangeExpr`，或静态类型
  `array`/`string`/`number`/`boolean`/`object`/`map`/`set`/`fn`/`void`/`unknown`）、`||`、`&&`、`!`。
- **一个分支都没匹配是编译错误**（`MAC020`），不会静默生成空代码。

#### `@each(#项 of #列表) { ... }` —— 编译期遍历不定项插槽

```shya
macro @share(#x, #y, ...#slots: callExpr) {
  #y = #x
  @each(#slot of #slots) {
    #y #slot
  }
}
```

#### `@range start:end,step`

系统宏，只能在 `for ... of` 的遍历位置使用，展开成计数循环（见 [5.6](#56-循环)）。

### 6.4 宏展开的时机

宏展开发生在类型检查**之前**，因此：

- 展开结果必须能通过类型检查；
- `@when` 的类型判断依赖一个**启发式静态类型环境**（扫描 `let x = 字面量` 与参数标注），
  精度有限——类型确实无法确定时会报 `MAC020`，此时请补上类型标注或改写成 `@ts`。

### 6.5 设计稿中的三个链式宏示例

```shya
d @safe say("hello") say("nice")
// -> d?.say("hello")?.say("nice")

player nextSeat @share _p recover(2) draw(2)
// -> const _p = player.nextSeat();
//    _p.recover(2);
//    _p.draw(2);

player nextSeat @safe_share _q recover(2) draw(2)
// -> const _q = player.nextSeat();
//    _q?.recover(2);
//    _q?.draw(2);
```

完整可运行版本见 `tests/cases/02-macros.shya`。

---

## 7. 标准库宏

标准库以 **shya 源码**的形式内建在编译器里（`src/stdlib.cpp`，`kStdlibSource`），
开箱即用，也可以被用户宏覆盖。原文即设计稿中的宏定义（仅去掉了 `@when` 分支里多余的逗号）。

| 宏 | 用法 | 产物 |
| --- | --- | --- |
| `@keys` | `a @keys` | `Object.keys(a)` |
| `@values` | `a @values` | `Object.values(a)` |
| `@entries` | `a @entries` | `Object.entries(a)` |
| `@len` | `arr @len` / `str @len` | `.length` |
| `@len` | `map @len` / `set @len` | `.size` |
| `@safe` | `d @safe say(1) say(2)` | `d?.say(1)?.say(2)` |
| `@share` | `x @share _p f(1) g(2)` | `const _p = x; _p.f(1); _p.g(2)` |
| `@safe_share` | `x @safe_share _p f(1) g(2)` | `const _p = x; _p?.f(1); _p?.g(2)` |

`@len` 只认 `array` / `string` / `map` / `set`；**对象字面量不是 map**，`{a:1} @len` 会报 `MAC020`，
请改用 `new Map()` 或显式标注类型。

标准库源码：

```shya
macro @keys(#x) {
  @ts{Object.keys(#x)}
}

macro @values(#x) {
  @ts{Object.values(#x)}
}

macro @entries(#x) {
  @ts{Object.entries(#x)}
}

macro @len(#x) {
  @when(#x is array || #x is string) @ts{#x.length}
  @when(#x is map || #x is set) @ts{#x.size}
}

macro @safe(#x, ...#slots: callExpr) {
  #x @each(#slot of #slots) {
    @when(#slot is safeCallExpr) {
      y: #slot
      n: ?#slot
    }
  }
}

macro @share(#x, #y, ...#slots: callExpr) {
  #y = #x
  @each(#slot of #slots) {
    #y #slot
  }
}

macro @safe_share(#x, #y, ...#slots: callExpr) {
  #y = #x
  @each(#slot of #slots) {
    @when(#slot is safeCallExpr) {
      y: #slot
      n: ?#slot
    }
  }
}
```

---

## 8. 编译产物对照表

| shya | ES2026 |
| --- | --- |
| `void` | `undefined` |
| `~lg10` | `1` |
| `~pi` | `3.141592653589793` |
| `a == b` | `a === b` |
| `a != b` | `a !== b` |
| `1 < 2 < 3` | `1 < 2 && 2 < 3` |
| `a ^ b` | `a ** b` |
| `a ~/ b` | `Math.trunc(a / b)` |
| `a +/ b` | `Math.ceil(a / b)` |
| `a -/ b` | `Math.floor(a / b)` |
| `a \/ b` | `Math.round(a / b)` |
| `arr @len` | `arr.length` |
| `m @len` | `m.size` |
| `bag @keys` | `Object.keys(bag)` |
| `x is array` | `Array.isArray(x)` |
| `x is string` | `typeof x === "string"` |
| `x is void` | `x === undefined` |
| `player getHp` | `player.getHp()` |
| `f?(1)` | `f?.(1)` |
| `d @safe say(1) say(2)` | `d?.say(1)?.say(2)` |
| `x = 1`（唯一赋值） | `const x = 1;` |
| `x = 1; x = 2` | `let x = 1; x = 2;` |
| `x += 1` | `x += 1;` |
| `x ^= 2` | `x **= 2;` |
| `define A = 100` | （不生成，就地替换为 `100`） |
| `for i of @range 0:9,2` | `for (let i = 0; i < 9; i += 2)` |
| `for n < 3 { }` | `while (n < 3) { }` |
| `for v of m` | `for (const v of m) { }` |
| `for k v of @entries(m)` | `for (const [k, v] of Object.entries(m)) { }` |
| `case x { 1,2: f() 3: fallthrough default: g() }` | `switch (x) { case 1: case 2: f(); break; case 3: /*fallthrough*/ default: g(); break; }` |

---

## 9. 诊断码表

诊断格式：`文件:行:列: error/warning: 说明 [码]`，并附源码片段与插入符。

| 前缀 | 阶段 | 例 |
| --- | --- | --- |
| `LEX` | 词法分析 | `LEX002` 无法识别的字符；`LEX006` 数学字面量缺少参数 |
| `SYN` | 语法分析 | `SYN001` 期望某个记号；`SYN011` `#名字` 出现在宏外 |
| `MAC` | 宏展开/脱糖 | `MAC014` 未定义的宏；`MAC016` 缺少参数；`MAC020` `@when` 无分支匹配 |
| `TC` | 类型检查 | `TC003` 类型不符；`TC006` 参数个数不符；`TC010` 未声明的标识符（警告）；`TC013` const 重赋值 |
| `CGN` | 代码生成 | `CGN004` 无法处理的节点（编译器内部错误） |

调试宏展开时可设置环境变量 `SHYA_DEBUG_WHEN=1`，编译器会在标准错误输出每次 `@when` 判定结果与宏展开的语句数。

---

## 10. 已知限制

- **不能自定义类**：设计如此，标注中的类名只在 `is` / `instanceof` 中对接宿主类型。
- **`@when` 的类型判定是启发式**：只扫描字面量初始化与显式标注，跨函数/跨语句的复杂推断可能得出
  `unknown` 并报 `MAC020`。
- **无结构化类型**：`object` 不带字段信息，`arr[0]` 之外没有元组/记录类型。
- **无泛型声明**：`<T>` 只在标注中出现，不参与函数泛型推导。
- **`case` 分支体只有一条语句**，多条需自行加块。
- **无位运算**（设计如此），需要时用 `@ts`。
- **`@ts` 内的代码不参与类型检查**。
- **箭头函数不提供专门语法**：请用 `fn` 声明或 `@ts`。
- **`switch` 的 `default` 写在最前时会退化成一个普通块**（罕见，见 `docs/decisions.md`）。
