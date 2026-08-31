/*
 * test_nav_message.cpp — unit tests for nav_message.h (host, C++11)
 *
 * Build & run:
 *   g++ -std=c++11 -Wall -Wextra -O1 ../src/nav/test_nav_message.cpp -o test_nav_message && ./test_nav_message
 *
 * Self-contained (no test framework). Exit code 0 = all pass.
 */
#include "nav_message.h"
#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

static bool T(bool cond, const char *name, int line) {
  if (cond) { ++g_pass; printf("  PASS  %s\n", name); }
  else      { ++g_fail; printf("  FAIL  %s   (line %d)\n", name, line); }
  return cond;
}
#define CHECK(cond, name) T((cond), (name), __LINE__)

static bool P(const char *json, NavState &s) {
  memset(&s, 0, sizeof s);
  return parse_nav_message(json, strlen(json), &s);
}

int main() {
  NavState s;

  /* --- 1: full valid nav message --------------------------------- */
  CHECK(P("{\"v\":1,\"t\":\"nav\",\"maneuver\":\"left\",\"text\":\"Main St.\","
          "\"dist_m\":350,\"limit_kmh\":50,\"batt_pct\":87,\"map_on\":true,"
          "\"ts\":1719792000000}", s), "1: full valid message parses");
  CHECK(s.has_data, "1: has_data set");
  CHECK(s.msg_type == NAV_MSG_NAV, "1: type == nav");
  CHECK(strcmp(s.maneuver, "left") == 0, "1: maneuver == left");
  CHECK(strcmp(s.text, "Main St.") == 0, "1: text == Main St.");
  CHECK(s.dist_m == 350, "1: dist_m == 350");
  CHECK(s.limit_kmh == 50, "1: limit_kmh == 50");
  CHECK(s.batt_pct == 87, "1: batt_pct == 87");
  CHECK(s.map_on == true, "1: map_on == true");
  CHECK(s.ts == 1719792000000ULL, "1: ts preserved (uint64)");

  /* --- 2: defaults for optional fields ----------------------------- */
  CHECK(P("{\"t\":\"beep\"}", s), "2: bare beep parses");
  CHECK(s.msg_type == NAV_MSG_BEEP, "2: type == beep");
  CHECK(s.batt_pct == -1, "2: batt default -1 (absent)");
  CHECK(s.limit_kmh == 0, "2: limit default 0 (absent)");
  CHECK(s.dist_m == 0, "2: dist default 0 (absent)");
  CHECK(s.map_on == false, "2: map_on default false");
  CHECK(s.ts == 0, "2: ts default 0");

  /* --- 3: invalid / rejected --------------------------------------- */
  CHECK(!P("{}", s), "3a: empty object rejected (missing t)");
  CHECK(!P("{\"t\":\"bogus\"}", s), "3b: unknown t rejected");
  CHECK(!P("{\"t\":3}", s), "3c: non-string t rejected");
  CHECK(!P("not json at all", s), "3d: non-JSON rejected");
  CHECK(!P("", s), "3e: empty input rejected");
  CHECK(!P("{\"t\":\"nav\",\"dist_m\":-3}", s), "3f: negative dist_m rejected");
  CHECK(!P("{\"t\":\"nav\",\"batt_pct\":101}", s), "3g: batt_pct > 100 rejected");
  CHECK(!P("{\"v\":2,\"t\":\"nav\"}", s), "3h: protocol version 2 rejected");
  CHECK(!P("{\"t\":\"nav\",\"ts\":-1}", s), "3i: negative ts rejected");
  CHECK(!P("{\"t\":\"nav\" extra", s), "3j: trailing junk rejected");

  /* --- 4: whitespace + key order ------------------------------------ */
  CHECK(P("  { \"t\" : \"nav\" , \"dist_m\" : 120 , \"maneuver\" : \"slight_right\" }  ", s),
        "4: loose whitespace + out-of-order keys parse");
  CHECK(s.dist_m == 120, "4: dist_m == 120");
  CHECK(strcmp(s.maneuver, "slight_right") == 0, "4: maneuver == slight_right");

  /* --- 5: JSON string escapes --------------------------------------- */
  CHECK(P("{\"t\":\"nav\",\"text\":\"A\\\"B\\\\C/n\\tTab\"}", s), "5: escapes parse");
  CHECK(strcmp(s.text, "A\"B\\C/n\tTab") == 0, "5: decoded correctly");
  /* ü / é via \u escapes */
  CHECK(P("{\"t\":\"nav\",\"text\":\"\u00c4\u00f6\u00dc G\u00fcnther\"}", s), "5b: \\u escapes parse");
  CHECK((unsigned char)s.text[0] == 0xC3, "5b: \xC4 decodes to UTF-8 0xC3 0x84");
  CHECK((unsigned char)s.text[1] == 0x84, "5b: ... second byte 0x84");

  /* --- 6: surrogate pair (emoji U+1F600, sent as JSON \ud83d\ude00) ---- */
  CHECK(P("{\"t\":\"nav\",\"text\":\"happy \\ud83d\\ude00 done\"}", s), "6: surrogate pair parses");
  CHECK((unsigned char)s.text[6] == 0xF0 && (unsigned char)s.text[7] == 0x9F,
        "6: emoji decodes to 4-byte UTF-8 lead F0 9F");

  /* --- 7: unknown keys / nesting skipped safely --------------------- */
  CHECK(P("{\"v\":1,\"t\":\"nav\",\"future\":{\"x\":[1,2,{\"y\":null}]},\"arr\":[true,false],"
          "\"skipme\":\"whatever \",\"dist_m\":7}", s),
        "7: unknown keys with nested values skipped");
  CHECK(s.dist_m == 7, "7: known key after unknowns still parsed");

  /* --- 8: maneuver keyword set -------------------------------------- */
  const char *dirs[] = {"left","right","straight","uturn","merge","slight_left","slight_right"};
  for (unsigned i = 0; i < sizeof(dirs)/sizeof(dirs[0]); ++i) {
    char js[128];
    snprintf(js, sizeof js, "{\"t\":\"nav\",\"maneuver\":\"%s\"}", dirs[i]);
    bool ok = P(js, s);
    if (!ok || strncmp(s.maneuver, dirs[i], 16) != 0) {
      T(false, "8: maneuver keyword round-trip", 231);
      break;
    }
  }
  T(true, "8: all 7 maneuver keywords round-trip", __LINE__);

  /* --- 9: explicit map_on=false ------------------------------------ */
  CHECK(P("{\"t\":\"nav\",\"map_on\":false}", s), "9: explicit map_on=false parses");
  CHECK(s.map_on == false, "9: map_on stays false");

  /* --- 10: zero-length numeric edge ---------------------------------- */
  CHECK(P("{\"t\":\"nav\",\"dist_m\":0}", s), "10: dist_m == 0 is legal");
  CHECK(s.dist_m == 0, "10: dist_m 0 accepted");

  /* ================================================================
   * Control message tests (protocol.md §5)
   * ================================================================ */
  CtrlState c;

  /* C1: brightness_pct */
  CHECK(parse_ctrl_message("{\"brightness_pct\":60}", 0, &c)
          && c.brightness_set && c.brightness_pct == 60, "C1: brightness 60 parses + flag");
  CHECK(parse_ctrl_message("{\"brightness_pct\":0}", 0, &c)
          && c.brightness_set && c.brightness_pct == 0, "C1b: 0 is legal");
  CHECK(parse_ctrl_message("{\"brightness_pct\":100}", 0, &c)
          && c.brightness_pct == 100, "C1c: 100 is legal");
  CHECK(!parse_ctrl_message("{\"brightness_pct\":101}", 0, &c), "C1d: 101 rejected");
  CHECK(!parse_ctrl_message("{\"brightness_pct\":-1}", 0, &c), "C1e: -1 rejected");
  CHECK(!parse_ctrl_message("{\"brightness_pct\":\"sixty\"}", 0, &c), "C1f: string rejected");

  /* C2: state */
  CHECK(parse_ctrl_message("{\"state\":\"idle\"}", 0, &c)
          && c.force_idle && !c.brightness_set, "C2: state idle → force_idle");
  CHECK(parse_ctrl_message("{\"state\":\"future\"}", 0, &c)
          && !c.force_idle && !c.brightness_set, "C3: unknown state accepted, no setter");
  CHECK(!parse_ctrl_message("{\"state\":42}", 0, &c), "C3b: number rejected");

  /* C7: reset_map */
  CHECK(parse_ctrl_message("{\"reset_map\":true}", 0, &c)
          && c.reset_map && !c.force_idle, "C7: reset_map true");
  CHECK(parse_ctrl_message("{\"reset_map\":false}", 0, &c)
          && !c.reset_map, "C7b: false → no flag");
  CHECK(!parse_ctrl_message("{\"reset_map\":\"yes\"}", 0, &c), "C7c: string rejected");

  /* C8: combined setters in one write */
  CHECK(parse_ctrl_message("{\"brightness_pct\":25,\"state\":\"idle\",\"reset_map\":true}", 0, &c)
          && c.brightness_set && c.brightness_pct == 25 && c.force_idle && c.reset_map,
        "C8: combo parsed, all setters set");

  /* C9: empty object has nothing to do — reject */
  CHECK(!parse_ctrl_message("{}", 0, &c), "C9: {} rejected");

  /* C10: trailing junk rejected; surrounding whitespace fine */
  CHECK(!parse_ctrl_message("{\"brightness_pct\":40} junk", 0, &c), "C10: trailing junk rejected");
  CHECK(parse_ctrl_message("  { \"brightness_pct\" : 45 }  ", 0, &c)
          && c.brightness_pct == 45, "C10b: whitespace tolerant");

  /* C11: unknown keys with nested values skipped */
  CHECK(parse_ctrl_message("{\"brightness_pct\":80,\"future\":{\"x\":[1,2],\"y\":{\"z\":null}}}", 0, &c)
          && c.brightness_set && c.brightness_pct == 80, "C11: nested unknowns skipped");

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
