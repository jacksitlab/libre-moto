// Cross-check (Kotlin app <-> C++ firmware) for the LibreMoto MapData wire
// format. Confirms the Kotlin encoder (app/src/main/java/.../protocol/MapFrame.kt)
// produces bytes that the firmware parser (map_frame_parse_impl.h) decodes back
// to identical absolute points. Catches any drift between the two sides.
//
// The mt0[45] byte stream below was emitted VERBATIM by the Kotlin encoder via
// firmware/test/emit_mt0.java (javac + java against the compiled Kotlin classes),
// not hand-transcribed. Regenerate:
//   KSTD=<kotlin-stdlib.jar> KCL=app/build/tmp/kotlin-classes/debug
//   javac -cp "$KSTD:$KCL" emit_mt0.java -d /tmp/emit && java -cp "/tmp/emit:$KSTD:$KCL" emit_mt0
#include "map_frame.h"
#include <cstdio>
#include <cstdint>

static const uint8_t mt0[45] = {
    0x4c, 0x4d, 0x01, 0x0a,           // magic "LM" LE, ver=1, seq=10
    0x05, 0x00,                       // flags ROT|VEHICLE
    0x00, 0x00,                       // heading 0
    0x14, 0x00,                       // scale 20 (0.20 px/m x100)
    0x02,                             // seg_count 2
    0x02, 0x0c, 0x02,                 // road, w12, n2
    0x38, 0xff, 0xd8, 0xff,           // pt0 (-200,-40) absolute
    0x90, 0x01, 0x00, 0x00,           // pt1 delta (+400, 0) -> (200,-40)
    0x01, 0x0a, 0x05,                 // route, w10, n5
    0x00, 0x00, 0x00, 0x00,           // pt0 (0,0) absolute
    0x00, 0x00, 0xba, 0xff,           // pt1 delta (0,-70) -> (0,-70)
    0x00, 0x00, 0xba, 0xff,           // pt2 delta (0,-70) -> (0,-140)
    0x00, 0x00, 0xba, 0xff,           // pt3 delta (0,-70) -> (0,-210)
    0x00, 0x00, 0xba, 0xff,           // pt4 delta (0,-70) -> (0,-280)
};

int main() {
    MapFrame f;
    if (!parse_map_frame(mt0, sizeof(mt0), &f)) {
        printf("CROSS-FAIL: firmware parser rejected Kotlin bytes\n"); return 1;
    }
    int fail = 0;
    if (f.seq != 0x0A)                                        { printf("seq 0x%02X != 0x0A\n", f.seq); fail = 1; }
    if (f.flags != (MAP_FLAG_ROTATE | MAP_FLAG_VEHICLE))      { printf("flags 0x%04X\n", f.flags); fail = 1; }
    if (f.heading != 0)                                       { printf("heading %d\n", f.heading); fail = 1; }
    if (f.scale != 0x0014)                                    { printf("scale 0x%04X\n", f.scale); fail = 1; }
    if (f.seg_count != 2)                                     { printf("segs %u\n", f.seg_count); fail = 1; }
    if (f.segs[0].type != MAP_SEG_ROAD || f.segs[0].width != 12 || f.segs[0].npts != 2) {
        printf("seg0 t=%u w=%u n=%u\n", f.segs[0].type, f.segs[0].width, f.segs[0].npts); fail = 1;
    }
    if (f.segs[1].type != MAP_SEG_ROUTE || f.segs[1].width != 10 || f.segs[1].npts != 5) {
        printf("seg1 t=%u w=%u n=%u\n", f.segs[1].type, f.segs[1].width, f.segs[1].npts); fail = 1;
    }
    if (f.segs[0].points[0] != -200 || f.segs[0].points[1] != -40 ||
        f.segs[0].points[2] !=  200 || f.segs[0].points[3] != -40) {
        printf("road pts: %d %d / %d %d  (want -200 -40 / 200 -40)\n",
               f.segs[0].points[0], f.segs[0].points[1], f.segs[0].points[2], f.segs[0].points[3]); fail = 1;
    }
    for (int i = 0; i < 5; i++)
        if (f.segs[1].points[i*2+0] != 0 || f.segs[1].points[i*2+1] != -70*i) {
            printf("route pt%d: %d %d  (want 0 %d)\n", i,
                   f.segs[1].points[i*2+0], f.segs[1].points[i*2+1], -70*i); fail = 1;
            break;
        }
    printf(fail ? "CROSS-FAIL\n" : "CROSS-PASS (Kotlin bytes decode identically in C++)\n");
    return fail;
}
