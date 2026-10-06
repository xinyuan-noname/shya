"use strict";

/**
 * shya language support — compile command.
 *
 * Dependency-free CommonJS: only the `vscode` module and Node's built-ins are used.
 * `shya.compile` runs `<shya.compilerPath> build <file> -o <file>.mjs` in the workspace
 * root, streams the result into an output channel, and (on failure) opens a plain text
 * document with the compiler's diagnostics. There is deliberately no DiagnosticCollection:
 * parsing shya diagnostics is the compiler's job, not the extension's.
 */

const vscode = require("vscode");
const path = require("path");
const fs = require("fs");
const cp = require("child_process");

/** @type {vscode.OutputChannel | undefined} */
let channel;

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
  };
}

/** Workspace root for the given file, falling back to the file's own directory. */
function workingDirectoryFor(uri) {
  const folder = vscode.workspace.getWorkspaceFolder(uri);
  if (folder) {
    return folder.uri.fsPath;
  }
  return path.dirname(uri.fsPath);
}

function quotingHint(spawnError, compilerPath) {
  const looksLikeENOENT =
    spawnError && (spawnError.code === "ENOENT" || /ENOENT|not found|no such file/i.test(String(spawnError.message)));
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

/**
 * Show the captured stdout/stderr in a plain, read-only text document so the user can
 * read the diagnostics with syntax-free scrolling and copy them out.
 */
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
  if (document.languageId !== "shya" && !document.fileName.toLowerCase().endsWith(".shya")) {
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
          // spawn() itself can throw synchronously for a malformed command.
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
          if (settled) {
            return;
          }
          settled = true;

          const stdoutText = Buffer.concat(stdout).toString("utf8");
          const stderrText = Buffer.concat(stderr).toString("utf8");
          if (stdoutText.trim()) {
            out.appendLine(stdoutText.replace(/\s+$/, ""));
          }
          if (stderrText.trim()) {
            out.appendLine(stderrText.replace(/\s+$/, ""));
          }

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

        if (child.stdout) {
          child.stdout.on("data", (chunk) => stdout.push(Buffer.from(chunk)));
        }
        if (child.stderr) {
          child.stderr.on("data", (chunk) => stderr.push(Buffer.from(chunk)));
        }
        child.on("error", (err) => finish(-1, err));
        child.on("close", (code) => finish(typeof code === "number" ? code : -1, null));
      }),
  );
}

function activate(context) {
  context.subscriptions.push(
    vscode.commands.registerCommand("shya.compile", () => {
      // Never let a failure escape as an unhandled rejection.
      return Promise.resolve(compileActiveFile()).catch((err) => {
        const message = `shya: unexpected error: ${err && err.stack ? err.stack : err}`;
        outputChannel().appendLine(message);
        vscode.window.showErrorMessage("shya: unexpected error, see the shya output channel.").then(undefined, () => undefined);
      });
    }),
  );

  // Touch the file system once so an obviously wrong compilerPath is reported while the
  // extension is loading rather than at first compile. Purely informational.
  try {
    const { compilerPath } = configuration();
    const looksLikePath = /[\\/]/.test(compilerPath);
    if (looksLikePath && !fs.existsSync(compilerPath)) {
      outputChannel().appendLine(
        `[shya] note: shya.compilerPath points at "${compilerPath}", which does not exist yet.`,
      );
    }
  } catch (err) {
    /* never let a diagnostic probe break activation */
  }
}

function deactivate() {
  if (channel) {
    channel.dispose();
    channel = undefined;
  }
}

module.exports = { activate, deactivate };
