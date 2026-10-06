"use strict";

/**
 * shya language support.
 *
 * Dependency-free CommonJS: only the `vscode` module and Node's built-ins.
 *
 *   shya.compile             compile the active file to `<file>.mjs` next to it
 *   shya.formatAndPreview    format the active file, then open a live side preview
 *
 * The preview panel shows the generated JavaScript and the compiler's diagnostics, and
 * refreshes while you type. The formatter is registered as a document formatting provider
 * as well, so Shift+Alt+F works.
 */

const vscode = require("vscode");
const path = require("path");
const fs = require("fs");
const os = require("os");
const cp = require("child_process");
const formatter = require("./formatter");

/** @type {vscode.OutputChannel | undefined} */
let channel;
/** @type {vscode.DiagnosticCollection | undefined} */
let diagnostics;
/** @type {{ panel: vscode.WebviewPanel, uri: vscode.Uri, timer: NodeJS.Timeout | undefined } | undefined} */
let preview;

/** Lazily created so that merely activating the extension does not add a panel. */
function outputChannel() {
  if (!channel) {
    channel = vscode.window.createOutputChannel("shya");
  }
  return channel;
}

function configuration() {
  const config = vscode.workspace.getConfiguration("shya");
  const compilerPath = String(config.get("compilerPath", "shya") || "shya").trim() || "shya";
  const buildArgs = config.get("buildArgs", ["build"]);
  const outExtension = String(config.get("buildOutputExtension", ".mjs") || ".mjs");

  return {
    compilerPath,
    buildArgs: Array.isArray(buildArgs) && buildArgs.length ? buildArgs.map(String) : ["build"],
    outExtension: outExtension.startsWith(".") || outExtension === "" ? outExtension : "." + outExtension,
    indentSize: Number(config.get("indentSize", 2)) || 2,
    previewLive: config.get("previewLive", true) !== false,
    formatOnPreview: config.get("formatOnPreview", true) !== false,
  };
}

/** Workspace root for the given file, falling back to the file's own directory. */
function workingDirectoryFor(uri) {
  const folder = vscode.workspace.getWorkspaceFolder(uri);
  return folder ? folder.uri.fsPath : path.dirname(uri.fsPath);
}

function isShyaDocument(document) {
  return (
    Boolean(document) &&
    (document.languageId === "shya" || document.fileName.toLowerCase().endsWith(".shya"))
  );
}

function quotingHint(spawnError, compilerPath) {
  const looksLikeENOENT =
    spawnError &&
    (spawnError.code === "ENOENT" || /ENOENT|not found|no such file/i.test(String(spawnError.message)));
  if (!looksLikeENOENT) {
    return "";
  }
  return (
    `\nThe compiler "${compilerPath}" could not be started.\n` +
    "Set `shya.compilerPath` to the full path of the executable, for example:\n" +
    "  D:\\\\project\\\\shya\\\\build\\\\shya.exe\n" +
    "or make sure `shya` is on PATH.\n"
  );
}

// ------------------------------------------------------------------ formatting ---

function fullRange(document) {
  return new vscode.Range(document.positionAt(0), document.positionAt(document.getText().length));
}

function formattingEdits(document) {
  const { indentSize } = configuration();
  const source = document.getText();
  const formatted = formatter.formatShya(source, { indentSize });
  if (formatted === source) {
    return [];
  }
  return [vscode.TextEdit.replace(fullRange(document), formatted)];
}

/** Formats `editor`'s document in place. Resolves to true when something changed. */
async function formatDocument(editor) {
  const edits = formattingEdits(editor.document);
  if (!edits.length) {
    return false;
  }
  const applied = await vscode.workspace.applyEdit(
    new vscode.WorkspaceEdit().set(editor.document.uri, edits),
  );
  return Boolean(applied);
}

// ------------------------------------------------------------------- compiling ---

/**
 * Runs the compiler over an arbitrary source text.
 *
 * The temporary copy is written **next to the original file**, because `import "./x.shya"`
 * resolves relative to the importing file — a copy in the system temp directory would break
 * every relative macro import.
 */
