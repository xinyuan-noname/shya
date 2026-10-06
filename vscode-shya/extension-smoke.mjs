#!/usr/bin/env node
/**
 * extension-smoke.mjs — runs out/extension.js against a small stub of the VS Code API.
 *
 * The extension cannot be launched headlessly without VS Code, but the parts that can
 * realistically be wrong (activation, command registration, the exact command line, the
 * missing-compiler path, the diagnostics panel) are all testable with a stub that records
 * what the extension asks the host to do.
 *
 * The stub lives in a generated CommonJS driver next to this file, which a child node
 * process then executes. That two-step dance exists because the extension is CommonJS and
 * needs `require("vscode")` to resolve to the stub. The child inherits this process's stdio
 * rather than piping it, which keeps the test runnable under a locked-down sandbox.
 *
 * Usage: node extension-smoke.mjs
 */

import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const EXTENSION = path.join(HERE, "out", "extension.js");
const DRIVER_PATH = path.join(HERE, ".extension-smoke-driver.cjs");

const DRIVER = String.raw`
const assert = require("node:assert/strict");
const Module = require("node:module");

const extensionPath = process.argv[2];

/* ------------------------------------------------------------------ record */

const state = {
  messages: [],
  channels: [],
  openedDocuments: [],
  inFlight: [],
  spawns: [],
};

function track(promise) {
  const guarded = Promise.resolve(promise).catch(() => undefined);
  state.inFlight.push(guarded);
  return guarded;
}

/* ------------------------------------------------------------------- stub */

const configStore = {
  compilerPath: process.env.SHYA_TEST_COMPILER_PATH || "shya-does-not-exist-xyz",
  buildArgs: ["build"],
  buildOutputExtension: ".mjs",
};

const uri = (fsPath) => ({ fsPath, scheme: "file", toString: () => "file:///" + fsPath.replace(/\\/g, "/") });

const activeDocument = {
  languageId: "shya",
  fileName: "C:\\\\proj\\\\a.shya",
  uri: uri("C:\\\\proj\\\\a.shya"),
  isDirty: false,
  save: async () => true,
};

function makeChannel(name) {
  const channel = {
    name,
    lines: [],
    appendLine(line) {
      channel.lines.push(String(line));
      state.messages.push("[channel] " + line);
    },
    append(line) {
      channel.lines.push(String(line));
    },
    clear() {},
    dispose() {},
  };
  state.channels.push(channel);
  return channel;
}

const vscode = {
  window: {
    activeTextEditor: { document: activeDocument },
    createOutputChannel: (name) => makeChannel(name),
    showWarningMessage: (m) => track(Promise.resolve(undefined)),
    showErrorMessage: (m) => {
      state.messages.push("[error] " + m);
      return track(Promise.resolve(undefined));
    },
    showInformationMessage: (m) => track(Promise.resolve(undefined)),
    setStatusBarMessage: (m) => {
      state.messages.push("[status] " + m);
      return { dispose() {} };
    },
    showTextDocument: (document) => {
      state.openedDocuments.push(document.content);
      return track(Promise.resolve({ document }));
    },
    withProgress: (_options, task) => {
      const inner = Promise.resolve(task({ report() {} }, { isCancellationRequested: false, onCancellationRequested() { return { dispose() {} }; } }));
      return track(inner.then(() => undefined));
    },
  },
  workspace: {
    getConfiguration: () => ({
      get: (key, fallback) => (key in configStore ? configStore[key] : fallback),
    }),
    getWorkspaceFolder: () => ({ uri: uri("C:\\\\proj"), name: "proj", index: 0 }),
    openTextDocument: (options) => track(Promise.resolve({ content: options.content, languageId: options.language })),
  },
  commands: {
    _handlers: {},
    registerCommand(id, handler) {
      vscode.commands._handlers[id] = handler;
      state.messages.push("[command] " + id);
      return { dispose() {} };
    },
    executeCommand: (id, ...args) => {
      const handler = vscode.commands._handlers[id];
      if (!handler) {
        return Promise.reject(new Error("no such command: " + id));
      }
      return Promise.resolve(handler(...args));
    },
  },
  ProgressLocation: { Window: 10, Notification: 15, SourceControl: 1 },
  Uri: { file: uri, parse: uri },
};

/* --------------------------------------------------------------- load it */

const originalRequire = Module.prototype.require;

/**
 * A fake child_process.spawn. Node's real spawn cannot be used inside the test sandbox
 * (piped stdio is blocked), and a fake also lets us drive the success and failure paths
 * deterministically.
 */
const { EventEmitter } = require("node:events");

const spawnPlan = { mode: "enoent" };

function fakeSpawn(command, args, options) {
  state.spawns.push({ command, args, options });
  const child = new EventEmitter();
  child.stdout = new EventEmitter();
  child.stderr = new EventEmitter();
  child.killed = false;
  child.kill = () => {
    child.killed = true;
  };

  setImmediate(() => {
    if (spawnPlan.mode === "enoent") {
      const err = new Error("spawn " + command + " ENOENT");
      err.code = "ENOENT";
      child.emit("error", err);
      return;
    }
    if (spawnPlan.mode === "ok") {
      child.stdout.emit("data", Buffer.from("export const answer = 42;\\n"));
      child.stderr.emit("data", Buffer.from(""));
      child.emit("close", 0);
      return;
    }
    child.stdout.emit("data", Buffer.from("partial output\\n"));
    child.stderr.emit("data", Buffer.from("a.shya:3:5: error: something went wrong [TC003]\\n"));
    child.emit("close", 3);
  });

  return child;
}

const fakeChildProcess = { spawn: fakeSpawn };

Module.prototype.require = function (request) {
  if (request === "vscode") {
    return vscode;
  }
  if (request === "child_process") {
    return fakeChildProcess;
  }
  return originalRequire.apply(this, arguments);
};

const extension = require(extensionPath);
assert.equal(typeof extension.activate, "function", "extension must export activate");
assert.equal(typeof extension.deactivate, "function", "extension must export deactivate");

const subscriptions = [];
const context = { subscriptions, extensionPath };
extension.activate(context);

assert.equal(typeof vscode.commands._handlers["shya.compile"], "function", "shya.compile must be registered");
assert.ok(subscriptions.length >= 1, "the command disposable must be pushed to context.subscriptions");

(async () => {
  /* 1. The compiler cannot be found: the extension must not crash. */
  spawnPlan.mode = "enoent";
  await vscode.commands.executeCommand("shya.compile");
  await Promise.all(state.inFlight.splice(0));

  const joined = state.messages.join("\n");
  assert.match(joined, /shya\.compile/, "the command should have been registered");
  assert.match(
    joined,
    /could not be started|could not run/,
    "a missing compiler must be reported, got:\n" + joined,
  );
  assert.match(joined, /shya\.compilerPath/, "the hint must mention the setting");
  assert.match(joined, /does-not-exist-xyz/, "the hint must name the path that was tried");
  assert.ok(state.openedDocuments.length >= 1, "a diagnostics panel should have been opened");
  assert.ok(
    state.openedDocuments.some((text) => /compiler/i.test(String(text))),
    "the diagnostics panel should explain the compiler problem",
  );

  /* 2. A working compiler: the exact command line, and a success report. */
  state.messages.length = 0;
  state.openedDocuments.length = 0;
  spawnPlan.mode = "ok";
  const compiler = "C:\\\\tools\\\\shya.exe";
  configStore.compilerPath = compiler;

  await vscode.commands.executeCommand("shya.compile");
  await Promise.all(state.inFlight.splice(0));

  const spawn = state.spawns[state.spawns.length - 1];
  assert.equal(spawn.command, compiler, "the compiler path must be passed to spawn");
  assert.deepEqual(
    spawn.args,
    ["build", "C:\\\\proj\\\\a.shya", "-o", "C:\\\\proj\\\\a.mjs"],
    "the argument vector must be: build <file> -o <file>.mjs",
  );
  assert.equal(spawn.options.cwd, "C:\\\\proj", "the command must run in the workspace root");

  const second = state.messages.join("\n");
  assert.match(second, /export const answer = 42/, "compiler stdout must reach the output channel");
  assert.match(second, /\[shya\] ok/, "a zero exit code should be reported as success");

  /* 3. A failing compiler: exit code 3 must open the diagnostics panel. */
  state.messages.length = 0;
  state.openedDocuments.length = 0;
  spawnPlan.mode = "fail";

  await vscode.commands.executeCommand("shya.compile");
  await Promise.all(state.inFlight.splice(0));

  const third = state.messages.join("\n");
  assert.match(third, /compile failed \(exit code 3\)/, "the failure must name the exit code, got:\n" + third);
  assert.match(third, /error: something went wrong/, "the compiler stderr must be shown");
  assert.ok(
    state.openedDocuments.some((text) => /something went wrong/.test(String(text))),
    "diagnostics must reach the plain text panel",
  );

  /* 4. deactivate must not throw. */
  extension.deactivate();

  console.log(
    "ok   extension: activation, registration, missing compiler, exact argv, success and failure paths",
  );
})().catch((err) => {
  console.error("extension smoke test failed:", err && err.stack ? err.stack : err);
  console.error("recorded messages:\n" + state.messages.join("\n"));
  process.exit(1);
});
`;

fs.writeFileSync(DRIVER_PATH, DRIVER);

let status = 1;
try {
  const result = spawnSync(process.execPath, [DRIVER_PATH, EXTENSION], { stdio: "inherit" });
  if (result.error) {
    console.error(`extension smoke test could not start: ${result.error.message}`);
  } else {
    status = typeof result.status === "number" ? result.status : 1;
  }
} finally {
  try {
    fs.rmSync(DRIVER_PATH, { force: true });
  } catch {
    /* best effort */
  }
}

process.exit(status);
