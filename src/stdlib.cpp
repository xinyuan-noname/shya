// shya - the embedded standard library, written in shya itself.
//
// This is deliberately the same text that the design document specifies for the
// standard-library macros, with two normalisations recorded in
// docs/decisions.md:
//   * stray `,` separators inside `@when` arms are dropped;
//   * `@safe_share`'s trailing `?.` on the argument list is dropped, so all
//     three chain macros insert exactly one `?.` per chain link (matching the
//     design document's own `d?.say("hello")?.say("nice")` example).
//
// Keep this file byte-for-byte identical to src/stdlib.cpp's kStdlibSource.
#include "shya.h"

namespace shya {

const char* kStdlibSource = R"SHYA(
// ---------------------------------------------------------------- 标准库宏 ---
// @keys / @values / @entries
macro @keys(#x) {
  @ts{Object.keys(#x)}
}

macro @values(#x) {
  @ts{Object.values(#x)}
}

macro @entries(#x) {
  @ts{Object.entries(#x)}
}

// @len: array / string -> .length, map / set -> .size
macro @len(#x) {
  @when(#x is array || #x is string) @ts{#x.length}
  @when(#x is map || #x is set) @ts{#x.size}
}

// @safe: d @safe say("hello") say("nice")  ->  d?.say("hello")?.say("nice")
macro @safe(#x, ...#slots: callExpr) {
  #x @each(#slot of #slots) {
    @when(#slot is safeCallExpr) {
      y: #slot
      n: ?#slot
    }
  }
}

// @share: player nextSeat @share _p recover(2) draw(2)
macro @share(#x, #y, ...#slots: callExpr) {
  #y = #x
  @each(#slot of #slots) {
    #y #slot
  }
}

// @safe_share: like @share, but every chain link is optional
macro @safe_share(#x, #y, ...#slots: callExpr) {
  #y = #x
  @each(#slot of #slots) {
    #y @when(#slot is safeCallExpr) {
      y: #slot
      n: ?#slot
    }
  }
}
)SHYA";

}  // namespace shya