function compileText(document, text) {
  const { compilerPath, buildArgs, outExtension } = configuration();
  const originalPath = document.uri.fsPath;
  const dir = path.dirname(originalPath);
  const base = path.basename(originalPath, path.extname(originalPath));
  const tmpSource = path.join(dir, `.shya-preview-${base}.shya`);
  const tmpOutput = path.join(os.tmpdir(), `shya-preview-${process.pid}-${Date.now()}${outExtension}`);
  const cwd = workingDirectoryFor(document.uri);
  const args = buildArgs.concat([tmpSource, "-o", tmpOutput]);

  return new Promise((resolve) => {
    const cleanup = () => {
      try { fs.unlinkSync(tmpSource); } catch { /* already gone */ }
      try { fs.unlinkSync(tmpOutput); } catch { /* already gone */ }
    };

    try {
      fs.writeFileSync(tmpSource, text, "utf8");
    } catch (err) {
      cleanup();
      resolve({ ok: false, code: -1, js: "", diagnosticsText: `could not write ${tmpSource}: ${err.message}`, command: "" });
      return;
    }

    // Show the compiler invocation with the real path, never the temporary copy.
    const command = `${compilerPath} ${args
      .map((a) => (a === tmpSource ? originalPath : a))
      .join(" ")}`;
    let child;
    try {
      child = cp.spawn(compilerPath, args, { cwd, windowsHide: true });
    } catch (err) {
      cleanup();
      resolve({
        ok: false, code: -1, js: "",
        diagnosticsText: `could not run the compiler (${err.message}).\n${quotingHint({ code: "ENOENT" }, compilerPath)}`,
        command,
      });
      return;
    }

    const stdout = [];
    const stderr = [];
    let settled = false;
    const finish = (code, spawnError) => {
      if (settled) return;
      settled = true;

      let js = "";
      try { js = fs.readFileSync(tmpOutput, "utf8"); } catch { /* failed compile */ }

      // Diagnostics point at the temporary copy; rewrite them onto the real file.
      const rewrite = (s) =>
        s
          .split(tmpSource).join(originalPath)
          .split(tmpSource.replace(/\\/g, "/")).join(originalPath);

      let text = rewrite(Buffer.concat(stderr).toString("utf8"));
      if (spawnError) {
        text = `could not run "${compilerPath}": ${spawnError.message}\n${quotingHint(spawnError, compilerPath)}`;
      }
      const outText = rewrite(Buffer.concat(stdout).toString("utf8"));
      if (!text.trim() && outText.trim()) text = outText;

      cleanup();
      resolve({ ok: code === 0 && !spawnError, code: typeof code === "number" ? code : -1, js, diagnosticsText: text, command });
    };

    if (child.stdout) child.stdout.on("data", (c) => stdout.push(Buffer.from(c)));
    if (child.stderr) child.stderr.on("data", (c) => stderr.push(Buffer.from(c)));
    child.on("error", (err) => finish(-1, err));
    child.on("close", (code) => finish(typeof code === "number" ? code : -1, null));
  });
}

/** `file:line:col: severity: message [CODE]` -> VS Code diagnostics. */
function publishDiagnostics(document, text) {
  if (!diagnostics) return;
  const items = [];
  // Codes are 2-3 letters plus 3 digits: TC003, LEX009, MOD001.
  const line = /^(.+?):(\d+):(\d+):\s*(error|warning|note):\s*(.*?)(?:\s*\[([A-Z]{2,3}\d{3})\])?\s*$/;
  for (const raw of text.split(/\r?\n/)) {
    const m = line.exec(raw.trim());
    if (!m) continue;
    const lineNo = Math.max(0, Number(m[2]) - 1);
    const colNo = Math.max(0, Number(m[3]) - 1);
    const severity =
      m[4] === "error" ? vscode.DiagnosticSeverity.Error
      : m[4] === "warning" ? vscode.DiagnosticSeverity.Warning
      : vscode.DiagnosticSeverity.Information;
    const safeLine = Math.min(lineNo, Math.max(0, document.lineCount - 1));
    const lineText = document.lineAt(safeLine).text;
    const start = new vscode.Position(safeLine, Math.min(colNo, Math.max(0, lineText.length)));
    const end = new vscode.Position(safeLine, lineText.length);
    const d = new vscode.Diagnostic(new vscode.Range(start, end), m[5], severity);
    if (m[6]) d.code = m[6];
    d.source = "shya";
    items.push(d);
  }
  diagnostics.set(document.uri, items);
}

