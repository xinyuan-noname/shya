// Extension tests against a stubbed VS Code API.
//
//   node vscode-shya/test/extension.test.mjs
//
// Covers activation, the exact compiler invocation, the formatter, the combined
// format-and-preview command (including its webview content and the diagnostics it
// publishes) and the "compiler not runnable" path. `child_process.spawn` is faked so
// nothing real has to be installed.
import { createRequire } from "node:module";
import { existsSync, mkdirSync, readFileSync, rmSync, unlinkSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import Module from "node:module";

const require = createRequire(import.meta.url);
const here = dirname(fileURLToPath(import.meta.url));
const extDir = resolve(here, "..");

const sandbox = join(extDir, ".test-tmp");
rmSync(sandbox, { recursive: true, force: true });
const projectDir = join(sandbox, "proj");
mkdirSync(projectDir, { recursive: true });
const mainPath = join(projectDir, "a.shya");
writeFileSync(mainPath, "let a=1\n");
// A second file, so the failure-path test gets its own preview panel.
const secondPath = join(projectDir, "b.shya");
writeFileSync(secondPath, "let b=1\n");

const state = {
  messages: [],
  spawns: [],
  applied: [],
  editBuilds: [],
  diagnostics: new Map(),
  panels: [],
  formattingProviders: [],
  commands: {},
  listeners: {},
};

let nextExit = { code: 0, stderr: "", stdout: "", js: "// compiled\nconsole.log(1);\n", error: null };

function uri(fsPath) {
  return {
    fsPath,
    path: fsPath.replace(/\\/g, "/"),
    scheme: "file",
    toString: () => "file://" + fsPath.replace(/\\/g, "/"),
  };
}

function makeDocument(text, fsPath) {
  const file = fsPath || mainPath;
  const lines = text.split("\n");
  return {
    languageId: "shya",
    fileName: file,
    uri: uri(file),
    isDirty: false,
    lineCount: lines.length,
    getText: () => text,
    positionAt: (offset) => ({ offset }),
    lineAt: (n) => ({ text: lines[n] === undefined ? "" : lines[n] }),
    save: async () => true,
  };
}

const activeDocument = makeDocument(readFileSync(mainPath, "utf8"));

class Emitter {
  constructor() {
    this.handlers = [];
  }
  event(handler) {
    this.handlers.push(handler);
    return { dispose() {} };
  }
  fire(value) {
    for (const h of this.handlers) h(value);
  }
}

const vscode = {
  window: {
    activeTextEditor: { document: activeDocument, edit: async () => true },
    createOutputChannel: (name) => ({
      name,
      lines: [],
      appendLine(l) {
        this.lines.push(String(l));
        state.messages.push("[channel] " + l);
      },
      append(l) {
        this.lines.push(String(l));
      },
      clear() {},
      dispose() {},
    }),
    showWarningMessage: (m) => {
      state.messages.push("[warn] " + m);
      return Promise.resolve(undefined);
    },
    showErrorMessage: (m) => {
      state.messages.push("[error] " + m);
      return Promise.resolve(undefined);
    },
    showInformationMessage: () => Promise.resolve(undefined),
    setStatusBarMessage: (m) => {
      state.messages.push("[status] " + m);
      return { dispose() {} };
    },
    showTextDocument: (document) => Promise.resolve({ document }),
    withProgress: (_o, task) => Promise.resolve(task({ report() {} }, { isCancellationRequested: false })),
    onDidChangeActiveTextEditor: (h) => {
      (state.listeners.activeEditor = state.listeners.activeEditor || new Emitter()).event(h);
      return { dispose() {} };
    },
    createWebviewPanel(kind, title, column, options) {
      const panel = {
        kind,
        title,
        column,
        options,
        html: "",
        disposed: false,
        webview: {
          set html(value) {
            panel._html = value;
          },
          get html() {
            return panel._html;
          },
        },
        reveal() {},
        onDidDispose(h) {
          panel._onDispose = h;
          return { dispose() {} };
        },
        dispose() {
          panel.disposed = true;
          if (panel._onDispose) panel._onDispose();
        },
      };
      panel._html = "";
      state.panels.push(panel);
      return panel;
    },
  },
  workspace: {
    getConfiguration: () => ({
      get: (key, fallback) => (key in configStore ? configStore[key] : fallback),
    }),
    getWorkspaceFolder: () => ({ uri: uri(projectDir), name: "proj", index: 0 }),
    openTextDocument: (options) => Promise.resolve({ content: options.content, languageId: options.language }),
    applyEdit: async (edit) => {
      state.applied.push(edit);
      return true;
    },
    onDidChangeTextDocument: (h) => {
      (state.listeners.change = state.listeners.change || new Emitter()).event(h);
      return { dispose() {} };
    },
    onDidSaveTextDocument: (h) => {
      (state.listeners.save = state.listeners.save || new Emitter()).event(h);
      return { dispose() {} };
    },
    onDidCloseTextDocument: (h) => {
      (state.listeners.close = state.listeners.close || new Emitter()).event(h);
      return { dispose() {} };
    },
  },
  languages: {
    createDiagnosticCollection: (name) => ({
      name,
      set(u, items) {
        state.diagnostics.set(u.fsPath, items);
      },
      delete(u) {
        state.diagnostics.delete(u.fsPath);
      },
      dispose() {},
    }),
    registerDocumentFormattingEditProvider(language, provider) {
      state.formattingProviders.push({ language, provider });
      return { dispose() {} };
    },
  },
  commands: {
    registerCommand(id, handler) {
      state.commands[id] = handler;
      return { dispose() {} };
    },
    executeCommand: (id, ...args) => Promise.resolve(state.commands[id](...args)),
  },
  ProgressLocation: { Window: 10 },
  ViewColumn: { Beside: -2, One: 1 },
  DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2, Hint: 3 },
  Uri: { file: uri, parse: uri },
  Range: class Range {
    constructor(start, end) {
      this.start = start;
      this.end = end;
    }
  },
  Position: class Position {
    constructor(line, character) {
      this.line = line;
      this.character = character;
    }
  },
  TextEdit: { replace: (range, text) => ({ range, newText: text }) },
  WorkspaceEdit: class WorkspaceEdit {
    set(u, edits) {
      // Recorded separately from applyEdit so the assertions can count applications.
      state.editBuilds.push({ uri: u, edits });
      return this;
    }
  },
  Diagnostic: class Diagnostic {
    constructor(range, message, severity) {
      this.range = range;
      this.message = message;
      this.severity = severity;
    }
  },
};

