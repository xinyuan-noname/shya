"use strict";

/**
 * shya formatter — pure JavaScript, no `vscode`, no dependencies.
 *
 * The hard constraint: **shya uses line breaks as statement separators**, so a formatter
 * must never join two lines or split one. It also must not touch the inside of strings,
 * template literals, comments or `@ts{…}` blocks — re-indenting those would change string
 * values or raw JavaScript.
 *
 * So this formatter only ever does four things:
 *   1. re-indents each line from its brace/bracket depth,
 *   2. collapses runs of spaces between tokens on a line down to one,
 *   3. drops the space before `,` `;` `)` `]`,
 *   4. trims trailing whitespace and collapses blank-line runs.
 *
 * Every one of those is a whitespace-only edit that cannot change what the program means.
 */

const PUNCTS = [
  "...", "**=", "===", "!==", "~/", "\\/", "+/", "-/", "==", "!=",
  "<=", ">=", "&&", "||", "++", "--", "+=", "-=", "*=", "/=",
  "%=", "^=", "=>", "->", "**", "(", ")", "[", "]", "{",
  "}", ",", ":", ";", ".", "=", "+", "-", "*", "/",
  "%", "<", ">", "!", "?", "^", "&", "|", "~", "\\",
];

const KEYWORDS = new Set([
  "if", "elif", "else", "case", "default", "fallthrough",
  "for", "of", "break", "continue", "let", "const",
  "define", "declare", "macro", "import", "from", "export",
  "as", "fn", "async", "await", "return", "throw",
  "try", "catch", "finally", "is", "not", "instanceof",
  "void", "true", "false", "this", "typeof", "new",
  "null", "undefined",
]);

/** Punctuation that never wants a space in front of it. */
const NO_SPACE_BEFORE = new Set([",", ";", ")", "]"]);

function isIdentStart(code) {
  return (
    (code >= 97 && code <= 122) || // a-z
    (code >= 65 && code <= 90) || // A-Z
    code === 95 || // _
    code === 36 || // $
    code >= 128 // non-ASCII (CJK identifiers, π …)
  );
}

function isIdentPart(code) {
  return isIdentStart(code) || (code >= 48 && code <= 57);
}

function isDigit(code) {
  return code >= 48 && code <= 57;
}

/**
 * Tokenizes shya into `{ text, type, protected, nlBefore, hadSpace }`.
 *
 * `protected` marks regions whose bytes must be emitted verbatim: strings, template
 * literals, comments and `@ts{…}` blocks.
 */
