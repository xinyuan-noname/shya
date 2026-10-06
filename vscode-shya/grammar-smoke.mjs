#!/usr/bin/env node
/**
 * grammar-smoke.mjs — structural and behavioural checks for the shya TextMate grammar.
 *
 * TextMate grammars are normally executed by Oniguruma (vscode-textmate). This file does
 * not reimplement Oniguruma; instead it covers the failure modes that actually bite when
 * hand-writing a grammar:
 *
 *   1. JSON shape: every pattern has match/include/begin, every begin has end (or while).
 *   2. Every "#include" resolves against `repository` (typos are silent in VS Code).
 *   3. Every regular expression compiles (as a JS RegExp, which rejects the same class of
 *      syntax errors we care about: unbalanced parens/brackets, bad escapes, bad groups).
 *   4. Targeted behaviour checks: `~/` versus math literals, every required math literal,
 *      numbers, keyword boundaries, the `_` placeholder and slots.
 *   5. A stack-based dry run of the `@ts{ ... }` region on the real example sources, which
 *      proves the braces balance and the embedded region really does close on the outer
 *      brace rather than the first one.
 *
 * Exported as `runGrammarChecks(grammarPath, sampleFiles)`.
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

/* ------------------------------------------------------------- rule index */

/** Flattens a grammar into { label, rule, container } records, including sub-patterns. */
function collectRules(grammar) {
  const rules = [];
  const seen = new Set();

  const walk = (patterns, where) => {
    (patterns || []).forEach((pattern, i) => {
      const at = `${where}[${i}]`;
      rules.push({ label: at, rule: pattern });
      if (pattern && Array.isArray(pattern.patterns)) {
        walk(pattern.patterns, `${at}.patterns`);
      }
    });
  };

  walk(grammar.patterns, "patterns");
  for (const [key, rule] of Object.entries(grammar.repository || {})) {
    // A repository entry that is only a container ("patterns": [...]) is itself
    // matchable only through its children, so it is not a rule in its own right.
    if (!rule.patterns) {
      rules.push({ label: `repository.${key}`, rule });
    }
    walk(rule.patterns, `repository.${key}.patterns`);
    seen.add(key);
  }
  return rules;
}

/** Patterns reachable from the top level, in order, with repository rules expanded. */
function topLevelRules(grammar) {
  const repository = grammar.repository || {};
  const rules = [];
  for (const pattern of grammar.patterns || []) {
    if (pattern.include && pattern.include.startsWith("#")) {
      const key = pattern.include.slice(1);
      for (const inner of (repository[key] || {}).patterns || []) {
        rules.push(inner);
      }
    } else {
      rules.push(pattern);
    }
  }
  return rules;
}

function compileRegex(source, label, problems) {
  try {
    return new RegExp(source);
  } catch (err) {
    problems.push(`${label}: /${source}/ does not compile — ${err.message}`);
    return null;
  }
}

function anchored(re, index) {
  const match = re.exec(index);
  return match && match.index === 0 ? match[0] : null;
}

/* --------------------------------------------------------- targeted checks */

/** Counts every checkAt probe that has been issued. */
let PROBE_COUNT = 0;

function checkAt(rules, text, offset, expectedName, problems, note) {
  PROBE_COUNT += 1;
  for (const rule of rules) {
    if (!rule.match) {
      continue;
    }
    const re = compileRegex(rule.match, `${note} pattern`, problems);
    if (!re) {
      continue;
    }
    const matched = anchored(re, text.slice(offset));
    if (matched !== null) {
      if (rule.name !== expectedName) {
        problems.push(`${note}: expected ${expectedName}, got ${rule.name} (matched ${JSON.stringify(matched)})`);
      }
      return matched;
    }
  }
  problems.push(`${note}: nothing matched at offset ${offset} of ${JSON.stringify(text)}`);
  return null;
}

function checkMathLiterals(rules, problems) {
  const positives = [
    "~pi",
    "~e",
    "~tau",
    "~inf",
    "~lg10",
    "~ln4",
    "~db10",
    "~deg2",
    "~rad2",
    "~sqrt2",
    "~ln~deg360",
    "~deg~e",
    "~π",
    "~ℯ",
    "~τ",
    "~∞",
  ];
  for (const literal of positives) {
    const matched = checkAt(
      rules,
      `${literal} `,
      0,
      "constant.numeric.math.shya",
      problems,
      `math literal ${literal}`,
    );
    if (matched !== literal) {
      problems.push(`math literal ${literal}: matched ${JSON.stringify(matched)} instead of the whole literal`);
    }
  }

  // ~/ is the truncating-division operator and must never be read as a math literal.
  const slash = checkAt(rules, "9 ~/ 2", 2, "keyword.operator.arithmetic.shya", problems, "operator ~/");
  if (slash !== "~/") {
    problems.push(`operator ~/: matched ${JSON.stringify(slash)}`);
  }

  // ~pi2 must not be swallowed as a prefix of an identifier.
  for (const rule of rules) {
    if (rule.name !== "constant.numeric.math.shya" || !rule.match) {
      continue;
    }
    const re = new RegExp(rule.match);
    if (anchored(re, "~pi2") !== null) {
      problems.push("math literal ~pi2 was matched; the trailing guard is missing");
    }
  }
}

