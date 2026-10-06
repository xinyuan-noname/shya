# Python 用户看 shya

> 给会写 Python、要读或改 shya 规则脚本的人。文中的每条 shya 写法都在
> `build\shya.exe` 上跑过，输出贴的是真实结果。
> 相关文档：[`shya-language-reference.md`](shya-language-reference.md)（语言参考）、
> [`ast-nodes.md`](ast-nodes.md)（宏与 AST 插槽类型）。

---

## 1. 一句话定位

**shya 编译成 ES2026，没有解释器、没有运行时。** 你写的每个 `@macro` 在编译期展开成
原生 JavaScript，产物里只有 JS：没有宏、没有 `define`、没有 `declare`、没有辅助运行时。
`build\shya.exe build a.shya` 得到 `a.mjs`，`node a.mjs` 直接跑——中间不会再加载任何 shya。

看齐 Python 的地方：**换行是语句分隔符**（分号可省），代码块用 `{}`，读起来仍是一行一句；
**链式比较**一致（`1 < x < 10` 即 `(1 < x) && (x < 10)`，中间操作数只求值一次）；
**序列/字符串/内置函数**几乎都有对应的宏（`@len`、`@sum`、`@sorted`、`@upper`…，
集中在 `lib/pystd.shya`）；**空值只有一个** `void`。

明显不同的地方：**静态类型**（`TC***` 诊断，`any` / `unknown` 是逃逸舱）；
**一切皆是函数**（`player hp` 编译成 `player.hp()`，属性读取要靠 `declare` 或 `@ts`）；
**没有类**（宿主对象用 `declare` 声明形状）；**没有位运算**（`^` 是乘方）；
**`/` 之外还有四种取整除法**（`//` 要写成 `-/`，不是 `~/`）；**宏是编译期的**，
能按实参的静态类型分派（`@when`），这在 Python 里没有对应物。

---

## 2. 逐条对照表

### 2.1 值与字面量

| Python | shya | 说明 |
| --- | --- | --- |
| `None` | `void` | **`null` / `undefined` 已从语言移除，只保留 `void`**；写 `null` 报 `LEX009`（`null` 已从语言中移除，请改写为 `void`），产物统一是 `undefined` |
| `True` / `False` | `true` / `false` | 小写 |
| `1` `1.5` `1_000` `0xff` `0b101` | 同左 | 千分位 `_`、`0x`/`0b`/`0o` 都支持 |
| `"text"` `'text'` | 同左 | **转义不解析**：`"a\nb"` 产物是 `"a\\nb"`，要真换行请用模板字符串里直接断行 |
| `float("inf")` / `math.pi` | `~inf` / `~pi` | 数学字面量，编译期折叠；还有 `~e` `~tau` `~lgN` `~lnN` `~dbN` `~degN` `~radN` `~sqrtN` |
| `x and y` / `x or y` | `x && y` / `x \|\| y` | 没有 `and` / `or` 关键字（写 `x and y` 会编译成 `x.and(y)`） |
| `not x` | `!x`，或 `not x` | `not` 也支持，但它是**一元运算符**：`not (a == b)` 合法，`not(a == b)` 会被当成函数调用 |
| `x is None` / `x is not None` | `x is void` / `x is not void` | 产物 `x === undefined`；词序变体 `x not is void` 也对 |
| `isinstance(x, C)` | `x instanceof C` | `not instanceof` 同样支持 |
| `type(x) is int` | `x is number` | 只认 `array` `string` `number` `boolean` `fn` `map` `set` `object` `void` |
| `s[i]` / `s[-1]` | `s[i]` / `s @last` | 切片没有语法，用 `@ts{s.slice(1, 3)}` |

### 2.2 运算符

