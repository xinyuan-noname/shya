# shya for Visual Studio Code

Language support for **shya** — a strongly-typed rule-scripting DSL that compiles to
ES2026 JavaScript.

## What you get

| Feature | Notes |
| --- | --- |
| Syntax highlighting | TextMate grammar (`source.shya`) covering the whole surface syntax |
| Snippets | `fn`, `afn`, `macro`, `when`, `each`, `share`, `safe`, `for`, `forin`, `range`, `case`, `ifelse`, `declare`, `try`, `import`, `tsblock` |
| Bracket matching & auto-closing | `{}`, `[]`, `()`, `""`, `''`, `` `` ``, `/* */` |
| Folding | `{` … `}`, `(` … `)`, plus `// region` / `// endregion` markers |
| Indentation | Increases after an unclosed `{` or `(`, decreases on `}` / `)` |
| Compile command | `shya.compile` runs the compiler and shows its output |

### Highlighted syntax

`//` line comments and **nestable** `/* … */` block comments; `@ts{ … }` raw blocks
(scoped as embedded JavaScript); double-quoted, single-quoted and backtick template
strings with `${…}` interpolation; the math literals `~pi ~e ~tau ~inf ~lg10 ~ln4 ~db10
~deg2 ~rad2 ~sqrt2` and their Unicode spellings `~π ~ℯ ~τ ~∞` (including nesting such as
`~ln~deg360`); decimal / `1_000` / `1.5e-3` / `0x` / `0b` / `0o` numbers; all keywords;
the `_` placeholder; macro definitions (`macro @len(#x)`) and macro uses (`bag @keys`);
slot references `#name` and named slot labels `#name:`; the operators `~/ +/ -/ \/ ^ ==
!= === !== <= >= && || ! = += -= *= /= %= ^= ++ -- ... ? : ->`; function declarations
(`fn`, `async fn`, `export fn`); `declare` blocks; and type annotations after `:`.

`~/` — the truncating-division operator — is matched *before* the math literals, so it is
never highlighted as `~` + `/`.

## Install

### From the packaged `.vsix`

```sh
code --install-extension shya-0.1.0.vsix
```

Or in VS Code: **Extensions** view → `…` menu → **Install from VSIX…** → pick
`shya-0.1.0.vsix`.

### From source

Copy `vscode-shya/` into `~/.vscode/extensions/shya/` (or press <kbd>F5</kbd> with
`vscode-shya/` open as a folder to start an Extension Development Host), then reload the
window.

## Compiling

With a `.shya` file focused, run **shya: Compile File** from the Command Palette
(`Ctrl+Shift+P`), or bind `shya.compile` to a key of your choice. The command:

1. saves the active file if it is dirty;
2. runs `<shya.compilerPath> build <file> -o <file>.mjs` in the workspace root;
3. streams stdout/stderr to the **shya** output channel;
4. on failure, opens the compiler's diagnostics in a plain text panel (shya diagnostics
   are just text — the extension does not try to parse them into squiggles).

If the compiler cannot be started you get a message telling you exactly which path was
tried, so a wrong setting is easy to spot.

### Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `shya.compilerPath` | `shya` | Path to the compiler executable. Defaults to `shya` on `PATH`; on Windows you probably want `D:\\project\\shya\\build\\shya.exe`. |
| `shya.buildArgs` | `["build"]` | Sub-command passed before the input file. Use `["check"]` to type-check without emitting. |
| `shya.buildOutputExtension` | `.mjs` | Extension of the `-o` output file. |

Example `settings.json`:

```json
{
  "shya.compilerPath": "D:\\project\\shya\\build\\shya.exe"
}
```

## A short shya sample

```shya
define MAX = 100

fn describe(name: string, times: number): string {
  let out = name
  for i of @range 0:times,1 {
    out = out + "!"
  }
  return out
}

macro @len(#x) {
  @when(#x is array || #x is string) @ts{#x.length}
  @when(#x is map || #x is set) @ts{#x.size}
}

fn main() {
  let costs: array<number> = [1, 2, 3]
  console log(describe("shya", 3))
  console log(costs @len, MAX + MAX, ~pi, ~ln~e)
  console log(9 ~/ 2, 9 +/ 2, 9 -/ 2, 9 \/ 2, 2 ^ 10)
}
```

## Repackaging

```sh
node build-vsix.mjs
```

Uses `npx --yes @vscode/vsce package` when available and otherwise falls back to a
built-in, dependency-free ZIP writer (Node's `zlib`), then verifies the archive by reading
every entry back out.

The script also runs two local checks before packaging:

- `grammar-smoke.mjs` — grammar shape (every pattern has `match`/`include`/`begin`, every
  `begin` has an `end`), `#include` resolution, regex compilation, ~60 token probes
  (math literals, `~/`, numbers, keywords, slots, `_`), and a stack-based dry run of the
  `@ts{ … }` region over the real files in `examples/` and `tests/cases/`;
- `extension-smoke.mjs` — loads `out/extension.js` against a stubbed VS Code API and
  checks activation, command registration, the exact `build <file> -o <file>.mjs` argument
  vector, the missing-compiler path and the diagnostics panel.

Both can also be run on their own:

```sh
node grammar-smoke.mjs ../examples/card-game.shya
node extension-smoke.mjs
```

## License

MIT © xinyuan-noname
