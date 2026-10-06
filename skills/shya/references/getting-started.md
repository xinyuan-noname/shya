# 开始写 shya 程序

这份文档带你从零写到一个能用的 shya 程序。**每一段 shya 代码都是可以单独编译运行的完整程序**，
本文所有片段都已用 `build/shya.exe` 实测过。

## 0. 先跑起来

```sh
build.bat                      # 构建编译器（需要 VS2022 的 C++ 工作负载）
build\shya.exe run hello.shya  # 编译并立刻用 node 运行
```

其它命令：

| 命令 | 用途 |
| --- | --- |
| `shya run a.shya` | 编译 + 运行，最常用 |
| `shya build a.shya -o a.mjs` | 只产出 `.mjs` |
| `shya check a.shya` | 只做类型检查（CI / 编辑时用） |
| `shya core a.shya` | 看宏展开后的样子，**调宏必备** |
| `shya ast a.shya` / `shya tokens a.shya` | 看语法树 / 词法单元 |

---

## 1. 最小程序

新建 `hello.shya`：

```shya
fn main() {
  console log("你好，shya")
}

main()
```

```sh
build\shya.exe run hello.shya
# 你好，shya
```

编译产物是 ES2026，没有运行时、没有包装：

```js
function main() {
  console.log("你好，shya");
}
main();
```

**要记住的第一件事**：`console log(...)` 就是 `console.log(...)`。
shya 里没有"点号访问属性"这种语法——**成员访问本身就是一次函数调用**（后面第 5 节会用到这个特性）。

---

## 2. 变量、常量与类型

```shya
define MAX_HP = 4

fn main() {
  let name: string = "赵云"
  let hp = MAX_HP
  hp = hp - 1
  const side = "蜀"
  console log(name, side, hp, MAX_HP)
}

main()
```

输出 `赵云 蜀 3 4`。要点：

- **`let` / `const` 可以省**，编译器自己推断：这个名字只被赋值一次 → 生成 `const`，
  被重新赋值过 → 生成 `let`。上面 `hp` 被改了，所以是 `let`；`side` 没改，是 `const`。
- **`define` 是编译期常量**，产物里根本没有 `MAX_HP` 这个变量，用到它的地方直接变成 `4`：

  ```js
  const name = "赵云";
  let hp = 4;
  hp = hp - 1;
  const side = "蜀";
  console.log(name, side, hp, 4);
  ```

- **类型标注是可选的**，但写了就会被检查。写 `let hp: number = "满血"` 会报 `TC003`。

---

## 3. 分支与循环

```shya
fn main() {
  let xs = [3, 1, 4]

  for x of xs {
    if x > 2 {
      console log(x, "大")
    } elif x == 2 {
      console log(x, "中")
    } else {
      console log(x, "小")
    }
  }

  for i of @range 0:3,1 {
    console log("第", i, "个")
  }
}

main()
```

要点：

- `if` 的圆括号可写可不写（`if (x > 2) { }` 也行）。注意 `==` **只有严格相等**一种含义。
- **只有两种循环**：`for 条件 { }`（产物是 `while`）和 `for x of 集合 { }`。
  没有三段式 `for(;;)`。
- `@range 起:止,步` **含首不含尾**，步长可省：`@range 0:3,1` 走 `0 1 2`。
  倒序写 `@range 3:0,-1`。

输出：

```
3 大
1 小
4 大
第 0 个
第 1 个
第 2 个
```

遍历 Map / 对象：

```shya
fn main() {
  let d = { 一: 1, 二: 2 }
  for k v of @entries(d) {
    console log(k, v)
  }
}

main()
```

```
一 1
二 2
```

---

## 4. 函数

```shya
fn describe(name: string, times: number = 1): string {
  let out = name
  for i of @range 0:times,1 {
    out = out + "!"
  }
  return out
}

fn total(first: number, ...rest: number[]): number {
  let sum = first
  for v of rest {
    sum = sum + v
  }
  return sum
}

fn main() {
  console log(describe("赵云"))
  console log(describe("赵云", 3))
  console log(total(1, 2, 3, 4))
}

main()
```

```
赵云!
赵云!!!
10
```

- 默认参数、剩余参数（`...rest: number[]`）都有；默认参数**不要求放在最后**。
- 调用参数个数和类型会被检查：`total()` 报 `TC006`，`describe(1)` 报 `TC003`。
- 异步用 `async fn`，里面用 `await`。

