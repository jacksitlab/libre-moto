/*
 * nav_message_parse_impl.h — C++ implementation of parse_nav_message()
 *
 * A small, self-contained JSON-object parser tuned for the NavData
 * protocol (docs/protocol.md §2). Not a general-purpose parser:
 * it validates structure (objects, arrays, strings, numbers, booleans,
 * null) and extracts the known keys.
 *
 *   - No exceptions, no dynamic allocation. All loops bounded by `len`.
 *   - Strings: full escape support (\\" \\\\ \\/ \b \f \n \r \t, \uXXXX
 *     incl. UTF-16 surrogate pairs → UTF-8); output is NUL-terminated
 *     and truncated to the caller buffer (no overflow by construction).
 *   - Unknown keys are skipped. Malformed input → false, out unchanged.
 *   - No trailing garbage allowed after the closing brace.
 *
 * Host-tested (firmware/test/test_nav_message.cpp, C++11).
 */

#ifndef LIBREMOTO_NAV_MESSAGE_PARSE_IMPL_H
#define LIBREMOTO_NAV_MESSAGE_PARSE_IMPL_H

#include <stdlib.h> /* strtod for exponent / decimal support */
#include <errno.h>

namespace navmsg {

struct Cursor {
  const char *p;
  const char *end;
};

inline bool skip_ws(Cursor &c) {
  while (c.p < c.end && (*c.p == ' ' || *c.p == '\t' || *c.p == '\r' || *c.p == '\n'))
    ++c.p;
  return c.p < c.end;
}

/* ---------- string decoding ---------- */

static inline int hex_val(int ch) {
  ch = (unsigned char)ch;
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

/* Append one UTF-8 code point, truncated to capacity. */
static inline void put_cp(char *buf, size_t *n, size_t cap, unsigned cp) {
  if (cp < 0x80) {
    if (*n + 1 < cap) buf[(*n)++] = (char)cp;
  } else if (cp < 0x800) {
    if (*n + 2 < cap) { buf[(*n)++] = (char)(0xC0 | (cp >> 6)); buf[(*n)++] = (char)(0x80 | (cp & 0x3F)); }
  } else if (cp < 0x10000) {
    if (*n + 3 < cap) {
      buf[(*n)++] = (char)(0xE0 | (cp >> 12));
      buf[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
  } else if (cp < 0x110000) {
    if (*n + 4 < cap) {
      buf[(*n)++] = (char)(0xF0 | (cp >> 18));
      buf[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      buf[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
  }
  buf[*n] = 0;
}

static inline bool read_hex4(Cursor &c, unsigned &out) {
  if (!skip_ws(c)) return false; /* no ws allowed inside \uXXXX, but be strict */
  if (c.end - c.p < 4) return false;
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    int h = hex_val(c.p[i]);
    if (h < 0) return false;
    v = (v << 4) | (unsigned)h;
  }
  c.p += 4;
  out = v;
  return true;
}

/* Input string MUST be well-formed UTF-8; copy raw bytes, decoding escapes.
 * c.p points AT the opening quote. Returns false on any error. */
inline bool parse_string(Cursor &c, char *buf, size_t cap) {
  if (!buf || cap == 0 || c.p >= c.end || *c.p != '"') return false;
  buf[0] = 0;
  size_t n = 0;
  ++c.p;
  while (c.p < c.end) {
    unsigned char ch = (unsigned char)*c.p;
    if (ch == '"') { ++c.p; return true; }
    if (ch < 0x20) return false; /* raw control character */
    if (ch != '\\') {
      if (n + 1 < cap) { buf[n] = (char)ch; n++; buf[n] = 0; }
      ++c.p;
      continue;
    }
    /* escaped sequence */
    ++c.p;
    if (c.p >= c.end) return false;
    char e = *c.p++;
    switch (e) {
      case '"':  put_cp(buf, &n, cap, 0x22);  continue;
      case '\\': put_cp(buf, &n, cap, 0x5C);  continue;
      case '/':  put_cp(buf, &n, cap, 0x2F);  continue;
      case 'b':  put_cp(buf, &n, cap, 0x08);  continue;
      case 'f':  put_cp(buf, &n, cap, 0x0C);  continue;
      case 'n':  put_cp(buf, &n, cap, 0x0A);  continue;
      case 'r':  put_cp(buf, &n, cap, 0x0D);  continue;
      case 't':  put_cp(buf, &n, cap, 0x09);  continue;
      case 'u': {
        unsigned hi = 0;
        if (!read_hex4(c, hi)) return false;
        if (hi >= 0xD800 && hi <= 0xDBFF) {
          /* high surrogate: a low one must immediately follow */
          if (c.end - c.p < 6 || c.p[0] != '\\' || c.p[1] != 'u') return false;
          Cursor lo = c;
          lo.p += 2;
          unsigned lval = 0;
          if (!read_hex4(lo, lval) || lval < 0xDC00 || lval > 0xDFFF) return false;
          unsigned cp = 0x10000u + ((hi - 0xD800) << 10) + (lval - 0xDC00);
          put_cp(buf, &n, cap, (unsigned)cp);
          c.p = lo.p;
        } else if (hi >= 0xDC00 && hi <= 0xDFFF) {
          return false; /* lone low surrogate */
        } else {
          put_cp(buf, &n, cap, hi);
        }
        continue;
      }
      default: return false;
    }
  }
  if (buf) buf[0] = 0;
  return false; /* no closing quote */
}

/* ---------- scalars ---------- */

inline bool parse_number(Cursor &c, double &out) {
  if (!skip_ws(c)) return false;
  const char *s = c.p;
  if (!(*c.p == '-' || *c.p == '+' || ((unsigned char)*c.p >= '0' && (unsigned char)*c.p <= '9')))
    return false;
  while (c.p < c.end &&
         (((unsigned char)*c.p >= '0' && (unsigned char)*c.p <= '9') ||
          *c.p == '.' || *c.p == 'e' || *c.p == 'E' || *c.p == '+' || *c.p == '-'))
    ++c.p;
  size_t len = (size_t)(c.p - s);
  if (len == 0 || len > 200) return false;
  /* digit check: first char was '-', '+', or digit (above); make sure at
     least one digit appears anywhere in the token */
  bool any_digit = false;
  for (size_t i = 0; i < len; i++)
    if ((unsigned char)s[i] >= '0' && (unsigned char)s[i] <= '9') any_digit = true;
  if (!any_digit) return false;
  char tmp[208];
  memcpy(tmp, s, len);
  tmp[len] = 0;
  out = strtod(tmp, 0); /* strtod handles sign, '.', 'e' correctly */
  return true;
}

inline bool parse_bool(Cursor &c, bool &out) {
  if (c.end - c.p >= 4 && memcmp(c.p, "true", 4) == 0)  { c.p += 4; out = true;  return true; }
  if (c.end - c.p >= 5 && memcmp(c.p, "false", 5) == 0) { c.p += 5; out = false; return true; }
  return false;
}

/* Skip any complete JSON value without caring about its meaning. */
inline bool skip_value(Cursor &c) {
  if (!skip_ws(c)) return false;
  unsigned char ch = (unsigned char)*c.p;
  if (ch == '{' || ch == '[') {
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    while (c.p < c.end) {
      char q = *c.p++;
      if (in_str) {
        if (esc) esc = false;
        else if (q == '\\') esc = true;
        else if (q == '"') in_str = false;
        continue;
      }
      if (q == '"') in_str = true;
      else if (q == '{' || q == '[') depth++;
      else if (q == '}' || q == ']') {
        depth--;
        if (depth == 0) return true;
      }
    }
    return false;
  }
  if (ch == '"') {
    char tmp[16];
    return parse_string(c, tmp, sizeof tmp);
  }
  if (ch == 't' || ch == 'f' || ch == 'n') {
    const char *word = (ch == 't') ? "true" : (ch == 'f' ? "false" : "null");
    size_t wl = (ch == 't') ? 4 : (ch == 'f' ? 5 : 4);
    if (c.end - c.p >= (ptrdiff_t)wl && memcmp(c.p, word, wl) == 0) { c.p += wl; return true; }
    return false;
  }
  if (ch == '-' || (ch >= '0' && ch <= '9')) {
    double v;
    return parse_number(c, v);
  }
  return false;
}

/* ---------- top-level object walk ---------- */

inline bool parse_object(Cursor &c, struct NavState *out) {
  if (!skip_ws(c) || *c.p != '{') return false;
  ++c.p;
  if (!skip_ws(c)) return false;
  if (*c.p == '}') { ++c.p; return false; } /* {} has no "t" → invalid */

  int t_state = 0; /* 0 = not seen, 1 = seen (validated below) */

  for (;;) {
    if (!skip_ws(c)) return false;
    if (*c.p != '"') return false; /* key must be a string */
    char key[48];
    if (!parse_string(c, key, sizeof key)) return false;

    if (!skip_ws(c) || c.p >= c.end || *c.p != ':') return false;
    c.p++; /* ':' */
    if (!skip_ws(c)) return false; /* space after colon is legal JSON — skip it before the value */

    if (strcmp(key, "t") == 0) {
      char v[16];
      if (!parse_string(c, v, sizeof v)) return false;
      if      (!strcmp(v, "nav"))     out->msg_type = NAV_MSG_NAV;
      else if (!strcmp(v, "beep"))    out->msg_type = NAV_MSG_BEEP;
      else if (!strcmp(v, "idle"))    out->msg_type = NAV_MSG_IDLE;
      else if (!strcmp(v, "reroute")) out->msg_type = NAV_MSG_REROUTE;
      else if (!strcmp(v, "arrived")) out->msg_type = NAV_MSG_ARRIVED;
      else return false; /* unknown type → whole message invalid */
      t_state = 1;
    } else if (strcmp(key, "maneuver") == 0) {
      char v[16];
      if (!parse_string(c, v, sizeof v)) return false;
      memcpy(out->maneuver, v, sizeof(v) < sizeof(out->maneuver) ? sizeof(v) : sizeof(out->maneuver) - 1);
    } else if (strcmp(key, "text") == 0) {
      char v[64];
      if (!parse_string(c, v, sizeof v)) return false;
      memcpy(out->text, v, sizeof(v) < sizeof(out->text) ? sizeof(v) : sizeof(out->text) - 1);
    } else if (strcmp(key, "dist_m") == 0) {
      double v; if (!parse_number(c, v)) return false;
      if (v < 0 || v > 1000000) return false;
      out->dist_m = (int)v;
    } else if (strcmp(key, "limit_kmh") == 0) {
      double v; if (!parse_number(c, v)) return false;
      if (v < 0 || v > 300) return false;
      out->limit_kmh = (int)v;
    } else if (strcmp(key, "batt_pct") == 0) {
      double v; if (!parse_number(c, v)) return false;
      if (v < 0 || v > 100) return false;
      out->batt_pct = (int)v;
    } else if (strcmp(key, "map_on") == 0) {
      bool v; if (!parse_bool(c, v)) return false;
      out->map_on = v;
    } else if (strcmp(key, "ts") == 0) {
      double v; if (!parse_number(c, v)) return false;
      if (v < 0) return false;
      out->ts = (uint64_t)v; /* up to 2^53 — plenty for epoch ms */
    } else if (strcmp(key, "v") == 0) {
      double v; if (!parse_number(c, v)) return false;
      if (v != 1) return false; /* protocol version mismatch → reject */
    } else {
      if (!skip_value(c)) return false; /* unknown key: skip value safely */
    }

    if (!skip_ws(c)) return false;
    if (*c.p == '}') { c.p++; break; }
    if (*c.p != ',') return false;
    c.p++;
    if (!skip_ws(c)) return false;
    if (*c.p == '}') { c.p++; break; } /* tolerate empty last slot: {a:1,} → stop */
  }

  if (!t_state) return false; /* "t" is mandatory */
  return true;
}

} /* namespace navmsg */

namespace navmsg {
/* Validate a full message and copy into `out`. Returns true on success. */
inline bool validate(const char *json, size_t len, struct NavState *out) {
  struct NavState fresh;
  memset(&fresh, 0, sizeof fresh);
  fresh.batt_pct = -1;
  fresh.msg_type = NAV_MSG_NONE;

  Cursor c;
  c.p = json;
  c.end = json + len;

  if (!parse_object(c, &fresh)) return false;
  /* only whitespace may follow the closing brace */
  while (c.p < c.end &&
         (*c.p == ' ' || *c.p == '\t' || *c.p == '\r' || *c.p == '\n'))
    ++c.p;
  if (c.p != c.end) return false; /* trailing junk */

  *out = fresh;
  out->has_data = true;
  return true;
}
} /* namespace navmsg */

/* Public entry point — extern "C" to match the declaration in nav_message.h */
extern "C" bool parse_nav_message(const char *json, size_t len, struct NavState *out) {
  if (!json || !out) return false;
  if (len == 0) len = strlen(json);
  if (len == 0) return false;
  return navmsg::validate(json, len, out);
}

#endif /* LIBREMOTO_NAV_MESSAGE_PARSE_IMPL_H */
