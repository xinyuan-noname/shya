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
| **Formatter** | `shya: 格式化文档`, or the standard **Format Document** (<kbd>Shift</kbd>+<kbd>Alt</kbd>+<kbd>F</kbd>) |
| **Format + preview** | `shya: 格式化并预览` (<kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>P</kbd>) — formats, then opens a live side panel |
| Compile command | `shya.compile` runs the compiler and writes `<file>.mjs` |

### Format + preview

`shya: 格式化并预览` does two things in one command:

1. **formats** the active file in place (undoable, one edit);
2. opens a **preview panel beside the editor** showing the generated JavaScript and the
   compiler's diagnostics. The panel refreshes ~400 ms after you stop typing, and again on
   save. Errors also land in the Problems panel with their shya codes (`TC003`, `MAC015`, …).

The formatter is deliberately **whitespace-only**:

- it re-indents each line from its brace/bracket depth;
- it collapses runs of spaces between tokens down to one, and drops the space before
  `,` `;` `)` `]`;
- it trims trailing whitespace and collapses blank-line runs;
- it **never joins two lines or splits one**, and never touches the inside of a string,
  template literal, comment or `@ts{ … }` block.

That restraint is not laziness — shya uses line breaks as statement separators, so a
formatter that reflowed code would change what the program means. The test suite proves it:
`test/formatter.test.mjs` formats every `.shya` file in the repository and requires the
compiled JavaScript to be **byte-identical** (see [Repackaging](#repackaging)).

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
code --install-extension shya-0.2.2.vsix
```

Or in VS Code: **Extensions** view → `…` menu → **Install from VSIX…** → pick
`shya-0.2.2.vsix`.

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
| `shya.indentSize` | `2` | Spaces per indentation level used by the formatter. |
| `shya.formatOnPreview` | `true` | Format before opening the preview. Turn off to preview the buffer exactly as typed. |
| `shya.previewLive` | `true` | Recompile the preview while you type. Turn off to refresh on save only. |

Example `settings.json`:

```json
{
  "shya.compilerPath": "D:\\project\\shya\\build\\shya.exe"
}
```

### Commands and keys

| Command | Key |
| --- | --- |
| `shya: 格式化并预览` | <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>P</kbd> (<kbd>Cmd</kbd>+<kbd>Alt</kbd>+<kbd>P</kbd>) |
| `shya: 格式化文档` | — (the standard Formatter action also works) |
| `shya: Compile File` | — |

All three are also in the editor context menu for `.shya` files.

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

The script also runs local checks before packaging:

- `grammar-smoke.mjs` — grammar shape (every pattern has `match`/`include`/`begin`, every
  `begin` has an `end`), `#include` resolution, regex compilation, ~60 token probes
  (math literals, `~/`, numbers, keywords, slots, `_`), and a stack-based dry run of the
  `@ts{ … }` region over the real files in `examples/` and `tests/cases/`;
- `test/extension.test.mjs` — loads `out/extension.js` against a stubbed VS Code API:
  activation, command and formatter registration, the exact `build <file> -o <file>.mjs`
  argument vector, the formatter's edits, the preview panel's HTML, the diagnostics it
  publishes, and the missing-compiler path;
- `test/formatter.test.mjs` — the formatter's whitespace rules, idempotency, what must stay
  verbatim, and a **round-trip over every real `.shya` file**: format it, compile both
  versions, and require byte-identical JavaScript (or the same diagnostic codes).

All three can also be run on their own:

```sh
node grammar-smoke.mjs ../examples/card-game.shya
node test/extension.test.mjs
node test/formatter.test.mjs
```

## Troubleshooting

### `shya: cannot write \`…\``

The preview and `shya.compile` both need the compiler to write a file, so this message means
the compiler process was not allowed to write where it was asked to. Work through these in
order — the first one is by far the most common, and it is **not** a permissions problem:

1. **Where the compiler executable lives.** If `shya.exe` sits inside a directory governed by
   a sandbox or restricted-execution policy (a container-mounted or agent-managed workspace,
   a folder under controlled-folder-access), a process started from there may be confined to
   writing back inside that same directory — while a copy of the identical binary placed
   elsewhere writes normally. Verified behaviour: the same `shya.exe` failed to write a file
   into a project folder from its original location, and succeeded immediately after being
   copied out. `shya.exe` is a single self-contained file, so the fix is simply to move it:

   ```powershell
   New-Item -ItemType Directory -Force C:\tools\shya
   Copy-Item D:\project\shya\build\shya.exe C:\tools\shya\shya.exe
   ```

   Then point `shya.compilerPath` at `C:\\tools\\shya\\shya.exe`. Confirm it directly with
   `C:\tools\shya\shya.exe build yourfile.shya` in the directory that was failing; if that
   writes `yourfile.mjs` next to the source, the extension will work too.

2. **The target directory really is read-only.** Check with an unrelated program:
   `cmd /c "echo x > probe.txt"`. If that fails too, it is a genuine permission problem —
   fix the folder's permissions instead.

3. **Security software.** Controlled folder access and some endpoint agents block writes from
   binaries they do not recognise. Allow `shya.exe`, or move it as in point 1.

The extension compiles a temporary copy next to your source file — never your actual file —
because `import "./x.shya"` resolves relative to the importing file. If the compiler cannot
write beside the source, it retries with its output in the system temp directory; if both
fail, the panel lists the two directories that were tried.

## License

MIT © xinyuan-noname