---

## 5. 对接宿主对象：`declare`（最重要的一节）

shya 不能自定义类，宿主对象（游戏引擎、浏览器 API、已有 JS 库）的**形状**用 `declare` 声明。
`declare` 只做类型检查，**不产出任何 JavaScript**。

```shya
@ts{globalThis.player = {
  name: "赵云",
  hp: 4,
  nextSeat: () => globalThis.player,
  judge: () => ({ suit: "red", rank: 7 }),
  recover: (n) => { console.log("回复 " + n); },
};}

declare Card {
  suit: string
  rank: number
}

declare Player {
  name: string              // 字段
  hp: number
  nextSeat(): Player        // 方法
  judge(): Card
  recover(n: number): void
}

fn main() {
  let p: Player = @ts{globalThis.player}

  console log(p name, p hp)        // 字段 -> p.name、p.hp（不加括号）
  p recover(2)                     // 方法 -> p.recover(2)

  let c: Card = p judge            // -> p.judge()
  console log(c suit, c rank)

  let q: Player = p nextSeat       // -> p.nextSeat()
  console log(q name)
}

main()
```

这是 shya 里唯一需要"记住规则"的地方：

| `declare` 里写的 | `p xxx` 编译成 |
| --- | --- |
| `name: string`（字段） | `p.name` —— 属性读取 |
| `recover(n: number): void`（方法） | `p.recover(n)` —— 函数调用 |

**没有 `declare` 的话，`p name` 会编译成 `p.name()`**，因为 shya 的默认规则是"一切皆是函数"。

宿主提供的全局函数也可以声明：

```shya
@ts{globalThis.hostRandom = (max) => Math.floor(max / 2);}

declare fn hostRandom(max: number): number

fn main() {
  console log(hostRandom(6))   // -> 3
}
main()
```

访问没声明过的成员只是**警告**（`TC014`），并且会告诉你该怎么补：

```
warning: 类型 `Player` 没有声明成员 `handSize`（可用 `declare Player { handSize: … }` 补充声明）
```

---

## 6. 宏：写出你自己的语法

宏在**编译期**展开成普通代码，产物里没有任何宏的痕迹。

### 6.1 定义与调用

```shya
// 定义：宏名以 @ 开头，#名字 是插槽
macro @unless(#cond, #body: stmt) {
  if #cond {
    _
  } else {
    #body
  }
}

fn main() {
  let hp = 3
  @unless(hp > 0, console log("已阵亡"))
  console log("继续")
}
main()
```

`_` 是"什么都不做"的占位语句（相当于 Python 的 `pass`）。

### 6.2 后缀形式与具名插槽

```shya
macro @onColor(#target: Player, #red: stmt, #black: stmt) {
  card = #target judge
  case (card getSuit) {
    "red": #red;
    "black": #black;
  }
}

@ts{globalThis.player = { judge: () => ({ getSuit: () => "red" }) };}

declare Card { getSuit(): string }
declare Player { judge(): Card }

fn main() {
  let p: Player = @ts{globalThis.player}

  // 具名插槽：读起来就是一句话
  p @onColor {
    #red: console log("红，扣 1 血")
    #black: console log("黑，摸一张")
  }
}
main()
```

- `p @onColor { ... }` 里的 `p` 是后缀目标，自动成为第一个参数（`#target`）。
- `@onColor(p) { ... }` 是等价的括号写法。
- 没有目标时第一个参数写 `_`。

### 6.3 插槽类型（传错了会被拦住）

```shya
macro @swapIf(#cond: compare, #yes: stmt, #no: stmt) {
  if #cond {
    #yes
  } else {
    #no
  }
}

macro @echoLit(#x: strLit) {
  console log("字面量是 " + #x)
}

fn main() {
  let a = 1
  @swapIf(a < 2, console log("小于"), console log("不小于"))
  @echoLit("hello")
}
main()
```

`#cond: compare` 要求实参必须是比较表达式；传 `1` 会报：