function tokenize(src) {
  const tokens = [];
  const n = src.length;
  let i = 0;
  let nlBefore = 0;
  let sawSpace = false;

  const push = (text, type, isProtected) => {
    tokens.push({
      text,
      type,
      protected: Boolean(isProtected),
      nlBefore,
      hadSpace: nlBefore === 0 && sawSpace,
    });
    nlBefore = 0;
    sawSpace = false;
  };

  const skipTrivia = () => {
    for (;;) {
      const c = src[i];
      if (c === " " || c === "\t" || c === "\r") {
        sawSpace = true;
        i += 1;
        continue;
      }
      if (c === "\n") {
        nlBefore += 1;
        sawSpace = false;
        i += 1;
        continue;
      }
      break;
    }
  };

  const readString = (quote) => {
    const start = i;
    i += 1;
    while (i < n) {
      const c = src[i];
      if (c === "\\") {
        i += 2;
        continue;
      }
      if (c === quote) {
        i += 1;
        break;
      }
      if (c === "\n") {
        break; // unterminated: stop at the newline, the compiler reports it
      }
      i += 1;
    }
    return src.slice(start, i);
  };

  const readTemplate = () => {
    const start = i;
    i += 1;
    while (i < n) {
      const c = src[i];
      if (c === "\\") {
        i += 2;
        continue;
      }
      if (c === "`") {
        i += 1;
        break;
      }
      i += 1;
    }
    return src.slice(start, i);
  };

  const readBlockComment = () => {
    const start = i;
    i += 2;
    let depth = 1;
    while (i < n && depth > 0) {
      if (src[i] === "/" && src[i + 1] === "*") {
        depth += 1;
        i += 2;
      } else if (src[i] === "*" && src[i + 1] === "/") {
        depth -= 1;
        i += 2;
      } else {
        i += 1;
      }
    }
    return src.slice(start, i);
  };

  /** `@ts{ … }` — balanced braces, string and comment aware. */
  const readTsBlock = () => {
    const start = i;
    i += 1; // @
    while (i < n && isIdentPart(src.charCodeAt(i))) i += 1;
    while (src[i] === " " || src[i] === "\t") i += 1;
    if (src[i] !== "{") {
      i = start + 1;
      return null;
    }
    i += 1;
    let depth = 1;
    while (i < n && depth > 0) {
      const c = src[i];
      if (c === '"' || c === "'") {
        i += 1;
        while (i < n) {
          const d = src[i];
          if (d === "\\") {
            i += 2;
            continue;
          }
          i += 1;
          if (d === c) break;
        }
        continue;
      }
      if (c === "`") {
        i += 1;
        while (i < n) {
          const d = src[i];
          if (d === "\\") {
            i += 2;
            continue;
          }
          i += 1;
          if (d === "`") break;
        }
        continue;
      }
      if (c === "/" && src[i + 1] === "/") {
        while (i < n && src[i] !== "\n") i += 1;
        continue;
      }
      if (c === "/" && src[i + 1] === "*") {
        readBlockComment();
        continue;
      }
      if (c === "{") depth += 1;
      if (c === "}") {
        depth -= 1;
        if (depth === 0) {
          i += 1;
          break;
        }
      }
      i += 1;
    }
    return src.slice(start, i);
  };

  while (i < n) {
    skipTrivia();
    if (i >= n) break;

    const before = i;
    const c = src[i];

    if (c === "/" && src[i + 1] === "/") {      const start = i;
      while (i < n && src[i] !== "\n") i += 1;
      push(src.slice(start, i), "comment", true);
      continue;
    }
    if (c === "/" && src[i + 1] === "*") {
      push(readBlockComment(), "comment", true);
      continue;
    }
    if (c === '"' || c === "'") {
      push(readString(c), "string", true);
      continue;
    }
    if (c === "`") {
      push(readTemplate(), "template", true);
      continue;
    }
    if (c === "@") {
      const ts = readTsBlock();
      if (ts !== null) {
        push(ts, "tsblock", true);
        continue;
      }
      push("@", "punct", false);
      continue;
    }
    if (c === "#") {
      i += 1;
      push("#", "punct", false);
      continue;
    }
    if (isDigit(c.charCodeAt(0)) || (c === "." && isDigit(src.charCodeAt(i + 1)))) {
      const start = i;
      while (i < n && (isIdentPart(src.charCodeAt(i)) || src[i] === ".")) i += 1;
      push(src.slice(start, i), "number", false);
      continue;
    }
    if (isIdentStart(c.charCodeAt(0))) {
      const start = i;
      while (i < n && isIdentPart(src.charCodeAt(i))) i += 1;
      const text = src.slice(start, i);
      if (text === "_" && src[i] !== "_") {
        push(text, "under", false);
      } else if (KEYWORDS.has(text)) {
        push(text, "keyword", false);
      } else {
        push(text, "ident", false);
      }
      continue;
    }
    if (c === "~") {
      const start = i;
      i += 1;
      if (src[i] === "/") {
        i += 1; // the `~/` operator, not a math literal
      } else {
        while (i < n && isIdentPart(src.charCodeAt(i))) i += 1;
        while (i < n && (isDigit(src.charCodeAt(i)) || src[i] === ".")) i += 1;
        while (src[i] === "~") {
          i += 1;
          while (i < n && isIdentPart(src.charCodeAt(i))) i += 1;
          while (i < n && (isDigit(src.charCodeAt(i)) || src[i] === ".")) i += 1;
        }
      }
      push(src.slice(start, i), "mathlit", false);
      continue;
    }

    let matched = null;
    for (const p of PUNCTS) {
      if (src.startsWith(p, i)) {
        matched = p;
        break;
      }
    }
    if (matched) {
      i += matched.length;
      push(matched, "punct", false);
      continue;
    }

    // Unknown byte: keep it so the formatter is lossless.
    push(src[i], "unknown", true);
    i += 1;

    if (i === before) {
      // Belt and braces: never let a tokenizer branch spin forever.
      i += 1;
    }
  }

  return tokens;
}

function clampInt(value, fallback, min, max) {
  const n = Number(value);
  if (!Number.isFinite(n)) return fallback;
  return Math.max(min, Math.min(max, Math.trunc(n)));
}

/**
 * Formats shya source. Whitespace-only: the token sequence is preserved exactly, so
 * compilation output is unchanged (see test/formatter.test.mjs for the round-trip check).
 */
function formatShya(src, options) {
  const opts = options || {};
  const indentSize = clampInt(opts.indentSize, 2, 0, 8);
  const maxBlankLines = clampInt(opts.maxBlankLines, 1, 0, 5);
  const text = String(src).replace(/\r\n?/g, "\n");
  const tokens = tokenize(text);

  let out = "";
  let depth = 0;
  let atLineStart = true;

  for (let k = 0; k < tokens.length; k += 1) {
    const tk = tokens[k];

    if (k > 0 && tk.nlBefore > 0) {
      out += "\n".repeat(Math.min(tk.nlBefore, 1 + maxBlankLines));
      atLineStart = true;
    }

    if (atLineStart) {
      const closes = !tk.protected && /^[)\]}]$/.test(tk.text);
      const lineDepth = Math.max(0, depth - (closes ? 1 : 0));
      out += " ".repeat(indentSize * lineDepth);
      atLineStart = false;
    } else if (tk.hadSpace && !NO_SPACE_BEFORE.has(tk.text)) {
      out += " ";
    }

    out += tk.text;

    if (!tk.protected) {
      for (const ch of tk.text) {
        if (ch === "{" || ch === "(" || ch === "[") depth += 1;
        else if (ch === "}" || ch === ")" || ch === "]") depth = Math.max(0, depth - 1);
      }
    }

    if (tk.text.indexOf("\n") !== -1) atLineStart = false;
  }

  return out.replace(/\s+$/, "") + "\n";
}

module.exports = { formatShya, tokenize };
