"use strict";

/**
 * The AST node kind names that double as macro slot types, plus the base slot
 * categories. Kept in sync with `src/macro.cpp` (`astKindTable()`) and
 * documented in `docs/ast-nodes.md`; `node vscode-shya/test/extension.test.mjs`
 * checks the list against that documentation so it cannot drift silently.
 *
 * Matching is case sensitive: `Str` is the type, `strLit` is not. The legacy
 * spellings are listed as well so the completion can still teach them.
 */

/** Base slot categories. */
const BASE_SLOT_TYPES = [
  { name: "expr", detail: "任意表达式（默认）" },
  { name: "stmt", detail: "语句；可整个省略，实参是表达式时自动包成语句" },
  { name: "type", detail: "类型引用" },
  { name: "expr[]", detail: "不定项：一组表达式" },
  { name: "callExpr", detail: "调用片段，如 recover(2)" },
  { name: "safeCallExpr", detail: "安全调用片段，如 d?.recover(2)" },
];

/** AST node kinds usable as slot types, in the compiler's table order. */
const AST_SLOT_TYPES = [
  "Num", "MathConst", "Str", "Tpl", "Bool", "Void", "Ident", "ArrayLit", "ObjectLit",
  "Prop", "Unary", "Binary", "Compare", "Ternary", "Call", "Member", "Index", "Spread",
  "Await", "MacroApply", "TsRaw", "RangeExpr", "Assign", "Decl", "IncDec", "If", "Case",
  "CaseArm", "ForWhile", "ForOf", "ForRange", "FnDecl", "Return", "Throw", "Try", "Break",
  "Continue", "Import", "Export", "Block", "Declare", "ExprStmt",
];

/** Pre-1.0 spellings -> the node kind name that replaced them. */
const LEGACY_SLOT_TYPE_ALIASES = {
  numLit: "Num",
  mathLit: "MathConst",
  strLit: "Str",
  tplLit: "Tpl",
  boolLit: "Bool",
  voidLit: "Void",
  ident: "Ident",
  arrayLit: "ArrayLit",
  objectLit: "ObjectLit",
  prop: "Prop",
  unary: "Unary",
  binary: "Binary",
  compare: "Compare",
  ternary: "Ternary",
  call: "Call",
  member: "Member",
  index: "Index",
  spread: "Spread",
  await: "Await",
  macroApply: "MacroApply",
  tsRaw: "TsRaw",
  rangeExpr: "RangeExpr",
  assign: "Assign",
  decl: "Decl",
  incDec: "IncDec",
  ifStmt: "If",
  caseStmt: "Case",
  caseArm: "CaseArm",
  whileStmt: "ForWhile",
  forOf: "ForOf",
  forRange: "ForRange",
  fnDecl: "FnDecl",
  returnStmt: "Return",
  throwStmt: "Throw",
  tryStmt: "Try",
  breakStmt: "Break",
  continueStmt: "Continue",
  importStmt: "Import",
  exportStmt: "Export",
  block: "Block",
  declareStmt: "Declare",
  exprStmt: "ExprStmt",
};

module.exports = {
  BASE_SLOT_TYPES,
  AST_SLOT_TYPES,
  LEGACY_SLOT_TYPE_ALIASES,
};