```
error: 宏 `@swapIf` 的参数 `#cond` 需要 AST 节点 `compare`，但传入的是 `Num` [MAC015]
```

除了 `expr` / `stmt` / `type` / `expr[]` / `callExpr` / `safeCallExpr` 六个基础类别，
**40 多个安全 AST 节点种类都能直接当插槽类型**（`ident` `numLit` `strLit` `binary` `call`
`member` `arrayLit` `ifStmt` `block` …），完整表见
[`ast-nodes.md`](ast-nodes.md)。

### 6.4 调试宏

宏展开不符合预期时，先看展开结果，再看判定过程：

```sh
build\shya.exe core a.shya        # 看宏展开成了什么
set SHYA_DEBUG_WHEN=1             # 每次 @when 判定都打印
```

---

## 7. 用现成的宏库

标准库宏**开箱即用**：`@keys` `@values` `@entries` `@len` `@safe` `@share` `@safe_share`。

```shya
fn main() {
  let xs = [3, 1, 4]
  let bag = { a: 1, b: 2 }

  console log(xs @len)          // -> xs.length
  console log(bag @keys)        // -> Object.keys(bag)

  // 链式调用拆成一行，读起来像自然语言
  let d = @ts{{ say: (m) => { console.log(m); return { say: (x) => console.log(x) }; } }}
  d @safe say("你好") say("世界")
}
main()
```

Python 风格的宏库在 `lib/pystd.shya`，**复制到你的工作目录**（或用 `-I` 指定路径）后：

```shya
import { @enumerate, @sum, @sorted } from "./pystd.shya"

fn main() {
  let xs = [3, 1, 4]

  for i v of @enumerate(xs) {
    console log(i, v)
  }

  console log("和", xs @sum)
  console log("排序", xs @sorted)
}
main()
```

- 导入语法和普通模块**完全一样**，只是路径以 `.shya` 结尾。
- 编辑期加载、**不产出 JavaScript**。
- 具名导入会自动带上被依赖的宏，不用手写 `@any` 之类。

---

## 8. 出错时怎么读

```shya
fn main() {
  let hp: number = "满血"
}
main()
```

```
a.shya:2:19: error: 变量 `hp` 的初始值：期望 number，实际是 string [TC003]
  let hp: number = "满血"
                  ^
shya: check failed with 1 error(s)
```

格式是 `文件:行:列: 级别: 说明 [诊断码]`，下面带源码和插入符。诊断码分六族：

| 前缀 | 阶段 | 常见码 |
| --- | --- | --- |
| `LEX` | 词法 | `LEX009` 写了 `null`/`undefined`（shya 只有 `void`） |
| `SYN` | 语法 | `SYN001` 期望某个记号 |
| `MOD` | 宏文件导入 | `MOD001` 找不到宏文件、`MOD002` 循环导入 |
| `MAC` | 宏展开 | `MAC014` 未定义的宏、`MAC015` 插槽类型不符、`MAC020` `@when` 无分支匹配 |
| `TC` | 类型 | `TC003` 类型不符、`TC006` 参数个数、`TC010` 未声明标识符（**警告**） |
| `CGN` | 代码生成 | 一般是编译器内部错误 |

未声明的标识符只是警告，所以在 shya 里直接用宿主的全局量是没问题的。

---

## 9. 五条思维模型（省掉 90% 的困惑）

1. **换行就是语句分隔符**，分号可省。所以 `console log(1)` 换行 `console log(2)` 是两条语句；
   成员访问、宏的无括号插槽、后缀 `@宏` 都不跨行。
2. **一切皆是函数**：`player getHp` 就是 `player.getHp()`。
   要读真正的属性，用 `declare` 声明字段，或 `arr[i]`，或 `@ts{obj.prop}`。
3. **只有 `void`**：`null` 和 `undefined` 已经不存在了。
4. **`define` 在编译期就没了**，`let`/`const` 由编译器推断。
5. **宏在编译期展开**，产物是干净的 JS；`@ts{...}` 是你的逃生舱，里面的东西原样透传。

---

## 10. 下一步

| 想看什么 | 去哪 |
| --- | --- |
| 完整语法与语义 | [`shya-language-reference.md`](shya-language-reference.md) |
| AST 节点与宏插槽类型全表 | [`ast-nodes.md`](ast-nodes.md) |
| 从 Python 过来 | [`python-users.md`](python-users.md) |
| 编译器内部怎么跑 | [`compiler-architecture.md`](compiler-architecture.md) |
| 某个设计为什么是这样 | [`decisions.md`](decisions.md) |
| 完整可跑的例子 | `examples/hello.shya`、`examples/card-game.shya` |
| 语言特性的实测样例 | `tests/cases/*.shya`（配 `tests/expected/` 里的期望产物） |
