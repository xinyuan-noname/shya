// shya formatter tests.
//
//   node vscode-shya/test/formatter.test.mjs
//
// Two layers:
//   1. unit tests for the whitespace rules and for what must stay verbatim;
//   2. a round-trip test over every real .shya file in the repository: format it,
//      compile both versions, and require byte-identical JavaScript (or the same
//      set of diagnostic codes for files that are meant to fail).
//
// Child processes write straight into files (never pipes) so this also runs under a
// sandbox that forbids piped stdio.
import { spawnSync } from "node:child_process";
import {
  closeSync, existsSync, mkdirSync, openSync, readFileSync, readdirSync, rmSync, statSync,
  writeFileSync,
} from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const here = dirname(fileURLToPath(import.meta.url));
const extDir = resolve(here, "..");
const root = resolve(extDir, "..");
const { formatShya, tokenize } = require(join(extDir, "out", "formatter.js"));

const exe = join(root, "build", process.platform === "win32" ? "shya.exe" : "shya");
const tmpDir = join(root, "tests", ".tmp-fmt");

let failures = 0;
let checks = 0;

function ok(name, condition, detail) {
  checks += 1;
  if (condition) {
    console.log(`  ok    ${name}`);
  } else {
    failures += 1;
    console.log(`  FAIL  ${name}`);
    if (detail) console.log(String(detail).replace(/^/gm, "        "));
  }
}

function eq(name, actual, expected) {
  ok(name, actual === expected, `expected:\n${JSON.stringify(expected)}\nactual:\n${JSON.stringify(actual)}`);
}

// ---------------------------------------------------------------- unit tests ---

console.log("unit:");

eq("re-indents a nested block", formatShya("fn f() {\nlet x = 1\nif x {\nconsole log(x)\n}\n}\n"), [
  "fn f() {",
  "  let x = 1;".replace(";", ""),
  "  if x {",
  "    console log(x)",
  "  }",
  "}",
  "",
].join("\n"));

eq(
  "collapses runs of spaces between tokens",
  formatShya("let   x    =     1\n"),
  "let x = 1\n",
);

eq("drops the space before , ; ) ]", formatShya("f(a , b )\nlet xs = [1, 2 ]\n"), "f(a, b)\nlet xs = [1, 2]\n");

eq("keeps the space before }", formatShya("let o = { a: 1 }\n"), "let o = { a: 1 }\n");

eq("trims trailing whitespace", formatShya("let x = 1   \nlet y = 2\t\n"), "let x = 1\nlet y = 2\n");

eq("collapses blank-line runs to one", formatShya("let a = 1\n\n\n\n\nlet b = 2\n"), "let a = 1\n\nlet b = 2\n");

eq("always ends with exactly one newline", formatShya("let a = 1"), "let a = 1\n");

eq("CRLF becomes LF", formatShya("let a = 1\r\nlet b = 2\r\n"), "let a = 1\nlet b = 2\n");

// --- things that must stay verbatim

eq(
  "does not touch the inside of a string",
  formatShya('let s = "a   b  c"\n'),
  'let s = "a   b  c"\n',
);

eq(
  "does not touch the inside of a template literal",
  formatShya("let s = `line1\n    line2   \n`\n"),
  "let s = `line1\n    line2   \n`\n",
);

eq(
  "does not touch the inside of an @ts block",
  formatShya("let x = @ts{{\n    a:   1,\n        b: 2\n}}\n"),
  "let x = @ts{{\n    a:   1,\n        b: 2\n}}\n",
);

eq(
  "does not touch the inside of a block comment",
  formatShya("/*\n   raw   text\n*/\nlet a = 1\n"),
  "/*\n   raw   text\n*/\nlet a = 1\n",
);

eq(
  "keeps `#y #slot` juxtaposition intact",
  formatShya("macro @m(#x, #y) {\n#y #x\n}\n"),
  "macro @m(#x, #y) {\n  #y #x\n}\n",
);

// --- newlines are statement separators: never join or split

eq(
  "never joins two statements",
  formatShya("console log(1)\nconsole log(2)\n"),
  "console log(1)\nconsole log(2)\n",
);

eq(
  "never splits one statement",
  formatShya("let x = 1 + 2\n"),
  "let x = 1 + 2\n",
);

// --- idempotency

const messy = [
  "macro @twice(#body: stmt) {",
  "#body",
  "#body",
  "}",
  "",
  "declare Player {",
  "name: string",
  "judge(): Card",
  "}",
  "",
  "fn main() {",
  "let p: Player = @ts{globalThis.p}",
  "case p name {",
  '"red", "heart": console log("红")',
  "default: console log(`其它 ${1 + 1}`)",
  "}",
  "}",
  "",
].join("\n");

