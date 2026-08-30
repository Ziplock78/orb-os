#pragma once
#include <stddef.h>

// {token} substitution for themeable text, shared by every screen that has any.
//
// A theme writes a format string like "{temp}  {cond}" and the screen supplies a table
// saying what those names mean right now. This engine is the only thing that knows how to
// walk the string; the screens only ever build tables.
//
// It lived inside radar_view.cpp as a static, which was fine while the Flight Tracker was
// the only screen with live text. The Weather map got four slots of its own at THEME_CAPS
// 28, and a second copy of a parser is how two screens quietly disagree about what an
// unclosed brace does. So: one parser, many tables.
//
// An unknown token expands to nothing rather than to its own name. A designer who typos
// {tmp} sees a gap, which reads as a mistake; leaving "{tmp}" on the glass reads as the
// firmware being broken.
namespace text_tokens {

struct Tok { const char *key; const char *val; };

// Writes at most outSz-1 characters plus a terminator. A brace with no closing partner is
// copied through literally, so a design that genuinely wants a { can have one.
void expand(char *out, size_t outSz, const char *fmt, const Tok *toks, size_t nToks);

}  // namespace text_tokens
