#pragma once
#include <cstddef>
#include <cstring>
#include <cctype>

// Incremental parser: retain one HTML tag, never the full flight page.
class FlightAwareRoute {
 public:
  char from[5] = {}, to[5] = {};
  bool complete() const { return from[0] && to[0]; }
  void feed(char c) {
    if (!inside_) {
      if (c == '<') { inside_ = true; used_ = 0; quote_ = 0; }
      return;
    }
    // Ignore non-meta tags immediately, including scripts whose text can
    // contain comparison operators and unmatched quotes.
    if (used_ < 4 && c != "meta"[used_]) {
      inside_ = false;
      return;
    }
    if (!quote_ && c == '>') {
      tag_[used_] = 0;
      if (!overflow_) parse();
      inside_ = overflow_ = false;
      return;
    }
    if (c == quote_) quote_ = 0;
    else if (!quote_ && (c == '\'' || c == '"')) quote_ = c;
    if (used_ + 1 < sizeof(tag_)) tag_[used_++] = c;
    else overflow_ = true;
  }
 private:
  void parse() {
    const char* p = tag_;
    if (std::strncmp(p, "meta", 4) || !std::isspace(static_cast<unsigned char>(p[4]))) return;
    p += 4;
    char name[16] = {}, content[16] = {};
    while (*p) {
      while (std::isspace(static_cast<unsigned char>(*p))) ++p;
      const char* key = p;
      while (*p && *p != '=' && !std::isspace(static_cast<unsigned char>(*p))) ++p;
      const size_t keylen = p - key;
      while (std::isspace(static_cast<unsigned char>(*p))) ++p;
      if (*p != '=') { if (*p) ++p; continue; }
      ++p;
      while (std::isspace(static_cast<unsigned char>(*p))) ++p;
      const char quote = (*p == '\'' || *p == '"') ? *p++ : 0;
      const char* value = p;
      while (*p && (quote ? *p != quote : !std::isspace(static_cast<unsigned char>(*p)))) ++p;
      const size_t len = p - value;
      char* out = keylen == 4 && !std::strncmp(key, "name", 4) ? name :
                  keylen == 7 && !std::strncmp(key, "content", 7) ? content : nullptr;
      if (out && len < 16) { std::memcpy(out, value, len); out[len] = 0; }
      if (quote && *p) ++p;
    }
    const size_t len = std::strlen(content);
    if (len < 3 || len > 4) return;
    for (size_t i = 0; i < len; ++i)
      if (!((content[i] >= 'A' && content[i] <= 'Z') || (content[i] >= '0' && content[i] <= '9'))) return;
    if (!std::strcmp(name, "origin")) std::strcpy(from, content);
    if (!std::strcmp(name, "destination")) std::strcpy(to, content);
  }
  char tag_[512] = {};
  size_t used_ = 0;
  char quote_ = 0;
  bool inside_ = false, overflow_ = false;
};