| Python | shya | 说明 |
| --- | --- | --- |
| `a + b` `a - b` `a * b` `a / b` `a % b` | 同左 | `%` 语义不同，见 §3.1 |
| `a // b` | `a -/ b` | **向下取整**：`(-7) -/ 2` = `-4`，`7 -/ 2` = `3`，产物 `Math.floor(a / b)` |
| —— | `a ~/ b` | 向零截断：`(-7) ~/ 2` = `-3`，产物 `Math.trunc(a / b)` |
| —— | `a +/ b` | 向上取整：`Math.ceil` |
| —— | `a \/ b` | 四舍五入：`Math.round` |
| `a ** b` | `a ^ b` | `^` 是乘方，右结合；`2 ^ 10` = `1024` |
| `a == b` / `a != b` | 同左 | **只有严格相等**，产物分别是 `===` / `!==`（`===` `!==` 也能写） |
| `1 < x < 10` | 同左 | 链式比较，`&&` 连接；中间操作数有副作用时只求值一次 |
| `a & b` `a \| b` `a << b` `a >> b` | **没有** | 完全没有位运算 |
| `~a`（按位取反） | **没有** | `~` 是数学字面量前缀（`~pi`）；`^` 已经被乘方占用 |
| `a if c else b` | `c ? a : b` | 三元 |
| `a += 1` `a -= 1` | 同左 | 复合赋值还有 `*=` `/=` `%=` `^=`（`x ^= 2` 产物 `x **= 2`） |
| —— | `i++` `i--` `++i` `--i` | 自增自减，**只能作为语句**，不构成表达式 |

### 2.3 容器与遍历

| Python | shya | 说明 |
| --- | --- | --- |
| `xs = [1, 2]` | `xs = [1, 2]` | 数组 |
| `d = {"a": 1}` | `d = @ts{{ a: 1 }}` | `{ a: 1 }` 单独写会被当成块，对象字面量要用 `@ts{ ... }` 包住 |
| `set()` | `new Set()` | `new` 支持 |
| `for x in xs:` | `for x of xs { }` | 遍历用 `of`，不是 `in` |
| `for i in range(10):` | `for i of @range 0:10,1 { }` | **含首不含尾**，写法是 `start:end,step`；步长可省 |
| `for i in range(10, 0, -1):` | `for i of @range 10:0,-1 { }` | 倒序 |
| `for i, v in enumerate(xs):` | `for i v of @enumerate(xs) { }` | 两个循环变量表示解构 `[i, v]` |
| `for a, b in zip(xs, ys):` | `for a b of @zip(xs, ys) { }` | `@zip` 按**较短**语义取值（越界一侧得 `undefined`） |
| `for k, v in d.items():` | `for k v of @items(d) { }` | `@items` 是 `Object.entries`，**只对普通对象有效，Map 会得到空数组** |
| 同上（内建写法） | `for k v of d @entries { }` | `@entries` 是内建宏，同样是 `Object.entries`，同样只认普通对象 |
| `for k in d:` / `for v in d.values():` | `for k of @keys(d) { }` / `for v of @values(d) { }` | `@keys` / `@values` 是内建宏，**不能从 pystd 导入** |
| `while cond:` | `for cond { }` | 条件循环的唯一写法，产物是 `while` |
| `len(xs)` / `len(s)` | `xs @len` / `s @len` | array / string → `.length` |
| `len(m)` / `len(s)`（map / set） | `m @len` / `s @len` | map / set → `.size` |
| `xs.append(v)` | `xs push(v)` | 成员访问即调用，**没有点号** |
| `xs[0]` `m[k]` `xs[-1]` | 同左 / `xs @last` | 下标用 `[]`；`@first` / `@last` 是 pystd 额外给的 |
| `x in xs` | **没有 `in` 运算符** | shya 里 `in` 根本不是关键字（遍历用 `of`）。见下面三行 |
| `sub in s` | `s @find(sub) >= 0` | pystd **没有 `@contains`**，不要用它。`@find` 就是 `indexOf`，找不到返回 `-1` |
| `x in xs`（数组） | `xs @find(x) >= 0` | `@find` 声明成 `(#s, #sub)`，实参不做类型校验，数组同样能跑：`[10, 20] @find(20)` 得 `1` |
| 列表推导 `[f(x) for x in xs]` | **没有** | `@ts{xs.map(f)}` 或显式 `for` 循环 + `push` |
| 生成器 / `yield` | **没有** | `yield` 不是关键字，会被当成普通标识符，可能编译通过但行为不对 |
| 解包 `a, b = b, a` | `a, b = b, a` | 多重赋值，产物 `[a, b] = [b, a]` |
| `a, b = 1, 2` | 同左 | 两个新名字 → 两条 `const` |

### 2.4 字符串

