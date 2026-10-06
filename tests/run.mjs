// shya test runner.
//
//   node tests/run.mjs            run every case and report failures
//   node tests/run.mjs --update   regenerate the golden files
//   node tests/run.mjs 01-core    run cases whose name contains the filter
//
// A case is `tests/cases/<name>.shya`. Optional golden files live in
// tests/expected/<name>.{out,js,err}:
//   .out  expected combined output of the compiled program
//   .js   expected generated JavaScript (exact match)
//   .err  the case must fail to compile, and every line must appear in the output
//
// It also compiles every `examples/*.shya` and requires a warning-free `check`.
//
// Child processes write straight into files (never pipes) so the runner also
// works under a sandbox that forbids piped stdio.
import { spawnSync } from "node:child_process";
import {
  closeSync, existsSync, mkdirSync, openSync, readdirSync, readFileSync, rmSync, writeFileSync,
} from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "..");
const exe = join(root, "build", process.platform === "win32" ? "shya.exe" : "shya");
const casesDir = join(here, "cases");
const expectDir = join(here, "expected");
const tmpDir = join(here, ".tmp");

const args = process.argv.slice(2);
const update = args.includes("--update");
const filter = args.find((a) => !a.startsWith("-"));

if (!existsSync(exe)) {
  console.error(`shya: compiler not found at ${exe}\n      run build.bat first`);
  process.exit(2);
}
mkdirSync(expectDir, { recursive: true });
rmSync(tmpDir, { recursive: true, force: true });
mkdirSync(tmpDir, { recursive: true });

/** Runs `program argv...`, sending both streams to `capture`, returns its text. */
function run(program, argv, capture) {
  const fd = openSync(capture, "w");
  let result;
  try {
    result = spawnSync(program, argv, { stdio: ["ignore", fd, fd] });
  } finally {
    closeSync(fd);
  }
  const text = existsSync(capture) ? readFileSync(capture, "utf8") : "";
  return { status: result.status, error: result.error, text };
}

const cases = readdirSync(casesDir)
  .filter((f) => f.endsWith(".shya") && !f.startsWith("dbg-"))
  .filter((f) => !filter || f.includes(filter))
  .sort();

let passed = 0;
const failures = [];

function report(name, ok, detail) {
  if (ok) {
    passed++;
    console.log(`  ok    ${name}`);
  } else {
    failures.push({ name, detail });
    console.log(`  FAIL  ${name}`);
  }
}

const readIf = (p) => (existsSync(p) ? readFileSync(p, "utf8") : null);
const normalise = (s) => s.replace(/\r\n/g, "\n").replace(/[ \t]+$/gm, "").trim();

for (const file of cases) {
  const name = file.replace(/\.shya$/, "");
  const src = join(casesDir, file);
  const out = join(tmpDir, `${name}.mjs`);
  const log = join(tmpDir, `${name}.log`);
  const expectErr = join(expectDir, `${name}.err`);
  const expectOut = join(expectDir, `${name}.out`);
  const expectJs = join(expectDir, `${name}.js`);

  const compiled = run(exe, ["build", src, "-o", out], log);
  const stderr = compiled.text + (compiled.error ? `\n${compiled.error}` : "");

  if (existsSync(expectErr) || (update && compiled.status !== 0)) {
    if (compiled.status === 0) {
      report(`${name} (diagnostics)`, false, "compiled successfully but errors were expected");
      continue;
    }
    if (update) {
      writeFileSync(expectErr, stderr.replace(/\r\n/g, "\n"));
      report(`${name} (diagnostics)`, true);
      continue;
    }
    const wanted = readIf(expectErr).split("\n").map((l) => l.trim()).filter(Boolean);
    const missing = wanted.filter((l) => !stderr.includes(l));
    report(`${name} (diagnostics)`, missing.length === 0,
      `missing from output:\n    ${missing.join("\n    ")}\n--- actual ---\n${stderr}`);
    continue;
  }

  if (compiled.status !== 0) {
    report(name, false, `compile failed:\n${stderr}`);
    continue;
  }

  const problems = [];
  const js = readFileSync(out, "utf8");

  if (existsSync(expectJs)) {
    if (update) writeFileSync(expectJs, js);
    else if (normalise(js) !== normalise(readIf(expectJs)))
      problems.push(`generated JS differs from ${expectJs}`);
  }

  if (existsSync(expectOut) || update) {
    const ran = run(process.execPath, [out], log);
    const actual = ran.text;
    if (ran.status !== 0) problems.push(`runtime exit ${ran.status}:\n${actual}`);
    if (update) writeFileSync(expectOut, actual.replace(/\r\n/g, "\n"));
    else if (normalise(actual) !== normalise(readIf(expectOut)))
      problems.push(`output differs\n--- expected ---\n${readIf(expectOut)}\n--- actual ---\n${actual}`);
  }

  report(name, problems.length === 0, problems.join("\n"));
}

console.log(`\n${passed}/${cases.length} case(s) ok`);

// The examples must always compile cleanly, without warnings.
const examplesDir = join(root, "examples");
if (existsSync(examplesDir)) {
  const examples = readdirSync(examplesDir).filter((f) => f.endsWith(".shya")).sort();
  let clean = 0;
  for (const f of examples) {
    const log = join(tmpDir, `ex-${f}.log`);
    const r = run(exe, ["check", join(examplesDir, f)], log);
    const text = readFileSync(log, "utf8");
    if (r.status === 0 && !text.includes("warning:")) clean++;
    else failures.push({ name: `examples/${f}`, detail: text });
  }
  console.log(`${clean}/${examples.length} example(s) check clean`);
}

if (failures.length) {
  console.log("\nfailures:");
  for (const f of failures) console.log(`\n### ${f.name}\n${f.detail}`);
  process.exit(1);
}