function checkNumbers(rules, problems) {
  const cases = [
    ["1_000_000", "constant.numeric.decimal.shya"],
    ["0x1f", "constant.numeric.hex.shya"],
    ["0b1010", "constant.numeric.binary.shya"],
    ["0o17", "constant.numeric.octal.shya"],
    ["1.5e-3", "constant.numeric.float.shya"],
    ["3.14", "constant.numeric.float.shya"],
  ];
  for (const [text, expected] of cases) {
    checkAt(rules, `${text} `, 0, expected, problems, `number ${text}`);
  }
}

function checkKeywords(rules, problems) {
  const cases = [
    ["if", "keyword.control.conditional.shya"],
    ["elif", "keyword.control.conditional.shya"],
    ["else", "keyword.control.conditional.shya"],
    ["case", "keyword.control.conditional.shya"],
    ["default", "keyword.control.conditional.shya"],
    ["fallthrough", "keyword.control.conditional.shya"],
    ["for", "keyword.control.loop.shya"],
    ["of", "keyword.control.loop.shya"],
    ["break", "keyword.control.loop.shya"],
    ["continue", "keyword.control.loop.shya"],
    ["let", "storage.type.shya"],
    ["const", "storage.type.shya"],
    ["define", "storage.type.shya"],
    ["macro", "storage.type.shya"],
    ["async", "storage.modifier.async.shya"],
    ["await", "keyword.control.flow.shya"],
    ["return", "keyword.control.flow.shya"],
    ["throw", "keyword.control.flow.shya"],
    ["try", "keyword.control.flow.shya"],
    ["catch", "keyword.control.flow.shya"],
    ["finally", "keyword.control.flow.shya"],
    ["import", "keyword.control.import-export.shya"],
    ["from", "keyword.control.import-export.shya"],
    ["export", "keyword.control.import-export.shya"],
    ["as", "keyword.control.import-export.shya"],
    ["is", "keyword.operator.type.shya"],
    ["not", "keyword.operator.type.shya"],
    ["instanceof", "keyword.operator.type.shya"],
    ["typeof", "keyword.operator.type.shya"],
    ["new", "keyword.operator.type.shya"],
    ["true", "constant.language.boolean.shya"],
    ["false", "constant.language.boolean.shya"],
    ["this", "variable.language.this.shya"],
    ["void", "constant.language.void.shya"],
  ];
  for (const [text, expected] of cases) {
    checkAt(rules, `${text}`, 0, expected, problems, `keyword ${text}`);
  }
}

function checkPlaceholdersAndSlots(rules, problems) {
  checkAt(rules, "_ @banner", 0, "variable.language.wildcard.shya", problems, "placeholder _");
  checkAt(rules, "macro @len(#x)", 11, "meta.slot.declaration.shya", problems, "slot declaration #x");
  checkAt(rules, "#red: console", 0, "meta.slot.label.shya", problems, "slot label #red:");
  checkAt(rules, "#x.length", 0, "meta.slot.reference.shya", problems, "slot reference #x");
  checkAt(rules, "@ts{1}", 0, "meta.macro.invocation.shya", problems, "macro @ts");
  checkAt(rules, "@len ", 0, "meta.macro.invocation.shya", problems, "macro @len");
}

/* --------------------------------------------------- @ts nesting dry run */

/**
 * Walks the text the way TextMate does, tracking the rule stack and recording which rule
 * begins or ends at which offset.
 *
 * Rules without an `end` pattern (the ones that only have a `name`, e.g. a synthesized
 * child standing in for a source.js rule) are treated as closing at the next `}` that is
 * not itself part of a longer run of closing braces. That mirrors how a brace-terminated
 * JavaScript rule behaves and keeps the simulation from stalling.
 */
