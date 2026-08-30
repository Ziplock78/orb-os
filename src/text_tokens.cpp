#include "text_tokens.h"
#include <string.h>

namespace text_tokens {

void expand(char *out, size_t outSz, const char *fmt, const Tok *toks, size_t nToks) {
    if (!out || outSz == 0) return;
    if (!fmt) { out[0] = 0; return; }
    size_t oi = 0;
    for (const char *p = fmt; *p && oi + 1 < outSz; ) {
        if (*p == '{') {
            const char *close = strchr(p, '}');
            if (close) {
                // 16 is the buffer, so the longest key this can match is 15 characters.
                // Every token any screen ships is well inside that; a longer one would
                // fall through and be copied literally, which is visible rather than silent.
                char key[16]; size_t klen = (size_t)(close - p - 1);
                if (klen > 0 && klen < sizeof(key)) {
                    memcpy(key, p + 1, klen); key[klen] = 0;
                    const char *val = "";
                    for (size_t i = 0; i < nToks; ++i) {
                        if (!strcmp(toks[i].key, key)) { val = toks[i].val; break; }
                    }
                    for (const char *v = val; *v && oi + 1 < outSz; ++v) out[oi++] = *v;
                    p = close + 1;
                    continue;
                }
            }
        }
        out[oi++] = *p++;
    }
    out[oi] = 0;
}

}  // namespace text_tokens