// --------------------------------------------------------------------- preview ---

function escapeHtml(s) {
  return String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function previewHtml(document, result) {
  const name = path.basename(document.uri.fsPath);
  const errorLines = result.diagnosticsText
    .split(/\r?\n/)
    .filter((l) => /:\d+:\d+:\s*(error|warning|note):/.test(l));
  const errors = errorLines.filter((l) => /:\s*error:/.test(l)).length;
  const warnings = errorLines.filter((l) => /:\s*warning:/.test(l)).length;

  const status = result.ok
    ? `<span class="ok">编译成功</span>`
    : `<span class="bad">编译失败（exit ${result.code}）</span>`;
  const counts = result.ok
    ? ""
    : `<span class="pill warn">${warnings} warning</span><span class="pill err">${errors} error</span>`;

  const diagnosticsBlock = result.diagnosticsText.trim()
    ? `<h2>诊断</h2><pre class="diag">${escapeHtml(result.diagnosticsText.trim())}</pre>`
    : "";

  const codeBlock = result.js
    ? `<h2>生成的 JavaScript</h2><pre class="code">${escapeHtml(result.js)}</pre>`
    : `<h2>生成的 JavaScript</h2><pre class="code dim">（编译失败，没有产出）</pre>`;

  return `<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline';">
<style>
  body { font-family: var(--vscode-font-family); font-size: var(--vscode-font-size); color: var(--vscode-foreground); padding: 0 12px 24px; }
  h1 { font-size: 1.05em; margin: 12px 0 4px; }
  h2 { font-size: .95em; margin: 18px 0 6px; opacity: .8; font-weight: 600; }
  .meta { opacity: .65; font-size: .85em; word-break: break-all; }
  .ok { color: var(--vscode-testing-iconPassed, #3fb950); font-weight: 600; }
  .bad { color: var(--vscode-testing-iconFailed, #f85149); font-weight: 600; }
  .pill { display: inline-block; margin-left: 8px; padding: 0 6px; border-radius: 8px; font-size: .8em; }
  .pill.err { background: rgba(248,81,73,.18); color: #f85149; }
  .pill.warn { background: rgba(210,153,34,.18); color: #d29922; }
  pre { font-family: var(--vscode-editor-font-family); font-size: var(--vscode-editor-font-size);
        background: var(--vscode-textCodeBlock-background); padding: 10px; border-radius: 4px;
        overflow-x: auto; white-space: pre; line-height: 1.45; }
  pre.diag { color: var(--vscode-editorWarning-foreground, #d29922); }
  pre.dim { opacity: .5; }
</style>
</head>
<body>
<h1>${escapeHtml(name)} ${status}${counts}</h1>
<div class="meta">${escapeHtml(result.command)}</div>
${diagnosticsBlock}
${codeBlock}
</body>
</html>`;
}

async function refreshPreview(document, options) {
  if (!preview || preview.uri.toString() !== document.uri.toString()) return;
  const result = await compileText(document, document.getText());
  publishDiagnostics(document, result.diagnosticsText);
  if (preview && preview.panel) {
    preview.panel.webview.html = previewHtml(document, result);
    preview.panel.title = result.ok ? `shya 预览 · ${path.basename(document.uri.fsPath)}` : `shya 预览 · 编译失败`;
  }
  if (!result.ok) {
    outputChannel().appendLine(`[shya] preview: exit ${result.code}`);
    if (result.diagnosticsText.trim()) outputChannel().appendLine(result.diagnosticsText.trim());
  }
  return result;
}

function scheduleRefresh(document) {
  if (!preview || preview.uri.toString() !== document.uri.toString()) return;
  if (!configuration().previewLive) return;
  if (preview.timer) clearTimeout(preview.timer);
  preview.timer = setTimeout(() => {
    if (preview) preview.timer = undefined;
    refreshPreview(document).catch(() => undefined);
  }, 400);
}

function disposePreview() {
  if (!preview) return;
  if (preview.timer) clearTimeout(preview.timer);
  preview = undefined;
}

/**
 * `shya.formatAndPreview` — format the active file, then open the preview panel beside it.
 */
async function formatAndPreview() {
  const editor = vscode.window.activeTextEditor;
  if (!editor || !isShyaDocument(editor.document)) {
    vscode.window.showWarningMessage("shya: 先打开一个 .shya 文件。");
    return;
  }

  const document = editor.document;
  const { formatOnPreview } = configuration();
  let formatted = false;
  if (formatOnPreview) {
    try {
      formatted = await formatDocument(editor);
    } catch (err) {
      outputChannel().appendLine(`[shya] format failed: ${err && err.message ? err.message : err}`);
    }
  }

  if (preview && preview.uri.toString() === document.uri.toString()) {
    preview.panel.reveal(vscode.ViewColumn.Beside, true);
  } else {
    disposePreview();
    const panel = vscode.window.createWebviewPanel(
      "shyaPreview",
      `shya 预览 · ${path.basename(document.uri.fsPath)}`,
      vscode.ViewColumn.Beside,
      { enableScripts: false, retainContextWhenHidden: true },
    );
    panel.webview.html = `<!DOCTYPE html><html><body style="font-family:sans-serif;padding:12px">正在编译…</body></html>`;
    panel.onDidDispose(() => disposePreview(), null, []);
    preview = { panel, uri: document.uri, timer: undefined };
  }

  const result = await refreshPreview(document);
  if (formatted) {
    vscode.window.setStatusBarMessage("shya: 已格式化并预览", 3000);
  } else if (result && result.ok) {
    vscode.window.setStatusBarMessage("shya: 预览已更新", 3000);
  }
}

// --------------------------------------------------------------- compile command ---

async function showDiagnostics(title, text) {
  try {
    const document = await vscode.workspace.openTextDocument({ content: text, language: "plaintext" });
    await vscode.window.showTextDocument(document, { preview: false, preserveFocus: false });
  } catch (err) {
    outputChannel().appendLine(`[shya] could not open the diagnostics panel: ${err && err.message ? err.message : err}`);
  }
  outputChannel().appendLine(text);
}

async function compileActiveFile() {
  const editor = vscode.window.activeTextEditor;
  if (!editor || !editor.document) {
    vscode.window.showWarningMessage("shya: open a .shya file first.");
    return;
  }

  const document = editor.document;
  if (!isShyaDocument(document)) {
    vscode.window.showWarningMessage("shya: the active file is not a shya file.");
    return;
  }

  if (document.isDirty) {
    const saved = await document.save();
    if (!saved) {
      vscode.window.showWarningMessage("shya: the file could not be saved, compile aborted.");
      return;
    }
  }

  const { compilerPath, buildArgs, outExtension } = configuration();
  const inputPath = document.uri.fsPath;
  const outPath = inputPath.replace(/\.shya$/i, "") + outExtension;
  const cwd = workingDirectoryFor(document.uri);
  const args = buildArgs.concat([inputPath, "-o", outPath]);

  const out = outputChannel();
  out.appendLine("");
  out.appendLine(`[shya] ${compilerPath} ${args.slice(0, -2).join(" ")} "${inputPath}" -o "${outPath}"`);

  await vscode.window.withProgress(
    { location: vscode.ProgressLocation.Window, title: "shya: compiling…" },
    () =>
      new Promise((resolve) => {
        let child;
        try {
          child = cp.spawn(compilerPath, args, { cwd, windowsHide: true });
        } catch (err) {
          const hint = `shya: could not run the compiler (${err && err.message ? err.message : err}).\n` +
            quotingHint({ code: "ENOENT" }, compilerPath);
          out.appendLine(hint);
          showDiagnostics("shya: compiler not runnable", hint).then(resolve, resolve);
          return;
        }

        const stdout = [];
        const stderr = [];
        let settled = false;
        const finish = (code, spawnError) => {
          if (settled) return;
          settled = true;

          const stdoutText = Buffer.concat(stdout).toString("utf8");
          const stderrText = Buffer.concat(stderr).toString("utf8");
          if (stdoutText.trim()) out.appendLine(stdoutText.replace(/\s+$/, ""));
          if (stderrText.trim()) out.appendLine(stderrText.replace(/\s+$/, ""));
          if (diagnostics && document) publishDiagnostics(document, stderrText);

          if (spawnError) {
            const text = `shya: ${spawnError.message || spawnError}\n${quotingHint(spawnError, compilerPath)}`;
            vscode.window
              .showErrorMessage(`shya: could not run "${compilerPath}". See the shya output channel.`)
              .then(undefined, () => undefined);
            showDiagnostics("shya: compiler not runnable", text).then(resolve, resolve);
            return;
          }

          if (code === 0) {
            out.appendLine(`[shya] ok → ${outPath}`);
            vscode.window.setStatusBarMessage(`shya: compiled ${path.basename(outPath)}`, 4000);
            resolve();
            return;
          }

          const header = `shya compile failed (exit code ${code})\n` + `file: ${inputPath}\n\n`;
          const body = (stderrText.trim() || stdoutText.trim()) || "(the compiler produced no output)";
          vscode.window
            .showErrorMessage(`shya: compile failed (exit code ${code}). See the diagnostics panel.`)
            .then(undefined, () => undefined);
          showDiagnostics("shya: diagnostics", header + body + "\n").then(resolve, resolve);
        };

        if (child.stdout) child.stdout.on("data", (chunk) => stdout.push(Buffer.from(chunk)));
        if (child.stderr) child.stderr.on("data", (chunk) => stderr.push(Buffer.from(chunk)));
        child.on("error", (err) => finish(-1, err));
        child.on("close", (code) => finish(typeof code === "number" ? code : -1, null));
      }),
  );
}

// -------------------------------------------------------------------- activation ---

function guard(label, fn) {
  return (...args) =>
    Promise.resolve(fn(...args)).catch((err) => {
      const message = `shya: unexpected error in ${label}: ${err && err.stack ? err.stack : err}`;
      outputChannel().appendLine(message);
      vscode.window.showErrorMessage("shya: unexpected error, see the shya output channel.").then(undefined, () => undefined);
    });
}

function activate(context) {
  diagnostics = vscode.languages.createDiagnosticCollection("shya");

  context.subscriptions.push(
    diagnostics,
    vscode.commands.registerCommand("shya.compile", guard("shya.compile", compileActiveFile)),
    vscode.commands.registerCommand("shya.formatAndPreview", guard("shya.formatAndPreview", formatAndPreview)),
    vscode.commands.registerCommand(
      "shya.format",
      guard("shya.format", async () => {
        const editor = vscode.window.activeTextEditor;
        if (!editor || !isShyaDocument(editor.document)) {
          vscode.window.showWarningMessage("shya: 先打开一个 .shya 文件。");
          return;
        }
        const changed = await formatDocument(editor);
        vscode.window.setStatusBarMessage(changed ? "shya: 已格式化" : "shya: 已经是格式化过的", 3000);
      }),
    ),
    vscode.languages.registerDocumentFormattingEditProvider("shya", {
      provideDocumentFormattingEdits: (document) => formattingEdits(document),
    }),
    vscode.workspace.onDidChangeTextDocument((event) => scheduleRefresh(event.document)),
    vscode.workspace.onDidSaveTextDocument((document) => {
      if (preview && preview.uri.toString() === document.uri.toString()) {
        refreshPreview(document).catch(() => undefined);
      }
    }),
    vscode.window.onDidChangeActiveTextEditor((editor) => {
      if (editor && preview && preview.uri.toString() === editor.document.uri.toString()) {
        refreshPreview(editor.document).catch(() => undefined);
      }
    }),
    vscode.workspace.onDidCloseTextDocument((document) => {
      if (diagnostics) diagnostics.delete(document.uri);
      if (preview && preview.uri.toString() === document.uri.toString()) disposePreview();
    }),
  );

  try {
    const { compilerPath } = configuration();
    if (/[\\/]/.test(compilerPath) && !fs.existsSync(compilerPath)) {
      outputChannel().appendLine(
        `[shya] note: shya.compilerPath points at "${compilerPath}", which does not exist yet.`,
      );
    }
  } catch {
    /* never let a diagnostic probe break activation */
  }
}

function deactivate() {
  disposePreview();
  if (channel) {
    channel.dispose();
    channel = undefined;
  }
  if (diagnostics) {
    diagnostics.dispose();
    diagnostics = undefined;
  }
}

module.exports = { activate, deactivate };
