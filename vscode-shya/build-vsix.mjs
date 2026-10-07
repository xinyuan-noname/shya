#!/usr/bin/env node
/**
 * build-vsix.mjs — package the shya VS Code extension into shya-0.1.0.vsix
 *
 * Route 1: `npx --yes @vscode/vsce package` (used when npx and the package are reachable).
 * Route 2: a built-in, dependency-free ZIP writer on top of Node's zlib, used whenever
 *          route 1 fails for any reason (offline machine, no npx, proxy, …).
 *
 * Either way the produced archive is verified afterwards by reading every local file
 * header and central-directory entry back out and inflating the stored payloads.
 *
 * Usage: node build-vsix.mjs
 */

import { spawnSync } from "node:child_process";
import { deflateRawSync, inflateRawSync } from "node:zlib";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { runGrammarChecks } from "./grammar-smoke.mjs";

const ROOT = path.dirname(fileURLToPath(import.meta.url));
const pkg = JSON.parse(fs.readFileSync(path.join(ROOT, "package.json"), "utf8"));

const VSIX_NAME = `${pkg.name}-${pkg.version}.vsix`;
const VSIX_PATH = path.join(ROOT, VSIX_NAME);

/* ------------------------------------------------------------------ CRC-32 */

let CRC_TABLE = null;

function crcTable() {
  if (CRC_TABLE) {
    return CRC_TABLE;
  }
  const table = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    table[n] = c;
  }
  CRC_TABLE = table;
  return table;
}

function crc32(buf) {
  const table = crcTable();
  let crc = -1;
  for (let i = 0; i < buf.length; i++) {
    crc = (crc >>> 8) ^ table[(crc ^ buf[i]) & 0xff];
  }
  return (crc ^ -1) >>> 0;
}

/* -------------------------------------------------------------- ZIP writer */

function u16(value) {
  const buf = Buffer.alloc(2);
  buf.writeUInt16LE(value, 0);
  return buf;
}

function u32(value) {
  const buf = Buffer.alloc(4);
  buf.writeUInt32LE(value >>> 0, 0);
  return buf;
}

/** DOS date/time. Fixed timestamp keeps repeated builds byte-stable. */
const DOS_TIME = 0;
const DOS_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1;

/**
 * @param {Array<{name: string, data: Buffer}>} entries
 * @returns {Buffer}
 */
function buildZip(entries) {
  const localParts = [];
  const centralParts = [];
  let offset = 0;

  for (const entry of entries) {
    const nameBytes = Buffer.from(entry.name, "utf8");
    const raw = entry.data;
    const compressed = deflateRawSync(raw, { level: 9 });
    const crc = crc32(raw);

    const localHeader = Buffer.concat([
      u32(0x04034b50),
      u16(20), // version needed to extract
      u16(0x0800), // general purpose flag: UTF-8 file names
      u16(8), // compression method: deflate
      u16(DOS_TIME),
      u16(DOS_DATE),
      u32(crc),
      u32(compressed.length),
      u32(raw.length),
      u16(nameBytes.length),
      u16(0), // extra field length
    ]);
    const localRecord = Buffer.concat([localHeader, nameBytes, compressed]);
    localParts.push(localRecord);

    const centralHeader = Buffer.concat([
      u32(0x02014b50),
      u16(20), // version made by
      u16(20), // version needed to extract
      u16(0x0800), // UTF-8 flag
      u16(8), // compression method
      u16(DOS_TIME),
      u16(DOS_DATE),
      u32(crc),
      u32(compressed.length),
      u32(raw.length),
      u16(nameBytes.length),
      u16(0), // extra field length
      u16(0), // file comment length
      u16(0), // disk number start
      u16(0), // internal attributes
      u32(0), // external attributes
      u32(offset), // relative offset of the local header
    ]);
    centralParts.push(Buffer.concat([centralHeader, nameBytes]));

    offset += localRecord.length;
  }

  const centralDirectory = Buffer.concat(centralParts);
  const eocd = Buffer.concat([
    u32(0x06054b50),
    u16(0), // this disk number
    u16(0), // disk with the central directory
    u16(entries.length),
    u16(entries.length),
    u32(centralDirectory.length),
    u32(offset),
    u16(0), // comment length
  ]);

  return Buffer.concat([...localParts, centralDirectory, eocd]);
}