const once = formatShya(messy);
eq("is idempotent", formatShya(once), once);
ok("actually changed the messy input", once !== messy, once);

// --- tokenizer losslessness

{
  const src = 'macro @m(#x) {\n  @ts{a: 1}\n}\nlet s = "x   y" // c\n';
  const rebuilt = tokenize(src).map((t) => t.text).join("");
  // Whitespace is normalised, so compare with all whitespace squeezed out.
  const squeeze = (s) => s.replace(/\s+/g, "");
  eq("tokenizer keeps every non-whitespace byte", squeeze(rebuilt), squeeze(src));
}

// ------------------------------------------------------------ round-trip test ---

function collectShya(dir, out = []) {
  for (const entry of readdirSync(dir)) {
    if (entry === ".tmp" || entry === ".tmp-fmt" || entry === "node_modules") continue;
    const p = join(dir, entry);
    const st = statSync(p);
    if (st.isDirectory()) collectShya(p, out);
    else if (entry.endsWith(".shya")) out.push(p);
  }
  return out;
}

function runTo(program, argv, capture) {
  const fd = openSync(capture, "w");
  let r;
  try {
    r = spawnSync(program, argv, { stdio: ["ignore", fd, fd] });
  } finally {
    closeSync(fd);
  }
  return { status: r.status, error: r.error, text: existsSync(capture) ? readFileSync(capture, "utf8") : "" };
}

/** Diagnostic codes only — positions legitimately shift when indentation changes. */
function codes(text) {
  return (text.match(/\[([A-Z]{3}\d{3})\]/g) || []).sort().join(",");
}

if (!existsSync(exe)) {
  console.log(`\nskipping round-trip: compiler not found at ${exe} (run build.bat)`);
} else {
  rmSync(tmpDir, { recursive: true, force: true });
  mkdirSync(tmpDir, { recursive: true });

  const sources = [
    ...collectShya(join(root, "tests", "cases")),
    ...collectShya(join(root, "examples")),
  ].sort();

  console.log(`\nround-trip over ${sources.length} real .shya file(s):`);

  let compared = 0;
  let failedCompiles = 0;

  for (const src of sources) {
    const original = readFileSync(src, "utf8");
    const formatted = formatShya(original, { indentSize: 2 });
    const rel = src.slice(root.length + 1).replace(/[\\/]/g, "_");

    // The formatted copy must live next to the original so `import "./x.shya"` still resolves.
    const formattedPath = join(dirname(src), `.fmt-${rel.replace(/^.*?_/, "")}`);
    const aOut = join(tmpDir, `${rel}.a.mjs`);
    const bOut = join(tmpDir, `${rel}.b.mjs`);
    const aLog = join(tmpDir, `${rel}.a.log`);
    const bLog = join(tmpDir, `${rel}.b.log`);

    writeFileSync(formattedPath, formatted);
    try {
      const a = runTo(exe, ["build", src, "-o", aOut], aLog);
      const b = runTo(exe, ["build", formattedPath, "-o", bOut], bLog);

      if (a.status === 0 && b.status === 0) {
        compared += 1;
        const ja = readFileSync(aOut, "utf8");
        const jb = readFileSync(bOut, "utf8");
        // The banner carries the source file name, which differs by construction.
        const strip = (s) => s.replace(/^\/\/ Generated by.*$/m, "");
        ok(`identical JavaScript: ${rel}`, strip(ja) === strip(jb),
          `--- original ---\n${strip(ja)}\n--- formatted ---\n${strip(jb)}`);
      } else if (a.status !== 0 && b.status !== 0) {
        failedCompiles += 1;
        ok(`same diagnostics after formatting: ${rel}`, codes(a.text) === codes(b.text),
          `original codes: ${codes(a.text)}\nformatted codes: ${codes(b.text)}`);
      } else {
        ok(`compile status agrees: ${rel}`, false,
          `original exit ${a.status}, formatted exit ${b.status}\n${a.text}\n${b.text}`);
      }
    } finally {
      rmSync(formattedPath, { force: true });
    }
  }

  console.log(`  (${compared} file(s) compared by output, ${failedCompiles} by diagnostics)`);
  rmSync(tmpDir, { recursive: true, force: true });
}

console.log(`\n${checks - failures}/${checks} check(s) ok`);
if (failures > 0) process.exit(1);
