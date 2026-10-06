# shya

**shya** 是一门强类型的规则脚本语言：用最接近自然语言的语法写规则逻辑，用**宏**做模板与复用，
用宿主桥拿底层能力，最终编译成 **ES2026** JavaScript 跑在任何地方。

本项目是 shya 的完整实现——编译器（词法分析 → 语法分析 → 模块加载 → 宏展开/脱糖 → 类型检查 →
代码生成）、命令行工具、标准库与 Python 风格宏库、VS Code 扩展、测试套件与文档。

> 语言设计与需求来自 `I0002-“shya”DSL设计稿.docx`。修订版正文见
> [`docs/design-draft.txt`](docs/design-draft.txt)，逐条改动见 [`docs/design-v2.md`](docs/design-v2.md)，
> 实现取舍见 [`docs/decisions.md`](docs/decisions.md)。

## 一分钟上手

```sh
build.bat                        # 用 MSVC 构建 build\shya.exe
build\shya.exe run examples/hello.shya
```

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
  let d = @ts{{ say: (m) => { console.log(m); return { say: (x) => console.log(x) }; } }}
  d @safe say("hello") say("nice")     // -> d?.say("hello")?.say("nice")
  console log(describe("shya", 3))
  console log(MAX + MAX, ~pi)
}

main()
```

输出：

```
shya!!!
200 3.141592653589793
hello
nice
```

## 命令行

```
shya build <file.shya> [-o out.js]   编译成 JavaScript
shya check <file.shya>               只做类型检查
shya run   <file.shya>               编译并立刻用 node 运行
shya tokens <file.shya>              打印词法单元流
shya ast    <file.shya>              打印语法树
shya core   <file.shya>              打印宏展开后的核心语法树
shya version
shya help
```

| 选项 | 作用 |
| --- | --- |
| `-o, --out <路径>` | 输出文件（默认与源文件同目录、同名 `.mjs`） |
| `-I, --include <目录>` | `.shya` 宏文件的额外搜索根 |
| `--no-typecheck` | 跳过类型检查 |
| `--warnings-as-errors` | 警告视为错误 |
| `-q, --quiet` | 只打印错误 |

环境变量 `SHYA_DEBUG_WHEN=1` 会在标准错误输出宏展开时每次 `@when` 的判定结果，
排查宏问题时很有用。

## 构建

需要 Visual Studio 2022（含「使用 C++ 的桌面开发」工作负载）。无第三方依赖，纯标准库。

```sh
build.bat          # Release
build.bat debug    # Debug
```

产物为 `build\shya.exe`。

## 语言速览

| 主题 | 一句话 |
| --- | --- |
| 空值 | **只有 `void`**；`null` / `undefined` 已移除（`@ts{}` 内的原始 JS 不受限） |
| 行分隔 | 换行就是语句分隔符，分号可选；成员访问、宏插槽、`@each/@when` 都不跨行 |
| 赋值 | `x = 1` 自动推断 `const`/`let`；复合赋值 `+= -= *= /= %= ^=` 只作语句 |
| 常量 | `define A = 100` 在编译期完全折叠，产物中没有这个变量 |
| 一切皆是函数 | `player getHp` 编译为 `player.getHp()`；宿主字段用 `declare` 声明后按属性读取 |
| 宿主对象 | `declare Player { hp: number  judge(): Card }` — 只做类型检查，不产出 JS |
| 数学字面量 | `~pi` `~ln4` `~deg360` `~db10` `~sqrt2`，可嵌套 `~ln~deg360`，词法期折叠 |
| 除法 | `~/` 截断、`+/` 上取整、`-/` 下取整、`\/` 四舍五入 |
| 比较 | 链式 `1 < 2 < 3`；`==` 只有严格相等；`is` / `is not` / `not is` / `not instanceof` |
| 分支 | `if/elif/else` 圆括号可省；`case` 分支默认 break，`fallthrough` 显式贯穿 |
| 循环 | 只有 `for(条件)` 与 `for x of ...`；区间用 `@range 起:止,步` |
| 类型 | TypeScript 风格标注；不能自定义类；检查是渐进式的 |
| 宏 | `macro @名字(#插槽: 类型) { ... }`，支持 `@when`/`@each` 编译期控制流与 `@ts` 原样透传 |
| 宏插槽类型 | 大部分**安全的 AST 节点种类**都能当类型：`#cond: compare`、`#b: block`、`#x: strLit` |
| 宏文件 | `import { @sum } from "./pystd.shya"` —— 与其它模块同一套 import，编译期加载、不产出 JS |
| 标准库 | `@keys` `@values` `@entries` `@len` `@safe` `@share` `@safe_share` |
| Python 宏库 | `lib/pystd.shya`：`@enumerate` `@zip` `@items` `@sum` `@sorted` `@mod` … |

完整说明见 [`docs/shya-language-reference.md`](docs/shya-language-reference.md)；
**第一次上手请看 [`docs/getting-started.md`](docs/getting-started.md)**（每段代码都跑过）。

## 示例

```sh
build\shya.exe run examples/hello.shya
build\shya.exe run examples/card-game.shya
```

`examples/card-game.shya` 把设计稿里的写法串成一个像真实规则的例子：
`define` 常量、`@share` / `@safe` 链式宏、`@judge_color` 具名插槽、`@len`、
区间遍历与数学字面量。

## 测试

```sh
node tests\run.mjs              # 跑全部用例
node tests\run.mjs 02-macros    # 只跑名字匹配的用例
node tests\run.mjs --update     # 重新生成期望产物
```

`tests/cases/*.shya` 是输入，`tests/expected/<名字>.{out,js,err}` 是期望的
运行输出 / 生成的 JS / 诊断信息。新增用例后先跑一次 `--update` 生成基准，再人工核对基准是否正确。
运行器还会编译 `examples/*.shya`，要求零警告。

| 用例 | 覆盖 |
| --- | --- |
| `01-core` | 设计稿核心语法：define、区间循环、链式比较、case/fallthrough、数学字面量、标准库宏 |
| `02-macros` | 设计稿三个链式宏示例（`@safe`/`@share`/`@safe_share`）与 `@judge_color` 具名插槽 |
| `03-macro-errors` | 宏层诊断：未定义宏、参数缺失/过多、未知具名插槽、非常量 define、`@when` 无分支匹配 |
| `04-mathlit` | 数学字面量的 ASCII 与 Unicode 两种写法 |
| `05-mathlit-error` | 设计稿点名 `~dege` 是错误写法 |
| `06-typecheck` | 类型检查器诊断：初始化类型不符、参数个数、运算符操作数、const 重赋值 |
| `07-misc` | 函数/默认参数/剩余参数、异步、异常、展开、模板字符串、`@range` 倒序与区间切片 |
| `08-doc-check` | 手册「编译产物对照表」的交叉核对：`is` 系列、多重赋值交换、链式比较的副作用提升 |
| `09-compound` | 复合赋值 `+= -= *= /= %= ^=` 与非末尾默认参数 |
| `10-typeops` | 四种类型判断运算符与混合链式比较 |
| `11-ast-types` | AST 节点作为宏插槽类型（`compare` / `strLit` / `block` / `@when(#x is binary)`） |
| `12-py-macros` | `.shya` 宏文件导入 + Python 风格宏库（含依赖闭包） |
| `13-module-errors` | 宏导入诊断：文件不存在、宏不存在、循环导入 |
| `14-declare` | `declare` 宿主对象：字段读属性、方法发调用、`declare fn` |
| `15-declare-errors` | `declare` 诊断：方法实参类型、宿主函数参数个数、未声明成员 |

## VS Code 扩展

`vscode-shya/` 是一个零依赖的 VS Code 扩展：语法高亮、代码片段、以及 `shya.compile` 命令
（用 `shya.compilerPath` 指定的编译器编译当前文件）。

```sh
cd vscode-shya
node build-vsix.mjs            # 产出 shya-0.2.2.vsix
code --install-extension shya-0.2.2.vsix
```

打包好的 `vscode-shya/shya-0.2.2.vsix` 直接随仓库提供，可手动安装。

扩展现在带**格式化器**和**格式化并预览**命令（<kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>P</kbd>）：
一个命令既格式化当前文件，又在侧边打开实时编译预览（生成的 JS + 诊断），输入时自动刷新。
格式化器是**纯空白改动**——shya 用换行分隔语句，所以它绝不合并或拆分行，也不碰字符串、
模板字符串、注释和 `@ts{…}` 内部；`node vscode-shya/test/formatter.test.mjs` 会把仓库里
全部 19 个 `.shya` 文件格式化后重新编译，要求产物**逐字节相同**。

### 排错：`shya: cannot write \`…\``

这条消息表示**编译器进程没被允许写文件**，不是编译器坏了。按顺序排查：

1. **编译器可执行文件放在哪**（最常见，且**不是权限问题**）。如果 `shya.exe` 位于某个被
   沙箱/受限策略管辖的目录里（容器挂载的工作区、agent 管理的目录、受控文件夹访问范围），
   从那里启动的进程可能只能写回该目录内部，写到别处一律被拒 —— 而**同一个文件复制到
   别处就完全正常**（本机实测：原位置的 `shya.exe` 写不进项目目录，复制出去后立刻成功）。
   `shya.exe` 是单文件、无依赖，复制即可用：

   ```powershell
   New-Item -ItemType Directory -Force C:\tools\shya
   Copy-Item D:\project\shya\build\shya.exe C:\tools\shya\shya.exe
   ```

   然后在 VS Code 设置里把 `shya.compilerPath` 指到 `C:\\tools\\shya\\shya.exe`。
   直接在出问题的目录里跑 `C:\tools\shya\shya.exe build 你的文件.shya` 验证一下即可。

2. **目标目录真的只读**：用别的程序试写，例如 `cmd /c "echo x > probe.txt"`。
   如果它也失败，那才是真的权限问题，去修目录权限。

3. **安全软件**：受控文件夹访问和某些终端防护会拦截它们不认识的二进制。
   放行 `shya.exe`，或按第 1 条把它挪个位置。

## 项目结构

```
build.bat                      MSVC 构建脚本（纯标准库，零第三方依赖）
src/
  shya.h                       总头文件：诊断、词法、AST、语法、宏、类型、代码生成
  lexer.cpp                    词法分析（数学字面量、@ts 原样块、换行标记、void 唯一性）
  parser.cpp                   语法分析 -> AST（含 declare、宏文件 import）
  modules.cpp                  .shya 宏文件加载（解析、依赖闭包、循环检测）
  macro.cpp                    宏展开 / 脱糖 -> 核心 AST，define 内联，AST 插槽类型表
  typecheck.cpp                类型系统、declare 结构类型、渐进式类型检查
  codegen.cpp                  核心 AST -> ES2026 JavaScript
  stdlib.cpp                   内建标准库（用 shya 自己写的宏源码）
  diag.cpp                     诊断渲染（源码片段 + 插入符，按 CJK 宽度对齐）
  main.cpp                     CLI 与编译流水线
lib/pystd.shya                 面向 Python 用户的宏库
vscode-shya/                   VS Code 扩展（源码 + 打包好的 .vsix）
examples/                      可运行示例
docs/
  getting-started.md           从零开始写第一个 shya 程序（每段代码都实测过）
  shya-language-reference.md   语言参考手册
  ast-nodes.md                 AST 节点与宏插槽类型全表
  python-users.md              Python -> shya 对照与语义陷阱
  compiler-architecture.md     编译器架构
  decisions.md                 设计决策记录（设计稿的歧义与取舍）
  design-draft.txt             设计稿 v2 正文
  design-draft-v1.txt          设计稿原文（溯源）
  design-v2.md                 v2 相对原文的逐条改动
skills/shya/SKILL.md           供 AI 助手使用的 shya 速查 skill
tests/                         用例、期望产物、测试运行器
```

## 设计理念

1. **自然语言优先**：`player nextSeat @share _p recover(2) draw(2)` 读起来就是「玩家下一个座位，记为 `_p`，恢复 2 点，摸 2 张」。
2. **零运行时**：产物不需要任何配套 runtime，宏全部在编译期展开成原生 JS。
3. **宏是唯一的复用机制**：标准库和 Python 宏库本身就是 shya 源码，语言的自举能力可验证；
   宏也能像普通模块一样 `import`。
4. **可读的产物**：`define` 内联、`const/let` 推断、字段读属性/方法发调用、保留缩进与空行，
   产物接近手写代码。