| Python | shya | 说明 |
| --- | --- | --- |
| `f"{name}: {hp}"` | `` `${name}: ${hp}` `` | 模板字符串，`${}` 里是 shya 表达式 |
| `s.upper()` / `s.lower()` / `s.strip()` | `s @upper` / `s @lower` / `s @strip` | 后缀宏：`s` 自动成为第一个参数 |
| `s.split(sep)` | `s @split(sep)` | `s @split(",")` |
| `s.replace(a, b)` | `s @replace(a, b)` | 内部是 `replaceAll`，**全部替换**，与 Python 一致 |
| `s.startswith(p)` / `s.endswith(q)` | `s @startswith(p)` / `s @endswith(q)` | |
| `s.find(sub)` | `s @find(sub)` | 找不到返回 `-1`，与 Python 一致 |
| `sep.join(xs)` | `xs @join(sep)` | **参数顺序反了**：集合在前，分隔符在后 |
| `"a" + "b"` | `"a" + "b"` | 拼接；`s * 3` 没有对应写法，用 `@ts{s.repeat(3)}` |

### 2.5 内置函数

| Python | shya | 说明 |
| --- | --- | --- |
| `sum(xs)` | `xs @sum` | 用 `reduce` 实现，**空序列返回 0** |
| `max(xs)` / `min(xs)` | `xs @max` / `xs @min` | 展开成 `Math.max(...xs)` / `Math.min(...xs)`；空序列得 `-Infinity` / `Infinity` |
| `sorted(xs)` | `xs @sorted` | 返回**新序列**，不改原序列；默认升序，没有 `key=` / `reverse=` |
| `reversed(xs)` | `xs @reversed` | 内部 `toReversed()` |
| `any(xs)` / `all(xs)` | `xs @any` / `xs @all` | `some(Boolean)` / `every(Boolean)`；空序列分别得假 / 真，与 Python 一致 |
| `abs(n)` / `round(n)` | `n @abs` / `n @round` | `@round` 是 **`Math.round`**：`.5` 一律向上，`round(0.5)` shya 得 `1`、Python 得 `0` |
| `int(n)` / `float(s)` | `n @int` / `s @float` | `int()` 是向零截断（`Math.trunc`），与 Python 一致 |
| `str(x)` / `bool(x)` | `x @str` / `x @bool` | `String(x)` / `Boolean(x)` |
| `pow(a, b)` | `a @pow(b)` | |
| `a % b`（Python 语义） | `a @mod(b)` | 见 §3.1 |
| `repr(x)` / `type(x).__name__` | `x @repr` / `x @typeOf` | `JSON.stringify(x)`（`Map`/`Set` 得 `{}`）/ `typeof` |
| `print(x)` | `console log(x)` | 宿主全局，成员访问即调用 |
| `len(xs)` 的另一种写法 | `xs @count` | pystd 额外提供（`Array.from(xs).length`） |
| `min(max(n, lo), hi)` | `n @clamp(lo, hi)` | pystd 额外提供 |
| `list(set(xs))` | `xs @unique` | pystd 额外提供，返回新数组 |

### 2.6 语句与结构

| Python | shya | 说明 |
| --- | --- | --- |
| `if c:` `elif` `else:` | `if c { }` `elif c { }` `else { }` | 圆括号可省可写 |
| `x = 1` | `x = 1` | 省略 `let`/`const`，编译器推断：**只赋值一次 → `const`，被再次赋值 → `let`** |
| `x = 1` 后再 `x = 2` | 同上 | 第二条赋值让整个名字变成 `let` |
| `x: int = 1` | `x: number = 1` | 显式标注也合法（`int` 是 `number` 的别名） |
| `x = 1; x = 2`（想改） | `let x = 1` | 想强制可变就写 `let` |
| —— | `const x = 1` + 再赋值 | 报错：`` `x` 是 const 常量，不能被重新赋值 [TC013] `` |
| `pass` | `_` | `_` 是待完成语句，什么都不生成 |
| 空语句 | `;` | `case` 分支里常用 |
| `def f(a, b=1):` | `fn f(a, b = 1) { }` | 参数默认值**不要求放在最后** |
| `async def f():` | `async fn f() { }` | `await` 同样支持 |
| `def f(*args):` | `fn f(...rest: number[]) { }` | 不定参数要写类型 `T[]` |
| `def f(**kw):` | **没有** | 没有关键字参数收集；宏倒是有具名插槽，但那是编译期的 |
| `lambda x: x + 1` | `fn (x) { return x + 1 }` | 匿名 `fn` 是表达式 |
| `yield` | **没有** | 见上 |
| `class C:` | **没有类** | 宿主对象用 `declare` 声明（见 §2.7） |
| `try: except E as e: finally:` | `try { } catch e { } finally { }` | `catch (e)` 带括号也行；没有 `except E` 的类型过滤，要自己 `if e instanceof TypeError` |
| `raise ValueError("x")` | `throw new Error("x")` | shya 没有异常类层次，通常 `throw new Error(...)` 或 `@ts{new TypeError(...)}` |
| `with open(f) as h:` | **没有** | 用 `try/finally` 显式关闭 |
| `match v:` `case 1 \| 2:` | `case v { 1, 2: ... }` | `case` 用逗号分隔多个模式 |
| `case _:` | `default:` | 默认分支 |
| `match` 的贯穿 | `fallthrough` | **每个分支默认 break**，写 `fallthrough` 才贯穿下一条 |
| `match` 的条件分支 `case _ if c:` | `case { c: ... }` | 省略主体按条件匹配，等价于 if/elif 链 |
| 分支体多行 | 分支体**恰好一条语句** | 多行请用块 `{ ... }` |
| `return` `break` `continue` | 同左 | |
| `import x` | `import { x } from "./m.mjs"` | 规则与 TypeScript 一致，原样透传到产物 |
| `from m import *` | `import * as ns from "./m.mjs"` | |
| —— | `import { @len } from "./pystd.shya"` | **宏文件也走同一套 import**，见 §5 |
| `global x` | **没有** | 没有全局声明语句；未声明标识符只报 warning `TC010` |
| `del x` | **没有** | 没有删除语句 |
| `assert c, "m"` | **没有** | pystd 提供 `@assertAny` / `@assertAll`（编译期展开成 `console.log`），见 §5 |
| 装饰器 `@dec` | **没有** | `@` 在 shya 里是宏调用前缀 |