const configStore = {};

/* -------------------------------------------------------------- fake spawn --- */

const fakeCp = {
  spawn(program, args, options) {
    const record = { program, args: args.slice(), options };
    state.spawns.push(record);
    if (nextExit.error) {
      const listeners = {};
      const child = {
        stdout: { on() {} },
        stderr: { on() {} },
        on(name, h) {
          listeners[name] = h;
          return this;
        },
      };
      setImmediate(() => listeners.error && listeners.error(nextExit.error));
      return child;
    }
    // Materialise the -o output so the extension can read it back.
    const outIndex = args.indexOf("-o");
    if (outIndex >= 0 && args[outIndex + 1]) {
      writeFileSync(args[outIndex + 1], nextExit.js, "utf8");
    }
    const handlers = { stdout: [], stderr: [], close: [], error: [] };
    const child = {
      stdout: { on: (n, h) => n === "data" && handlers.stdout.push(h) },
      stderr: { on: (n, h) => n === "data" && handlers.stderr.push(h) },
      on(name, h) {
        handlers[name].push(h);
        return this;
      },
    };
    setImmediate(() => {
      if (nextExit.stdout) for (const h of handlers.stdout) h(Buffer.from(nextExit.stdout));
      if (nextExit.stderr) for (const h of handlers.stderr) h(Buffer.from(nextExit.stderr));
      for (const h of handlers.close) h(nextExit.code);
    });
    return child;
  },
};

