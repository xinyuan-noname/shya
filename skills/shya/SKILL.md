---
name: shya
description: Write, review, and compile shya — a strongly-typed DSL that compiles to ES2026 JavaScript. Use when asked to author or modify .shya source, design shya macros, explain shya syntax/semantics, debug shya compiler diagnostics (LEX/SYN/MAC/TC/CGN), or work on the shya compiler itself. 中文触发：写 shya、改 shya、shya 语法、shya 宏、编译 shya、shya 报错、shya 编译器、设计稿 I0002。
---

# shya

**shya** 是一门强类型规则脚本语言，编译成 **ES2026** JavaScript。关键字：
最近自然语言的语法 + 宏做模板复用 + `@ts` 宿主桥 + 零运行时产物。

- 语言参考：[`references/language-reference.md`](references/language-reference.md)
- 编译器架构：[`references/compiler-architecture.md`](references/compiler-architecture.md)
- 设计决策记录：[`references/decisions.md`](references/decisions.md)

> 参考文件随 skill 一起安装；若缺失，直接向用户索取或查阅项目 `docs/` 目录。

## 先定位项目

shya 项目根目录里有 `build.bat`、`src/`、`docs/`、`tests/`、`examples/`。
不确定路径时先问用户，或用 `build.bat` / `build/shya.exe` 作为锚点判断当前目录是不是项目根。

## 常用命令

```sh
build.bat                        # MSVC 构建（需要 VS2022 C++ 工作负载，无第三方依赖）
build/shya.exe run   a.shya      # 编译并立刻用 node 运行
build/shya.exe build a.shya [-o out.mjs]
build/shya.exe check a.shya      # 只做类型检查
build/shya.exe ast   a.shya      # 语法树
build/shya.exe core  a.shya      # 宏展开后的核心语法树（调宏必用）
node tests/run.mjs               # 跑测试；--update 重新生成基准
```

调试宏时设 `SHYA_DEBUG_WHEN=1`，编译器会在 stderr 打印每次 `@when` 判定与宏展开的语句数。

## 写 shya 时最容易踩的 10 个坑

1. **换行就是语句分隔符**，分号可省。所以 `console log(1)` 换行 `console log(2)` 是两条语句。
   推论：**成员访问、宏的无括号插槽、`@each`/`@when` 拼接线都不跨行**。
2. **一切皆是函数**：`player getHp` 编译成 `player.getHp()`。要读真正的属性，用标准库宏（`arr @len`）
   或 `@ts{obj.prop}`。
3. **`==` 只有严格相等**（产物 `===`）；`^` 是乘方不是异或；**没有位运算**。
4. **`~/` 截断、`+/` 上取整、`-/` 下取整、`\/` 四舍五入**——四个除法各有含义。
5. **数学字面量** `~pi ~e ~ln4 ~deg360 ~db10 ~sqrt2` 在词法期折叠；常量不带参数、函数必须带参数，
   所以 `~dege` 是错的（要写 `~deg~e`）。
6. **`define` 是编译期常量**，产物里没有这个变量；初值必须是字面量/数学字面量/其它 define/它们的常量运算。
7. **`let`/`const` 可省**，编译器推断：只写一次赋值 → `const`，被重赋值 → `let`。显式 `const` 重赋值报错。
8. **宏的无括号尾部插槽是「单个调用片段」**，不是成员链：
   `x @share _p recover(2) draw(2)` 是 4 个参数（`x`、`_p`、`recover(2)`、`draw(2)`）。
   复杂表达式请用括号形式 `@macro(expr, expr)`。
9. **具名插槽块只在真的以 `#名字:` 开头时才算插槽**；否则那个 `{` 是 `for`/`if` 的语句体。
10. **`@when` 的类型判定是启发式的**（只看字面量初始化和显式标注）。判不出来会报 `MAC020`，
    此时补类型标注，或把该段改成 `@ts`。

## 核心语法速查

