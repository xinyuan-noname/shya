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

相关文档：[`ast-nodes.md`](ast-nodes.md)（AST 节点与宏插槽类型）、
[`python-users.md`](python-users.md)（Python 用户对照）、
[`compiler-architecture.md`](compiler-architecture.md)（编译器内部）、
[`decisions.md`](decisions.md)（设计稿歧义处的取舍）。

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
let  const  define  declare  macro
fn  async  await  return  throw  try  catch  finally
import  from  export  as
is  not  instanceof  typeof  new
true  false  this  void
```

`is not`、`not instanceof` 是两个词构成一个运算符，两种词序 `is not` / `not is` 都接受。
**`void` 是唯一的空值字面量**：`null` 与 `undefined` 已从语言中移除，写出来会报 `LEX009`
（`@ts{...}` 里的原始 JS 不受此限制，那里可以照常写 `null`）。

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
| `void` | 空值（**语言里只有 `void`，没有 `null` / `undefined`**） |
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
| 其它标识符 | 宿主类型（`Player`、`Card`…），用 [`declare`](#511-declare-声明外置宿主对象) 补充结构 |

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
x not is array      // 同上，另一种词序
x instanceof Date   // 原生 instanceof
x not instanceof Date
```

`is` 右侧写成 `void` 时编译成 `x === undefined`；**不存在 `is null` / `is undefined`**
（这两个词已经不是关键字，会按宿主类型名处理）。

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
[1, 2, 3]                             // 数组
{ x: 1, y: 2 }                        // 对象
{ "带空格的键": 1, nested: { a: [] } }  // 字符串键与嵌套
```

对象字面量由**键值对**和**方法定义**两种成员组成，键是标识符或字符串字面量。
带方法的对象会**一个成员一行**地输出，方法体正常缩进：

```shya
let skill = {
  id: "judge_color",
  trigger: { player: "phaseBegin" },

  // 方法定义：key(参数) { 方法体 }
  filter(event, player) {
    return event kind == "judge"
  },

  content(event: Event, player: Player): void {   // 参数/返回可以写标注，产物里剥掉
    trace(player name)
  },

  // 异步方法
  async resolve(event, player) {
    await @ts{Promise.resolve()}
    trace(player name)
  },
}
```

```js
let skill = {
  id: "judge_color",
  trigger: { player: "phaseBegin" },
  filter(event, player) {
    return event.kind === "judge";
  },
  content(event, player) {
    trace(player.name);
  },
  async resolve(event, player) {
    await Promise.resolve();
    trace(player.name);
  },
};
```

`async` 只作为**方法定义的修饰符**出现，写在方法名前面。`{ async: 1 }` 仍然是一个
键为 `async` 的键值对；但 `async` 是关键字，所以不能用 `obj async` 去读这个名字。

以下形式会报错：

| 写法 | 结果 |
| --- | --- |
| `{ a }`（属性简写） | `SYN030`，提示写成 `{ a: a }` |
| `{ ...x }`（展开） | `SYN013` |
| `{ [k]: 1 }`（计算键） | `SYN013` |

对象字面量只在**表达式位置**成立；语句开头的 `{` 一律是块，
所以裸写 `{ a: 1 }` 当语句是语法错误（也没意义）。

> 方法定义里的 `#插槽`（宏体）可以照常使用，所以技能骨架很适合写成宏 —— 见
> `lib/skill-type.shya` 的 `@skill_trigger`。

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

### 4.4 成员访问：`x y` 读属性，`x y()` 调用

**`. ` 不是运算符**——shya 里没有点号成员访问。成员访问靠**并置**书写，
它们分别读作"取属性"和"发调用"：

```shya
player hp            // player.hp        —— 属性读取
player judge()       // player.judge()   —— 函数调用
arr length           // arr.length       —— 属性
arr push(4)          // arr.push(4)      —— 调用
map get("k")         // map.get("k")     —— 调用
```

**括号总是意味着调用。** 反过来，省略括号时：

| 成员是什么 | `x y` 编译成 |
| --- | --- |
| 属性 / 字段 | `x.y` |
| **无必填参数**的方法 | `x.y()` |
| 类型未知（没 `declare` 过、来自 `@ts` 等） | **`x.y`** —— 属性读取 |

```shya
declare Player {
  hp: number            // 字段
  judge(): Card         // 无必填参数的方法
  recover(n: number): void
}
```

```shya
p hp            // p.hp              —— 字段，属性读取
p judge         // p.judge()         —— 无参方法，省略括号仍是调用
p judge()       // p.judge()         —— 等价写法
p recover(2)    // p.recover(2)      —— 有参数，必须写括号
p recover       // 报错 TC006：期望 1 个实参
p hp()          // 报错 TC015：字段不是方法
```

所以**当你想要的是属性、却没有 `declare` 类型时，省略括号就是对的**；
想让编译器把 `x y` 认成调用，就得让 `y` 有可查的类型（见 [5.11](#511-declare--声明外置宿主对象)）。

也可以直接用下标访问：`arr[0]`、`map[key]`。

> **函数不是一等公民**：shya 没有函数类型、没有函数字面量，也不能把函数作为
> 参数传递（回调）。详见 [10. 已知限制](#10-已知限制)。

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

### 5.11 `declare` —— 声明外置（宿主）对象

shya 不能自定义类，但可以**声明**宿主提供的对象长什么样。`declare` 只参与类型检查，
**不产出任何 JavaScript**：

```shya
declare Card {
  suit: string
  rank: number
}

declare Player {
  name: string              // 字段：`p name` 编译成属性读取 p.name
  hp: number
  hand: array<Card>
  judge(): Card             // 方法：`p judge` 与 `p judge()` 都编译成调用
  recover(n: number): void
  say(msg: string): Player
  draw(n: number = 1): void
  note?: string             // 可选字段
}

declare fn hostRandom(max: number): number
declare fn hostLog(msg: string, level?: number): void
```

声明之后：

- **字段**（`name: T`）读的是属性：`p name` 编译成 `p.name`；
- **方法**（`judge(): T`）生成调用：`p judge` 与 `p judge()` 都编译成 `p.judge()`；
- 成员类型参与检查：`p recover("oops")` 报 `TC003`；`p recover`（缺实参）报 `TC006`；
  对字段写括号调用报 `TC015`；
- 访问未声明的成员是**警告** `TC014`，并给出补声明的位置；它同时按属性读取处理；
- **参数类型不能写成 `fn`**（`SYN032`）：语言不支持回调；
- `declare fn` 的名字进入作用域，可直接调用，参数与返回值都参与检查。

```shya
fn main() {
  let p: Player = @ts{makePlayer()}
  console log(p name, p hp)     // -> p.name, p.hp
  let c: Card = p judge         // -> p.judge()
  p recover(2)                  // -> p.recover(2)
  for card of p hand {          // -> for (const card of p.hand)
    console log(card suit)      // -> card.suit
  }
}
```

`declare` 里写的默认值只描述宿主契约，**不会**由编译器注入到调用点；宿主自己要有默认值。

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
  插槽名可以是关键字，例如 `#from`、`#default`。
- 插槽可标注类型；不标注默认为 `expr`。可用类型分三类：

  | 类别 | 名字 |
  | --- | --- |
  | 基础类别 | `expr`（默认）、`stmt`、`type`、`expr[]` |
  | 语义插槽 | `callExpr`（调用片段）、`safeCallExpr`（安全调用片段） |
  | **AST 节点种类** | `Ident` `Num` `Str` `ObjectLit` `Binary` `Call` `Block` `If` … |

  **大部分安全的 AST 节点都能直接当插槽类型用**，例如 `#cond: Compare` 只接受比较表达式、
  `#b: Block` 只接受块、`#lit: Str` 只接受字符串字面量。传错会报 `MAC015` 并指出实际节点种类。
  类型名不区分大小写的时代已经结束：**名字就是 AST 节点种类名，严格大小写敏感**，
  旧写法（`strLit`、`ident`、`objectLit`…）现在报 `MAC015` 并给出正确拼写。
  完整的名字表、以及哪些宏模板内部节点被刻意排除在外，见
  [`ast-nodes.md`](ast-nodes.md)。

- 宏体本身也必须符合 shya 语法。

#### 可选插槽 `#名字: 类型?`

类型后面多一个 `?`，这个插槽就**可以省略**：

```shya
macro @skill(#id: Ident?, #translation: Str?, #trigger: ObjectLit?, #content: stmt) {
  #id = {
    translation: #translation,
    trigger: #trigger,
  }
  #content
}

@skill {
  #translation: "翻译"
  #content: console log("ok")
}
// #id 与 #trigger 都没传 —— 两条引用都编译成空节点，
// `#id = {...}` 整条赋值不产出，只剩 console.log("ok")
```

- 省略时必须**没有实参**（`#trigger:` 后面什么都不写，或整项不写）；写 `_` 也算省略。
  `stmt` 槽本来就可以省略，不需要 `?`。
- `...` 不定项插槽不能加 `?`。
- 省略的插槽绑定成**空节点**，根据出现的位置有三档：
  1. 整条只用到它的语句（典型是 `#id = { … }`）**整条不产出**，不会留下非法的空基址；
  2. 单独成句的引用（`#content`）也不产出，不会变成一句多余的 `undefined;`；
  3. 必须出现在表达式里时变成 **`undefined`**——`"what=" + #what` → `"what=" + undefined`、
     `[#what]` → `[undefined]`、`{ value: #what }` → `{ value: undefined }`，
     因为表达式里留个空洞是语法错误。
- 没写 `?` 的插槽缺实参仍然报 `MAC016`。

写法上 `?` 只属于**参数声明**：宏模板里的 `?#slot`（强制可选链）与后缀安全调用
`f?(...)` 不受影响，两者可以同时出现（`#x: Call?` 配 `?#x` 是合法组合）。

#### 插槽类型名 = AST 节点种类名（大小写敏感）

上表第三类**就是 `shya ast` 与 `MAC015` 诊断里印出来的节点种类名**，严格区分大小写：
`Str` 是字符串字面量、`Ident` 是标识符、`ObjectLit` 是对象字面量。
完整的名字表见 [`ast-nodes.md`](ast-nodes.md)。

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
// 四个参数： Member(player,nextSeat) / Ident(_p) / Call(recover) / Call(draw)
```

要传复杂表达式请用括号形式 `@macro(a + b)`。

具名插槽的 `#y:` / `#n:` 在 `@when` 中另有含义（见下）。

**具名插槽的值在表达式位置是「表达式」**：`#name: "x"` 里的 `"x"` 是一条表达式语句，
展开到表达式位置（对象字面量、实参）时取的就是这个表达式本身，不会变成 `{ "x"; }`。
值里写了真正的语句（`#filter: return ...`）而插槽又在表达式位置，才会报 `MAC013`。
孤立的 `{}`（如 `#trigger: {}`）在表达式位置是**空对象字面量**。

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
- 条件可用 `is`（插槽形态 `expr`/`stmt`/`callExpr`/`safeCallExpr`/`rangeExpr`，静态类型
  `array`/`string`/`number`/`boolean`/`object`/`map`/`set`/`fn`/`void`/`unknown`，
  或任意 AST 节点种类如 `binary`/`call`/`strLit`）、`||`、`&&`、`!`。
- 判定顺序是：插槽形态 → 值类型 → AST 节点种类；判定是三值的，`unknown` 不会命中任何分支。
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

### 6.4 宏文件导入

宏文件和普通模块**用同一套 import 语法**，只是路径以 `.shya` 结尾：

```shya
import "./pystd.shya"                            // 导入文件里全部宏
import { @enumerate, @zip } from "./pystd.shya"  // 只导入这几个
import { @keys as k } from "./my.shya"           // 改名（语法上接受）
```

- 路径以 `./`、`../` 开头时相对**当前源文件**解析；否则依次在源文件所在目录和 `-I` 指定的
  搜索根下查找。
- 具名导入会**自动带上被依赖的宏**：`import { @assertAny } from "…"` 会把 `@assertAny` 用到的
  `@any` 一起注册，不必手写。
- 被导入文件里的 `define` 与 `declare` 会一并进入导入方。
- 宏导入**完全不产出 JavaScript**；被导入的文件自己也可以导入别的 `.shya`，循环导入报 `MOD002`。
- 宏文件里不能导入 `.js`/`.mjs`（`MOD004`）；普通 JS 模块的 import 原样透传，行为不变。

### 6.5 宏展开的时机

宏展开发生在类型检查**之前**，因此：

- 展开结果必须能通过类型检查；
- `@when` 的类型判断依赖一个**启发式静态类型环境**（扫描 `let x = 字面量` 与参数标注），
  精度有限——类型确实无法确定时会报 `MAC020`，此时请补上类型标注或改写成 `@ts`。

### 6.6 设计稿中的三个链式宏示例

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

### 7.1 Python 用户的宏库 `lib/pystd.shya`

项目自带一份面向 Python 用户的宏库 [`lib/pystd.shya`](../lib/pystd.shya)，用法和其它模块一样：

```shya
import { @enumerate, @zip, @items, @sum, @sorted } from "../lib/pystd.shya"
import { @upper, @strip, @mod } from "../lib/pystd.shya"

for i v of @enumerate(xs) { }        // for i, v in enumerate(xs)
for a b of @zip(xs, ys) { }          // for a, b in zip(xs, ys)
for k v of @items(d) { }             // for k, v in d.items()
console log(xs @sum, xs @sorted)     // sum(xs), sorted(xs)
console log((-7) @mod(3))            // Python 的 % 语义 -> 2
```

提供 `@enumerate` `@zip` `@items` `@count` `@first` `@last` `@sum` `@max` `@min` `@sorted`
`@reversed` `@unique` `@any` `@all` `@join` `@upper` `@lower` `@strip` `@split` `@replace`
`@startswith` `@endswith` `@find` `@abs` `@round` `@int` `@float` `@pow` `@mod` `@clamp`
`@str` `@bool` `@typeOf` `@repr` `@assertAny` `@assertAll`。

逐条对照（含 `%` 符号、成员访问、换行这三个主要陷阱）见
[`python-users.md`](python-users.md)。

### 7.2 标准库源码

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
| `declare Player { name: string }` + `p name` | `p.name`（字段，不加括号） |
| `declare Player { judge(): Card }` + `p judge` | `p.judge()`（方法，自动加括号） |
| `declare fn hostRandom(n: number): number` | （不生成，调用点按普通函数调用） |
| `import { @sum } from "./pystd.shya"` | （不生成，宏在编译期注册） |
| `define A = 100` | （不生成，就地替换为 `100`） |
| `for i of @range 0:9,2` | `for (let i = 0; i < 9; i += 2)` |
| `for n < 3 { }` | `while (n < 3) { }` |
| `for v of m` | `for (const v of m) { }` |
| `for k v of @entries(m)` | `for (const [k, v] of Object.entries(m)) { }` |
| `case x { 1,2: f() 3: fallthrough default: g() }` | `switch (x) { case 1: case 2: f(); break; case 3: /*fallthrough*/ default: g(); break; }` |

### 8.1 产物格式化阶段

代码生成之后还有一个**格式化阶段**（`src/format.cpp`），它决定整个文件的版式。
它只动空白，具体做四件事：

1. 顶层语句之间插入一个空行，让文件读起来是一串声明；
2. 去掉多余的空行（最多保留一个）与空语句 `;`，去掉行尾空白；
3. 修掉 `@ts` 载荷自带分号造成的 `;;`；
4. 文件末尾恰好一个换行。

**它绝不改动 `@ts{…}` 原样载荷**：代码生成会给原样载荷的每一行打上标记，
格式化阶段看到标记就把那行原样输出。字符串与模板字符串同理，因为它们在生成阶段
已经是一个整体。

要保留代码生成时的原始版式，加 `--no-format`（仍然会去掉标记并统一行尾）。

---

## 9. 诊断码表

诊断格式：`文件:行:列: error/warning: 说明 [码]`，并附源码片段与插入符。

| 前缀 | 阶段 | 例 |
| --- | --- | --- |
| `LEX` | 词法分析 | `LEX002` 无法识别的字符；`LEX006` 数学字面量缺少参数；`LEX009` 写了 `null`/`undefined` |
| `SYN` | 语法分析 | `SYN001` 期望某个记号；`SYN011` `#名字` 出现在宏外；`SYN013` 对象字面量的键非法（展开 / 计算键）；`SYN023`+ `declare` 相关；`SYN030` 属性简写；`SYN032` 参数类型写了 `fn` |
| `MOD` | 宏文件导入 | `MOD001` 找不到宏文件；`MOD002` 循环导入；`MOD003` 宏文件本身有错；`MOD004` 宏文件里导入了非 `.shya`；`MOD005` 该文件没有这个宏 |
| `MAC` | 宏展开/脱糖 | `MAC014` 未定义的宏；`MAC015` 插槽类型不符；`MAC016` 缺少参数；`MAC020` `@when` 无分支匹配 |
| `TC` | 类型检查 | `TC003` 类型不符；`TC006` 参数个数不符；`TC010` 未声明的标识符（警告）；`TC013` const 重赋值；`TC014` 宿主类型没有该成员（警告）；`TC015` 把字段当方法调用 |
| `CGN` | 代码生成 | `CGN004` 无法处理的节点（编译器内部错误）；`CGN007` `.shya` 导入未被解析 |

调试宏展开时可设置环境变量 `SHYA_DEBUG_WHEN=1`，编译器会在标准错误输出每次 `@when` 判定结果与宏展开的语句数。

---

## 10. 已知限制

- **没有 `null` / `undefined`**：只有 `void`（`@ts{...}` 里的原始 JS 不受限）。
- **不能自定义类**：设计如此；宿主对象用 [`declare`](#511-declare-声明外置宿主对象) 声明形状。
- **`@when` 的类型判定是启发式**：只扫描字面量初始化、显式标注与 `declare` 成员，
  跨函数/跨语句的复杂推断可能得出 `unknown` 并报 `MAC020`。
- **`declare` 的结构是名义的**：同名即同类型，不做结构化匹配，也不产生运行时校验。
- **`declare` 的默认值不注入调用点**：宿主实现自己要有默认值。
- **无结构化类型**：`object` 不带字段信息，`arr[0]` 之外没有元组/记录类型。
- **无泛型声明**：`<T>` 只在标注中出现，不参与函数泛型推导；`declare Name<T>` 直接报错。
- **`case` 分支体只有一条语句**，多条需自行加块。
- **无位运算**（设计如此），需要时用 `@ts`。
- **`@ts` 内的代码不参与类型检查**（但其中的 `#插槽` 替换是真实节点，会被检查）。
- **箭头函数不提供专门语法**：请用 `fn` 声明或 `@ts`。
- **函数不是一等公民**：没有函数类型（参数写 `fn` 报 `SYN032`）、没有函数字面量、
  不能把函数作为参数传递，也没有 `.` 成员访问运算符可以取出函数引用。
  `@ts{…}` 里的宿主函数不受此限，因为那里不经过 shya 的类型系统。
- **`switch` 的 `default` 写在最前时会退化成一个普通块**（罕见，见 `docs/decisions.md`）。
- **宏文件之间没有隔离**：宏名是全局的，具名导入只做「按需注册 + 依赖闭包」，
  不支持同名覆盖或命名空间。