/* ------------------------------------------------------------------ loading --- */

const originalRequire = Module.prototype.require;
Module.prototype.require = function patched(id) {
  if (id === "vscode") return vscode;
  if (id === "child_process") return fakeCp;
  return originalRequire.apply(this, arguments);
};

const extension = require(join(extDir, "out", "extension.js"));

/* -------------------------------------------------------------------- tests --- */

let checks = 0;
let failures = 0;

function ok(name, condition, detail) {
  checks += 1;
  if (condition) {
    console.log(`  ok    ${name}`);
  } else {
    failures += 1;
    console.log(`  FAIL  ${name}`);
    if (detail !== undefined) console.log(String(detail).replace(/^/gm, "        "));
  }
}

function eq(name, actual, expected) {
  ok(name, JSON.stringify(actual) === JSON.stringify(expected), `expected ${JSON.stringify(expected)}\nactual   ${JSON.stringify(actual)}`);
}

const tick = () => new Promise((r) => setTimeout(r, 60));

console.log("extension:");

const context = { subscriptions: [] };
extension.activate(context);

eq("registers the three commands", Object.keys(state.commands).sort(), ["shya.compile", "shya.format", "shya.formatAndPreview"]);
eq("registers a formatting provider for shya", state.formattingProviders.map((p) => p.language), ["shya"]);

/* --- formatter provider --- */

{
  const provider = state.formattingProviders[0].provider;
  const messy = makeDocument("fn f() {\nlet x=1\nconsole log(x)\n}\n");
  const edits = provider.provideDocumentFormattingEdits(messy);
  eq("formatter provider returns one full-document edit", edits.length, 1);
  eq("formatter provider output", edits[0].newText, "fn f() {\n  let x=1\n  console log(x)\n}\n");

  const clean = makeDocument("fn f() {\n  let x = 1\n}\n");
  eq("formatter provider is a no-op on formatted input", provider.provideDocumentFormattingEdits(clean).length, 0);
}

/* --- shya.compile: exact invocation --- */

{
  nextExit = { code: 0, stderr: "", stdout: "", js: "", error: null };
  state.spawns.length = 0;
  await vscode.commands.executeCommand("shya.compile");
  await tick();
  eq("shya.compile spawns the compiler once", state.spawns.length, 1);
  eq("shya.compile argv", state.spawns[0].args, ["build", mainPath, "-o", join(projectDir, "a.mjs")]);
  eq("shya.compile cwd is the workspace folder", state.spawns[0].options.cwd, projectDir);
}

/* --- shya.format --- */

{
  vscode.window.activeTextEditor = { document: makeDocument("fn f() {\nlet x=1\n}\n") };
  state.applied.length = 0;
  await vscode.commands.executeCommand("shya.format");
  eq("shya.format applies one edit", state.applied.length, 1);
  eq("shya.format edit text", state.editBuilds[0].edits[0].newText, "fn f() {\n  let x=1\n}\n");
}

/* --- shya.formatAndPreview: formats, compiles a same-directory temp copy, renders --- */