function runStack(text, rules) {
  const stack = [];
  const events = [];
  let line = 1;
  let i = 0;

  const top = () => (stack.length ? stack[stack.length - 1] : null);
  const advanceTo = (j) => {
    for (let k = i; k < j; k++) {
      if (text[k] === "\n") {
        line += 1;
      }
    }
    i = j;
  };
  const openRule = (rule, length, kind) => {
    stack.push({ rule });
    events.push({ line, offset: i, kind, rule });
    advanceTo(i + length);
  };
  const closeRule = (length) => {
    const active = stack.pop();
    events.push({ line, offset: i, kind: "end", rule: active.rule });
    advanceTo(i + length);
  };
  const skip = () => {
    if (text[i] === "\n") {
      line += 1;
    }
    i += 1;
  };

  while (i < text.length) {
    const active = top();
    const rest = text.slice(i);

    if (active && active.endRe) {
      const endMatch = active.endRe.exec(rest);
      const endLen = endMatch && endMatch.index === 0 ? endMatch[0].length : 0;
      const child = firstBegin(active.rule.patterns, rest);

      if (child && (endLen === 0 || child.length >= endLen)) {
        openRule(child.rule, child.length, "begin-child");
      } else if (endLen > 0) {
        closeRule(endLen);
      } else if (child) {
        openRule(child.rule, child.length, "begin-child");
      } else {
        skip();
      }
      continue;
    }

    if (active && !active.endRe) {
      const child = firstBegin(active.rule.patterns, rest);
      const nextClose = rest.indexOf("}");
      const closeLen = nextClose === 0 ? (rest[1] === "}" ? 0 : 1) : 0;

      if (closeLen > 0) {
        closeRule(1);
      } else if (child) {
        openRule(child.rule, child.length, "begin-child");
      } else {
        skip();
      }
      continue;
    }

    const child = firstBegin(rules, rest);
    if (child) {
      openRule(child.rule, child.length, "begin");
    } else {
      skip();
    }
  }
  return { events, open: stack.length };
}

function firstBegin(rules, rest) {
  for (const rule of rules || []) {
    if (!rule.begin) {
      continue;
    }
    const re = new RegExp(rule.begin);
    const match = re.exec(rest);
    if (match && match.index === 0) {
      return { rule, length: match[0].length };
    }
  }
  return null;
}

function withEndRegex(rules) {
  return (rules || []).map((rule) => {
    if (!rule.begin || !rule.end) {
      return rule;
    }
    return { ...rule, endRe: new RegExp(rule.end) };
  });
}

/**
 * Finds every `@ts{ ... }` region in a source file and returns, for each one, the index
 * of the `}` that a brace counter says closes it.
 */
