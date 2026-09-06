/* Java harness: invoke the compiled Kotlin MapFrame encoder with the exact mt0
 * parameters (identical to app/src/test/.../MapFrameTest.mt0_matches_firmware_reference)
 * and emit a C byte array. No manual transcription. */
import dev.jacklibre.libremoto.protocol.*;
import java.util.*;

public class emit_mt0 {
    public static void main(String[] a) {
        MapFrame f = new MapFrame(10, 5, 0, 20, Arrays.asList(
            new Segment(SegmentType.Road, 12,
                Arrays.asList(new Point(-200, -40), new Point(200, -40))),
            new Segment(SegmentType.Route, 10,
                Arrays.asList(new Point(0, 0), new Point(0, -70), new Point(0, -140), new Point(0, -210), new Point(0, -280)))));
        byte[] b = f.encode();
        StringBuilder sb = new StringBuilder();
        for (byte x : b) sb.append(String.format("0x%02x, ", x & 0xff));
        System.out.println(sb.toString().trim());
        System.out.println("LENGTH " + b.length);
    }
}
