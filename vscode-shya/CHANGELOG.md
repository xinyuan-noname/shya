# Changelog

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