function tsBlocks(text) {
  const blocks = [];
  const re = /@ts\s*\{/g;
  let match;
  while ((match = re.exec(text)) !== null) {
    let depth = 0;
    let j = match.index + match[0].length - 1;
    for (; j < text.length; j++) {
      if (text[j] === "{") {
        depth += 1;
      } else if (text[j] === "}") {
        depth -= 1;
        if (depth === 0) {
          break;
        }
      }
    }
    blocks.push({ open: match.index, close: j, spansEndOfInput: j >= text.length });
    re.lastIndex = j + 1;
  }
  return blocks;
}

function checkTsBlocks(grammar, sampleFiles, problems, log) {
  const tsRules = (grammar.repository && grammar.repository["ts-blocks"] && grammar.repository["ts-blocks"].patterns) || [];
  const tsRule = tsRules.find((p) => p.begin && p.name === "meta.embedded.block.javascript.shya");

  if (!tsRule) {
    problems.push("@ts: no meta.embedded.block.javascript.shya rule found");
    return;
  }
  // The end pattern is the single brace, wrapped in a capture group so endCaptures has a
  // group to refer to. A recursive \g<0> form (which would need engine support) is not used.
  const singleBraceEnd = "(\\})";
  if (tsRule.end !== singleBraceEnd) {
    problems.push(`@ts: end pattern is ${JSON.stringify(tsRule.end)}, expected ${JSON.stringify(singleBraceEnd)}`);
  }

  // 1. A synthetic nested case: the block must close on the *outer* brace.
  const synthetic = "let d = @ts{{ say: (m) => console.log(m) }}\nlet e = 1\n";
  const syntheticBlocks = tsBlocks(synthetic);
  const syntheticRun = runStack(synthetic, withEndRegex([tsRule]));
  if (syntheticRun.open !== 0) {
    problems.push(`@ts synthetic: ${syntheticRun.open} rule(s) still open at end of input`);
  }
  const syntheticEnds = syntheticRun.events.filter((e) => e.kind === "end");
  const syntheticClosedAt = synthetic.indexOf("}}") + 1;
  if (!syntheticEnds.length) {
    problems.push("@ts synthetic: the begin rule never ended");
  } else if (syntheticEnds[syntheticEnds.length - 1].offset !== syntheticClosedAt) {
    problems.push(
      `@ts synthetic: the block closed at offset ${syntheticEnds[syntheticEnds.length - 1].offset}, expected ${syntheticClosedAt} (the outer brace)`,
    );
  }
  if (syntheticBlocks.length !== 1 || syntheticBlocks[0].close !== syntheticClosedAt) {
    problems.push("@ts synthetic: brace counter disagrees with the dry run");
  }
  log(
    `ok   @ts dry run (synthetic): 1 block, closes on the outer brace at offset ${syntheticClosedAt} of ${synthetic.length}`,
  );

  // 2. Every real example/test source: one close event per balanced @ts block.
  for (const file of sampleFiles) {
    if (!fs.existsSync(file)) {
      log(`skip @ts dry run: ${file} not found`);
      continue;
    }
    const text = fs.readFileSync(file, "utf8");
    const blocks = tsBlocks(text);
    const run = runStack(text, withEndRegex([tsRule]));
    const ends = run.events.filter((e) => e.kind === "end");

    if (run.open !== 0) {
      problems.push(`@ts dry run on ${path.basename(file)}: ${run.open} rule(s) left open`);
    }
    if (ends.length !== blocks.length) {
      problems.push(
        `@ts dry run on ${path.basename(file)}: source has ${blocks.length} balanced @ts block(s) but the dry run produced ${ends.length} close event(s)`,
      );
    }
    for (const block of blocks) {
      if (block.spansEndOfInput) {
        problems.push(`@ts on ${path.basename(file)}: unbalanced braces in the block starting at offset ${block.open}`);
      }
    }
    log(`ok   @ts dry run ${path.basename(file)}: ${blocks.length} block(s), ${ends.length} close event(s)`);
  }
}

/* ------------------------------------------------------------------ entry */

export function runGrammarChecks(grammarPath, sampleFiles = [], log = console.log) {
  const problems = [];
  const grammar = JSON.parse(fs.readFileSync(grammarPath, "utf8"));

  if (grammar.scopeName !== "source.shya") {
    problems.push(`scopeName is "${grammar.scopeName}", expected "source.shya"`);
  }

  // 1 + 2 + 3: shape, include resolution, regex compilation.
  const allRules = collectRules(grammar);
  const repository = grammar.repository || {};
  let beginCount = 0;
  let matchCount = 0;
  let includeCount = 0;

  for (const { label, rule } of allRules) {
    if (!rule || typeof rule !== "object") {
      problems.push(`${label}: not an object`);
      continue;
    }
    const hasMatch = typeof rule.match === "string";
    const hasInclude = typeof rule.include === "string";
    const hasBegin = typeof rule.begin === "string";
    if (!hasMatch && !hasInclude && !hasBegin) {
      problems.push(`${label}: has neither match, include nor begin`);
      continue;
    }
    if (hasMatch) {
      matchCount += 1;
      compileRegex(rule.match, `${label}.match`, problems);
    }
    if (hasInclude) {
      includeCount += 1;
      if (rule.include.startsWith("#")) {
        const key = rule.include.slice(1);
        if (key !== "root" && !(key in repository)) {
          problems.push(`${label}: include "#${key}" does not resolve`);
        }
      }
    }
    if (hasBegin) {
      beginCount += 1;
      compileRegex(rule.begin, `${label}.begin`, problems);
      if (typeof rule.end === "string") {
        compileRegex(rule.end, `${label}.end`, problems);
      } else if (typeof rule.while !== "string") {
        problems.push(`${label}: begin without end (and without while)`);
      }
    }
    if (typeof rule.end === "string" && !hasBegin) {
      problems.push(`${label}: has an end but no begin`);
    }
  }

  log(`ok   grammar shape: ${allRules.length} rules (${beginCount} begin/end, ${matchCount} match, ${includeCount} include)`);

  // 4: behaviour.
  const rules = topLevelRules(grammar);
  PROBE_COUNT = 0;
  checkMathLiterals(rules, problems);
  checkNumbers(rules, problems);
  checkKeywords(rules, problems);
  checkPlaceholdersAndSlots(rules, problems);
  log(
    `ok   token probes: ${PROBE_COUNT} probes (math literals incl. nesting and ~/, numbers, keywords, slots, placeholders)`,
  );

  // 5: @ts nesting.
  checkTsBlocks(grammar, sampleFiles, problems, log);

  if (problems.length) {
    throw new Error(`grammar smoke test failed:\n  - ${problems.join("\n  - ")}`);
  }
  log("ok   grammar smoke test passed");
  return { rules: allRules.length, beginCount, matchCount, includeCount };
}

/* Allow standalone use: node grammar-smoke.mjs [sample.shya ...] */
const invokedDirectly = process.argv[1] && process.argv[1].replace(/\\/g, "/").endsWith("grammar-smoke.mjs");
if (invokedDirectly) {
  const here = path.dirname(fileURLToPath(import.meta.url));
  const grammarPath = path.join(here, "syntaxes", "shya.tmLanguage.json");
  runGrammarChecks(grammarPath, process.argv.slice(2));
}