/* ------------------------------------------------------- ZIP verification */

function findEocd(buf) {
  const min = Math.max(0, buf.length - 65557);
  for (let i = buf.length - 22; i >= min; i--) {
    if (buf.readUInt32LE(i) === 0x06054b50) {
      return i;
    }
  }
  throw new Error("end-of-central-directory record not found");
}

/** Walks the central directory and inflates every entry. Returns a summary. */
function verifyZip(buf) {
  const eocd = findEocd(buf);
  const total = buf.readUInt16LE(eocd + 10);
  let p = buf.readUInt32LE(eocd + 16);
  const entries = [];

  for (let i = 0; i < total; i++) {
    if (buf.readUInt32LE(p) !== 0x02014b50) {
      throw new Error(`bad central directory signature at offset ${p}`);
    }
    const method = buf.readUInt16LE(p + 10);
    const crc = buf.readUInt32LE(p + 16);
    const compressedSize = buf.readUInt32LE(p + 20);
    const rawSize = buf.readUInt32LE(p + 24);
    const nameLen = buf.readUInt16LE(p + 28);
    const extraLen = buf.readUInt16LE(p + 30);
    const commentLen = buf.readUInt16LE(p + 32);
    const localOffset = buf.readUInt32LE(p + 42);
    const name = buf.toString("utf8", p + 46, p + 46 + nameLen);

    const check = buf.readUInt32LE(localOffset);
    if (check !== 0x04034b50) {
      throw new Error(`entry "${name}": missing local file header`);
    }
    const localNameLen = buf.readUInt16LE(localOffset + 26);
    const localExtraLen = buf.readUInt16LE(localOffset + 28);
    const dataStart = localOffset + 30 + localNameLen + localExtraLen;
    const payload = buf.subarray(dataStart, dataStart + compressedSize);

    let content;
    if (method === 0) {
      content = Buffer.from(payload);
    } else if (method === 8) {
      content = inflateRawSync(payload);
    } else {
      throw new Error(`entry "${name}": unsupported compression method ${method}`);
    }

    if (content.length !== rawSize) {
      throw new Error(`entry "${name}": size mismatch (${content.length} != ${rawSize})`);
    }
    if (crc32(content) !== crc) {
      throw new Error(`entry "${name}": CRC mismatch`);
    }

    entries.push({ name, rawSize, compressedSize, content });
    p += 46 + nameLen + extraLen + commentLen;
  }

  if (entries.length !== total) {
    throw new Error(`read ${entries.length} entries out of ${total}`);
  }
  return entries;
}

/* ------------------------------------------------------- content assembly */

const manifest = `<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011" xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">
  <Metadata>
    <Identity Language="en-US" Id="${pkg.name}" Version="${pkg.version}" Publisher="${pkg.publisher}" />
    <DisplayName>${pkg.displayName}</DisplayName>
    <Description xml:space="preserve">${pkg.description}</Description>
    <Tags>shya,dsl,javascript,es2026,macros</Tags>
    <Categories>Programming Languages,Snippets</Categories>
    <GalleryFlags>Public</GalleryFlags>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Manifest" Value="extension/package.json" />
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="${pkg.engines.vscode}" />
      <Property Id="Microsoft.VisualStudio.Code.ExtensionDependencies" Value="" />
      <Property Id="Microsoft.VisualStudio.Code.ExtensionPack" Value="" />
    </Properties>
    <License>extension/LICENSE</License>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code" />
  </Installation>
  <Dependencies />
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true" />
  </Assets>
</PackageManifest>
`;

const contentTypes = `<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension=".json" ContentType="application/json" />
  <Default Extension=".js" ContentType="application/javascript" />
  <Default Extension=".md" ContentType="text/markdown" />
  <Default Extension=".xml" ContentType="text/xml" />
  <Default Extension=".vsixmanifest" ContentType="text/xml" />
  <Default Extension=".tmLanguage" ContentType="application/plist" />
  <Default Extension=".txt" ContentType="text/plain" />
</Types>
`;