### 2.7 类与宿主对象

shya 没有类。**宿主**（跑 shya 产物的那段 JS）提供的对象用 `declare` 声明形状；
`declare` 只参与类型检查，不产出任何 JavaScript：

```shya
declare Card {
  suit: string
  rank: number
}

declare Player {
  name: string              // 字段：编译成属性读取 p.name
  hp: number
  hand: array<Card>
  note?: string             // 可选字段
  judge(): Card             // 方法：编译成调用 p.judge()
  recover(n: number): void
  say(msg: string): Player
  draw(n: number = 1): void // 默认值只描述契约，编译期不注入
}

declare fn hostRandom(max: number): number
```

| 声明成 | 写法 | 编译成 |
| --- | --- | --- |
| 字段 `name: T` | `p name` | `p.name`（**属性读取**） |
| 方法 `judge(): T` | `p judge` | `p.judge()`（**函数调用**） |
| 宿主函数 `declare fn` | `hostRandom(6)` | `hostRandom(6)`，参数个数与类型受检查 |

对照 `tests/cases/14-declare.shya`：

```shya
fn main() {
  let p: Player = @ts{makePlayer()}
  console log(p name, p hp)          // -> p.name, p.hp
  console log("手牌数", p hand @len)  // -> p.hand.length
  let c: Card = p judge              // -> p.judge()
  p recover(2)                       // -> p.recover(2)
  for card of p hand {               // -> for (const card of p.hand)
    console log(card suit, card rank)
  }
}
```

访问没声明的成员只报 **warning** `TC014`，并提示补声明的位置；`p recover("oops")` 报 `TC003`。
没有 `declare` 时，`p name` 一律编译成 `p.name()`。

---

## 3. 三个语义陷阱

### 3.1 `%` 的符号

JS / shya 的 `%` **结果符号跟随被除数**，Python 跟随除数。负数时结论相反：

```shya
console log(-7 % 3)          // -1
console log((-7) @mod(3))    // 2
console log(7 % 3)           // 1
```

`@mod(a, b)` 的定义就是 `((a % b) + b) % b`，即 Python 语义。

**必须写 `(-7) @mod(3)`，不能写 `-7 @mod(3)`。** 后缀宏的优先级比一元负号高，
所以 `-7 @mod(3)` 解析成 `-(7 @mod(3))`：

```shya
console log((-7) @mod(3))    // 2
console log(-7 @mod(3))      // -1       即 -(7 % 3) = -(1)
```

实测（`tests/cases/12-py-macros.shya` 第 41 行就是这么写的）：

```
mod paren 2
mod noparen -1
jsmod -1
```

### 3.2 一切皆是函数

shya 里 `a b` 就是 `a.b()`。**没有属性这个概念**：

