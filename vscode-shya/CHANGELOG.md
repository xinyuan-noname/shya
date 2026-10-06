# Changelog

## 0.2.0

Formatter and a combined format-and-preview command.

- **Formatter** (`out/formatter.js`, dependency-free). Registered as a document formatting
  provider for `shya`, so <kbd>Shift</kbd>+<kbd>Alt</kbd>+<kbd>F</kbd> works, and exposed as
  `shya: 格式化文档`. It is deliberately whitespace-only: re-indents by brace depth, collapses
  runs of spaces, drops the space before `,` `;` `)` `]`, trims trailing whitespace and
  collapses blank-line runs — and never joins or splits a line, and never touches the inside
  of a string, template literal, comment or `@ts{…}` block, because shya uses line breaks as
  statement separators.
- **`shya: 格式化并预览`** (<kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>P</kbd>) — formats the active
  file, then opens a webview panel beside the editor with the generated JavaScript and the
  compiler's diagnostics. The panel refreshes ~400 ms after you stop typing (configurable)
  and on save.
- **Problems panel integration**: compiler diagnostics are parsed into a
  `DiagnosticCollection` with their shya codes, so errors show up as squiggles. Code matching
  handles both two- and three-letter prefixes (`TC003`, `LEX009`, `MOD001`).
- The preview compiles a temporary copy **in the original file's directory**, so
  `import "./x.shya"` still resolves; the copy is removed immediately and its path is never
  shown — the panel displays the real filename.
- New settings: `shya.indentSize`, `shya.formatOnPreview`, `shya.previewLive`.
- New context-menu entries for `.shya` files and a keybinding for the preview command.
- Tests: `test/extension.test.mjs` (29 checks against a stubbed VS Code API) and
  `test/formatter.test.mjs` (37 checks, including a round-trip that formats all 19 real `.shya`
  files and requires byte-identical compiled output). Replaces `extension-smoke.mjs`.

## 0.1.0 — 2026-01-01

Initial release.

- `source.shya` TextMate grammar: nestable block comments, `@ts{…}` raw blocks, all three
  string forms with `${…}` interpolation, math literals (`~pi` … `~ln~deg360`, plus
  `~π ~ℯ ~τ ~∞`), numeric literals (`1_000`, `1.5e-3`, `0x`/`0b`/`0o`), keywords, `_`
  placeholder, macro definitions and invocations, slots `#x` and slot labels `#x:`,
  operators (including `~/`, `+/`, `-/`, `\/`, `^`, `->`), function declarations,
  `declare` blocks and type annotations.
- Language configuration: comments, brackets, auto-closing pairs, surrounding pairs,
  `{}`/`()` folding, indentation rules and a Unicode-aware `wordPattern`.
- 16 snippets (`fn`, `afn`, `macro`, `when`, `each`, `share`, `safe`, `for`, `forin`,
  `range`, `case`, `ifelse`, `declare`, `try`, `import`, `tsblock`).
- `shya.compile` command driving the shya compiler, with an output channel and a plain
  text diagnostics panel. Settings: `shya.compilerPath`, `shya.buildArgs`,
  `shya.buildOutputExtension`.
- `build-vsix.mjs`: packages the extension with `@vscode/vsce` when reachable, otherwise
  with a built-in `zlib`-based ZIP writer.