/** extension/-relative path -> archive path */
const FILES = [
  "package.json",
  "language-configuration.json",
  "README.md",
  "CHANGELOG.md",
  "LICENSE",
  "out/extension.js",
  "out/formatter.js",
  "out/slot-types.js",
  "syntaxes/shya.tmLanguage.json",
  "snippets/shya.json",
];

function collectEntries() {
  const entries = [];
  const missing = [];

  for (const rel of FILES) {
    const abs = path.join(ROOT, rel);
    if (!fs.existsSync(abs)) {
      missing.push(rel);
      continue;
    }
    entries.push({ name: `extension/${rel}`, data: fs.readFileSync(abs) });
  }

  if (missing.length) {
    throw new Error(`missing files, cannot package: ${missing.join(", ")}`);
  }

  // [Content_Types].xml and the manifest live at the ZIP root; a second copy of the
  // manifest inside extension/ keeps hosts that look for it there happy.
  entries.push({ name: "[Content_Types].xml", data: Buffer.from(contentTypes, "utf8") });
  entries.push({ name: "extension.vsixmanifest", data: Buffer.from(manifest, "utf8") });
  entries.push({ name: "extension/extension.vsixmanifest", data: Buffer.from(manifest, "utf8") });

  return entries;
}

/* ----------------------------------------------------------- static checks */

/** Grammar + pattern checks; also exercises the example sources when they exist. */
function checkGrammar() {
  const samples = [
    path.join(ROOT, "..", "examples", "hello.shya"),
    path.join(ROOT, "..", "examples", "card-game.shya"),
    path.join(ROOT, "..", "tests", "cases", "01-core.shya"),
    path.join(ROOT, "..", "tests", "cases", "02-macros.shya"),
    path.join(ROOT, "..", "tests", "cases", "04-mathlit.shya"),
    path.join(ROOT, "..", "tests", "cases", "07-misc.shya"),
  ];
  runGrammarChecks(path.join(ROOT, "syntaxes", "shya.tmLanguage.json"), samples, (line) => console.log(line));
}

function checkJsonExistence() {
  const jsons = [
    path.join(ROOT, "package.json"),
    path.join(ROOT, "language-configuration.json"),
    path.join(ROOT, "syntaxes", "shya.tmLanguage.json"),
    path.join(ROOT, "snippets", "shya.json"),
  ];
  for (const file of jsons) {
    JSON.parse(fs.readFileSync(file, "utf8"));
  }
  console.log(`ok   ${jsons.length} JSON files parse`);
}

/**
 * Exercise out/extension.js and out/formatter.js. The extension test needs to spawn a
 * child node process, which some sandboxes refuse; a refusal is reported, not fatal.
 */
function checkExtension() {
  const entry = path.join(ROOT, "out", "extension.js");
  if (!fs.existsSync(entry)) {
    throw new Error("out/extension.js is missing");
  }
  if (!fs.existsSync(path.join(ROOT, "out", "formatter.js"))) {
    throw new Error("out/formatter.js is missing");
  }
  JSON.parse(fs.readFileSync(path.join(ROOT, "package.json"), "utf8"));

  for (const runner of ["test/extension.test.mjs", "test/formatter.test.mjs"]) {
    const full = path.join(ROOT, runner);
    if (!fs.existsSync(full)) {
      console.log(`skip ${runner}: not present`);
      continue;
    }
    const result = spawnSync(process.execPath, [full], { cwd: ROOT, stdio: "inherit" });
    if (result.error) {
      console.log(`skip ${runner}: could not spawn node (${result.error.code || result.error.message})`);
      continue;
    }
    if (result.status !== 0) {
      throw new Error(`${runner} failed with exit code ${result.status}`);
    }
  }
}

/* -------------------------------------------------------------- packaging */

function rmIfExists(file) {
  try {
    fs.rmSync(file, { force: true });
  } catch {
    /* ignore */
  }
}