```shya
player getHp                 // -> player.getHp()
player nextSeat nextSeat     // -> player.nextSeat().nextSeat()
arr length                   // -> arr.length()   ← 大概率不是你想要的
```

要读真正的属性，只有三条路：

1. **用 `declare` 声明的字段**（上面的 `p name` → `p.name`）；
2. **下标**：`arr[0]`、`map[key]`；
3. **`@ts` 桥**：`@ts{obj.prop}`，或标准库宏（`arr @len` → `arr.length`，`bag @keys`）。

具体后果：

```shya
let o = { a: 1 }
console log(o a)             // -> console.log(o.a())  ← TypeError: o.a is not a function
console log(@ts{o.a})        // 1
console log(o["a"])          // 1
```

**已知不一致（模板字符串里的字段）**：`declare` 字段在普通表达式里编译成属性读取，
但在模板字符串的 `${}` 里会被编译成调用。实测：

```shya
declare Player { name: string }

console log(p name)          // -> console.log(p.name)        正确
console log("" + p name)     // -> console.log("" + p.name)   正确
console log(`x ${p name}`)   // -> console.log(`x ${p.name()}`)  ← 会抛 TypeError
```

模板字符串里读 `declare` 字段请先取到变量，或直接用 `@ts`：

```shya
let n = p name
console log(`x ${n}`)
console log(`y ${@ts{p.name}}`)
console log(`z ${p["name"]}`)
```

### 3.3 换行是语句分隔符

分号可省，**换行就是语句边界**：`console log(1)` 换行 `console log(2)` 是两条语句，
而不是 `console.log(1).log(2)`。由此产生三条不能折行的规则：

1. **成员访问不跨行**。`a` 换行 `b` 是两条语句，不是 `a.b()`。
2. **宏的无括号插槽参数不跨行**。`x @safe a(1)` 换行 `b(2)` 时，第二行是新语句。
3. **`@each` / `@when` 拼接线不跨行**。宏模板里 `#x @each(...)` 必须在同一行。

所以不能像 Python 那样把长表达式拆行：

```python
total = (a + b
         + c)
```

```shya
total = a + b
      + c            // ← 这是新语句，语法错误

total = a + b + c    // 正确：写在一行，或用括号包住
```

长逻辑折行请拆成中间变量，或把整段放进 `fn`。

---

## 4. 一个完整的对照示例

shya 版用 `build\shya.exe run` 跑过，输出在下面。

```python
def report(scores):
    rows = []
    for name, score in scores.items():
        if score < 60:
            rows.append(f"{name}: {score}")
    return "\n".join(sorted(rows))

print(report({"ann": 92, "bob": 55, "cid": 41}))
print("total", sum([92, 55, 41]))
```

```shya
import { @items, @sorted, @join, @sum } from "../../lib/pystd.shya"

declare Player {
  name: string
  hp: number
}

fn report(scores: object): string {
  let rows: array<string> = []
  for name score of @items(scores) {
    if score < 60 {
      rows push(`${name}: ${score}`)
    }
  }
  return rows @sorted @join(`
`)
}

fn main() {
  let scores = @ts{{ ann: 92, bob: 55, cid: 41 }}
  console log(report(scores))
  console log("total", [92, 55, 41] @sum)

  let p: Player = @ts{{ name: "ann", hp: 3 }}
  let who = p name
  let life = p hp
  console log(`${who} (${life} HP)`)
}

main()
```

实测输出：

```
bob: 55
cid: 41
total 188
ann (3 HP)
```

不显然的四处：

| Python | shya | 为什么 |
| --- | --- | --- |
| `dict[str, int]` | `object` | `@items` 展开成 `Object.entries`，只认普通对象；标成 `map<K,V>` 会得到空数组 |
| `"\n".join(rows)` | `rows @join(换行模板字符串)` | 参数顺序相反；`"\n"` 不解析转义，换行要写在模板字符串里 |
| `rows.append(x)` | `rows push(x)` | 成员访问即调用，没有点号 |
| （无对应写法） | `declare Player { name: string }` | Python 不需要声明；shya 靠它把 `p name` 变成属性读取 |

---

## 5. 导入宏库的最小步骤

`lib/pystd.shya` 是普通文件。步骤：**复制到自己的项目**（比如项目根的 `lib/pystd.shya`；
宏导入路径相对**当前源文件**解析，所以复制比引用稳妥）；然后**写具名导入**：