```shya
// 常量与变量
define DRAW = 2
let hp: number = 4
count = 3                    // 省略 let/const，自动推断

// 函数
fn describe(name: string, times: number = 1, ...more: number[]): string {
  let out = name
  for i of @range 0:times,1 { out = out + "!" }
  return out
}
async fn later(): number { await p; return 1 }

// 分支：圆括号可省；case 分支默认 break，fallthrough 显式贯穿
if hp < 1 { } elif hp < 3 { } else { }
case suit {
  "red", "heart": console log("红")
  "black": fallthrough
  default: console log("其它")
}

// 条件循环与遍历（只有这两种循环）
for n < 3 { n++ }
for p of players { }
for k v of @entries(map) { }
for i of @range 0:100,2 { }      // 含首不含尾
for v of arr @range 1:3,1 { }    // 集合上的区间

// 类型判断
x is array      // Array.isArray(x)
x is string     // typeof x === "string"
x is map        // x instanceof Map
x is Player     // x instanceof Player（宿主类型）

// 宿主桥
let d = @ts{{ say: (m) => console.log(m) }}
```

## 写宏

```shya
macro @len(#x) {                                  // 定义；#name 是插槽
  @when(#x is array || #x is string) @ts{#x.length}   // 编译期分支
  @when(#x is map || #x is set) @ts{#x.size}
}

macro @share(#x, #y, ...#slots: callExpr) {       // ...#slots 是不定项
  #y = #x
  @each(#slot of #slots) {                        // 编译期遍历
    #y #slot
  }
}

bag @keys                       // 后缀：bag 是第一个参数
@keys(bag)                      // 前缀：等价写法
x @judge_color { #red: ...  #black: ... }   // 具名插槽
_ @judge_color { #red: ... }                // 没有 target 时用 _ 占位
```

插槽类型：`expr`（默认）、`stmt`、`callExpr`、`safeCallExpr`、`rangeExpr`、`expr[]`、`type`。
`@ts{...}` 内的内容**原样透传**，只有 `#插槽` 会被替换。

标准库宏（内建，可用用户宏覆盖）：
`@keys` `@values` `@entries` `@len` `@safe` `@share` `@safe_share`。

## 诊断码

`文件:行:列: error/warning: 说明 [码]`，带源码片段与插入符。

| 前缀 | 阶段 |
| --- | --- |
| `LEX` | 词法（`LEX006` 数学字面量缺少参数…） |
| `SYN` | 语法（`SYN001` 期望记号、`SYN011` `#名字` 出现在宏外…） |
| `MAC` | 宏展开（`MAC014` 未定义宏、`MAC016` 缺参数、`MAC020` `@when` 无分支匹配…） |
| `TC` | 类型检查（`TC003` 类型不符、`TC006` 参数个数、`TC010` 未声明标识符=警告、`TC013` const 重赋值） |
| `CGN` | 代码生成（通常是编译器内部错误） |

未声明的标识符只是 **warning**；宿主全局量可放心使用。

## 改编译器时

- 纯 C++20、只依赖标准库、`/W4` 但不把警告当错误。
- 单头 `src/shya.h` 共享全部 AST 与接口；流水线在 `src/main.cpp` 的 `compileSource()`。
- 加语言特性时同步更新：`docs/shya-language-reference.md`、`docs/decisions.md` 的对应条目、
  以及 `tests/cases/` 下的用例（先 `node tests/run.mjs --update` 生成基准，再人工核对基准是否正确）。
- 改动后必须 `node tests/run.mjs` 全绿，并在 `examples/` 上跑一遍。

## 交付检查清单

- [ ] `build/shya.exe check <file>` 无 error（warning 需能解释）
- [ ] `build/shya.exe run <file>` 行为与需求一致
- [ ] 用了宏的地方 `build/shya.exe core <file>` 看一眼展开结果是否符合预期
- [ ] 新增/修改的用例已加入 `tests/cases/` 且 `node tests/run.mjs` 通过
- [ ] 文档（语言参考 / 决策记录）已同步