{
  nextExit = {
    code: 0,
    stderr: "",
    stdout: "",
    js: "// Generated by the shya compiler\nconsole.log(1);\n",
    error: null,
  };
  state.spawns.length = 0;
  state.panels.length = 0;
  state.applied.length = 0;
  state.diagnostics.clear();
  vscode.window.activeTextEditor = { document: makeDocument("fn main() {\nlet x = 1\n}\nmain()\n") };

  await vscode.commands.executeCommand("shya.formatAndPreview");
  await tick();

  eq("formatAndPreview formatted the document first", state.applied.length, 1);
  eq("formatAndPreview opened one preview panel", state.panels.length, 1);
  eq("preview opens beside the editor", state.panels[0].column, vscode.ViewColumn.Beside);
  ok("preview html contains the compiled JavaScript", state.panels[0]._html.includes("console.log(1);"), state.panels[0]._html.slice(0, 400));
  ok("preview html reports success", state.panels[0]._html.includes("编译成功"), state.panels[0]._html.slice(0, 400));

  const spawned = state.spawns[0];
  const sourceArg = spawned.args[1];
  eq("compiles a temporary copy next to the original (so relative imports resolve)", dirname(sourceArg), projectDir);
  ok("temporary source is named for the original", /\.shya-preview-a\.shya$/.test(sourceArg), sourceArg);
  ok("temporary source is cleaned up", !existsSync(sourceArg), sourceArg);
}

/* --- failure path: diagnostics are published and shown in the panel --- */

{
  const diagText = `${secondPath}:2:7: error: 变量 \`hp\` 的初始值：期望 number，实际是 string [TC003]\n` +
    `${secondPath}:2:7: warning: 示例警告 [TC014]\n` +
    `shya: check failed with 1 error(s)\n`;
  nextExit = { code: 1, stderr: diagText, stdout: "", js: "", error: null };
  state.spawns.length = 0;
  state.panels.length = 0;
  state.diagnostics.clear();
  const doc = makeDocument("fn main() {\nlet hp: number = \"x\"\n}\nmain()\n", secondPath);
  vscode.window.activeTextEditor = { document: doc };

  await vscode.commands.executeCommand("shya.formatAndPreview");
  await tick();

  const published = state.diagnostics.get(doc.uri.fsPath) || [];
  eq("publishes two diagnostics", published.length, 2);
  eq("first diagnostic is an error", published[0].severity, vscode.DiagnosticSeverity.Error);
  eq("diagnostic code is carried through", published[0].code, "TC003");
  eq("diagnostic range row is 0-based", published[0].range.start.line, 1);
  eq("second diagnostic is a warning", published[1].severity, vscode.DiagnosticSeverity.Warning);
  ok("panel shows the raw diagnostic text",
    state.panels.length > 0 && state.panels[0]._html.includes("TC003"),
    `panels=${state.panels.length}\n` + state.messages.slice(-8).join("\n"));
  ok("panel title reports failure",
    state.panels.length > 0 && /编译失败/.test(state.panels[0].title),
    state.panels.length ? state.panels[0].title : `panels=${state.panels.length}`);
  ok("temporary source was still cleaned up",
    state.spawns.every((s) => !existsSync(s.args[1])), "temp file left behind");
}

/* --- the preview never leaks the temporary copy's path --- */

{
  ok("no preview mentions the temporary copy",
    state.panels.every((p) => !/\.shya-preview-/.test(p._html)),
    state.panels.map((p) => p._html).join("\n").slice(0, 600));
  ok("diagnostics are keyed by the real file",
    state.diagnostics.has(secondPath), [...state.diagnostics.keys()].join(", "));
}

/* --- compiler not runnable --- */

{
  nextExit = { error: Object.assign(new Error("spawn shya ENOENT"), { code: "ENOENT" }) };
  state.spawns.length = 0;
  vscode.window.activeTextEditor = { document: makeDocument("let a = 1\n") };
  await vscode.commands.executeCommand("shya.formatAndPreview");
  await tick();
  ok("missing compiler is reported, not thrown",
    state.messages.some((m) => /could not run|ENOENT|compilerPath/.test(m)),
    state.messages.slice(-4).join("\n"));
}

/* --- cleanup --- */

Module.prototype.require = originalRequire;
extension.deactivate();
rmSync(sandbox, { recursive: true, force: true });

console.log(`\n${checks - failures}/${checks} check(s) ok`);
if (failures > 0) process.exit(1);