```shya
import { @enumerate, @zip, @items, @sum, @max, @min, @sorted, @reversed, @count } from "../../lib/pystd.shya"
import { @upper, @strip, @split, @join, @mod, @assertAny } from "../../lib/pystd.shya"
import { @unique, @clamp, @str, @repr } from "../../lib/pystd.shya"
```

（这就是 `tests/cases/12-py-macros.shya` 的三行。）要用全部宏时可以整文件导入：
`import "./pystd.shya"`。

**具名导入会自动带上被依赖的宏。** `@assertAny` 的模板体用到了 `@any`：

```shya
macro @assertAny(#seq, #message: strLit) {
  if (@any(#seq)) {
    console log("通过：" + #message)
  } else {
    console log("失败：" + #message)
  }
}
```

所以只写 `import { @assertAny } from "../../lib/pystd.shya"` 就够了，`@any` 会自动注册。
两点注意：`#message: strLit` 要求**字符串字面量**，传变量报
`` 宏 `@assertAny` 的参数 `#message` 需要 AST 节点 `strLit`，但传入的是 `Ident` [MAC015] ``；
而它们展开成 `console.log`，**不是真的断言**，失败不会中断程序。

**哪些宏不能从 pystd 导入。** `@keys`、`@values`、`@entries`、`@len`、`@safe`、`@share`、
`@safe_share` 是**编译器内建**的标准库宏（`src/stdlib.cpp` 的 `kStdlibSource`），
开箱即用，不在 `pystd.shya` 里。实测两种导入后果：

```
error: 宏文件 `…/pystd.shya` 里没有宏 `@contains` [MOD005]
error: 宏文件 `…/pystd.shya` 里没有宏 `@len` [MOD005]
```

所以 `@keys` / `@values` / `@entries` / `@len` **直接使用**；`@contains` 不存在，
包含检查用 `@find(s, sub) >= 0`。

---

## 6. 无法一一对应的 Python 惯用法

遇到下面这些请改设计，而不是硬翻：

| Python | 现状 |
| --- | --- |
| 列表/字典/集合推导式 | 没有。用 `@ts{xs.map(...)}`、`@ts{xs.filter(...)}` 或显式 `for` + `push` |
| 生成器、`yield`、`itertools` | 没有。`yield` 不是关键字，写了可能编译通过但语义不对 |
| 切片 `xs[1:3]`、`xs[::-1]` | 没有语法。用 `@reversed`、`@ts{xs.slice(1, 3)}` |
| 类、继承、`@property`、`__init__` | 没有类。宿主对象用 `declare` 声明形状，行为由宿主 JS 提供 |
| `with` 上下文管理器 | 没有。用 `try/finally` |
| `except ValueError as e` 类型过滤 | `catch` 不筛类型，要自己 `if e instanceof TypeError` |
| `**kwargs`、`functools.wraps`、装饰器 | 没有。宏（`@名字`）是编译期模板，不是函数包装 |
| `match` 的结构化模式（`case {"k": v}`） | 只有值匹配与条件匹配，没有解构模式 |
| `dict` 推导 / `defaultdict` / `Counter` | 没有。`Map` 要 `@ts{new Map()}`，注意 `@items` 不认 Map |
| `enumerate(xs, start=1)` | `@enumerate(seq)` 只有单参数，起点固定 0 |
| `zip(xs, ys, ...)` 三个以上序列、`strict=` | `@zip` 只接受两个序列，按较短语义 |
| `sorted(xs, key=..., reverse=True)` | `@sorted` 单参数，无 `key` / `reverse` |
| `max(xs, key=...)` / `min` 多参数形式 | `@max` / `@min` 只接受一个序列 |
| `round(x, ndigits)` / `str.format` / `%` 格式化 | 都没有；格式化只有模板字符串 |
| `assert` / 单元测试框架 | 只有 `@assertAny` / `@assertAll`，它们打印日志而不中断 |
| 可变默认参数的行为差异 | 参数可省且不要求放最后，但**默认值不会由编译器注入到宿主方法调用点** |
| `is` 身份比较 | `is` 在 shya 里是**类型判断**，不是身份比较 |
| `global` / `nonlocal` / `del` | 都没有对应语句 |
| 多行表达式（隐式续行） | 没有。见 §3.3 |
| `try/except/else` | 只有 `try/catch/finally`，没有 `else` 子句 |
| 三元嵌套 `a if c else (d if e else f)` | 支持 `c ? a : e ? d : f`，但不能折行 |