/**
 * Route 1: `npx --yes @vscode/vsce package`.
 *
 * On Windows `npx` resolves to the npx.cmd shim, which spawn() cannot execute without a
 * shell, and shell:true is deprecated for argument safety. So the direct spawn is tried
 * first and the shell form is only used as a retry when the shim itself cannot be found.
 * Whatever happens, a non-zero exit or a missing artefact means "fall back".
 */
function vsceRoute() {
  rmIfExists(VSIX_PATH);
  const args = ["--yes", "@vscode/vsce", "package", "--out", VSIX_NAME];
  console.log("try  npx --yes @vscode/vsce package ...");

  let result = spawnSync("npx", args, { cwd: ROOT, stdio: "inherit" });

  const unusable = result.error && ["ENOENT", "EINVAL", "EPERM", "EACCES"].includes(result.error.code);
  if (unusable) {
    console.log(`info direct npx spawn failed (${result.error.code}); retrying through the shell ...`);
    result = spawnSync("npx", args, { cwd: ROOT, stdio: "inherit", shell: true });
  }

  if (result.error) {
    console.log(`warn npx could not be run: ${result.error.code || ""} ${result.error.message}`.trim());
    return false;
  }
  if (result.status !== 0) {
    console.log(`warn vsce exited with code ${result.status}`);
    return false;
  }
  return fs.existsSync(VSIX_PATH) && fs.statSync(VSIX_PATH).size > 0;
}

function fallbackRoute() {
  console.log("info building the .vsix with the built-in zlib ZIP writer ...");
  const entries = collectEntries();
  const zip = buildZip(entries);
  fs.writeFileSync(VSIX_PATH, zip);
  console.log(`info wrote ${entries.length} entries, ${zip.length} bytes`);
}

/* ------------------------------------------------------------------- main */

function main() {
  console.log(`shya extension packager — ${pkg.name}@${pkg.version}`);
  console.log(`root  ${ROOT}`);

  checkJsonExistence();
  checkExtension();
  checkGrammar();

  let route = "vsce";
  if (!vsceRoute()) {
    route = "fallback";
    rmIfExists(VSIX_PATH);
    fallbackRoute();
  }

  const buf = fs.readFileSync(VSIX_PATH);
  if (buf.length < 4 || buf[0] !== 0x50 || buf[1] !== 0x4b || buf[2] !== 0x03 || buf[3] !== 0x04) {
    throw new Error(`${VSIX_NAME} does not start with PK\\x03\\x04`);
  }
  if (buf.length < 4096) {
    throw new Error(`${VSIX_NAME} is suspiciously small (${buf.length} bytes)`);
  }

  const entries = verifyZip(buf);
  console.log(`ok   ${VSIX_NAME}: ${buf.length} bytes, PK\\x03\\x04, ${entries.length} entries`);
  for (const entry of entries) {
    console.log(`     ${String(entry.rawSize).padStart(7)}  ${String(entry.compressedSize).padStart(7)}  ${entry.name}`);
  }

  const required = [
    "extension/package.json",
    "extension/language-configuration.json",
    "extension/README.md",
    "extension/CHANGELOG.md",
    "extension/LICENSE",
    "extension/out/extension.js",
    "extension/out/formatter.js",
    "extension/out/slot-types.js",
    "extension/syntaxes/shya.tmLanguage.json",
    "extension/snippets/shya.json",
    "[Content_Types].xml",
    "extension.vsixmanifest",
    "extension/extension.vsixmanifest",
  ];
  const names = new Set(entries.map((e) => e.name));
  const absent = required.filter((name) => !names.has(name));
  if (absent.length) {
    throw new Error(`required entries missing from the archive: ${absent.join(", ")}`);
  }

  // The archive's own copy of package.json must round-trip.
  const packaged = JSON.parse(entries.find((e) => e.name === "extension/package.json").content.toString("utf8"));
  if (packaged.name !== pkg.name || packaged.version !== pkg.version) {
    throw new Error("packaged package.json does not match the source manifest");
  }

  console.log("");
  console.log(`done  route: ${route}`);
  console.log(`      ${VSIX_PATH}`);
}

main();
