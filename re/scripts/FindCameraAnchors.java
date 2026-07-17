// KenshiFP M0: locate camera command-name strings + camera option strings and
// their xref functions. The camera_forward/back/left/right needles are the
// WASD pan command registrations (InputHandler::addCommand cluster); the
// option strings anchor CameraClass::updateOptionSettings' neighborhood.
// Read-only. Modeled on ../KenshiMP/re/scripts/FindSpeedStrings.java.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import java.util.LinkedHashSet;
import java.util.Set;

public class FindCameraAnchors extends GhidraScript {
    long base = 0x140000000L;
    String[] needles = { "camera_forward", "camera_back", "camera_left", "camera_right",
                         "camera_rotate_left", "camera_rotate_right",
                         "camera_zoom_in", "camera_zoom_out",
                         "camera speed", "Camera Rotate Speed X", "Camera Zoom Speed" };

    @Override
    public void run() throws Exception {
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Set<Function> toDump = new LinkedHashSet<>();

        DataIterator di = currentProgram.getListing().getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            Object v = d.getValue();
            if (!(v instanceof String)) continue;
            String s = (String) v;
            for (String needle : needles) {
                if (!s.equals(needle)) continue;
                Address a = d.getAddress();
                println("string \"" + s + "\" @ " + a + " (RVA 0x" + Long.toHexString(a.getOffset() - base) + ")");
                ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(a);
                int n = 0;
                while (it.hasNext()) {
                    Reference r = it.next();
                    Function f = getFunctionContaining(r.getFromAddress());
                    String fn = f == null ? "(no func)" : f.getName() + " @0x" + Long.toHexString(f.getEntryPoint().getOffset() - base);
                    println("    xref " + r.getFromAddress() + " (" + r.getReferenceType() + ") in " + fn);
                    if (f != null) toDump.add(f);
                    n++;
                }
                if (n == 0) println("    (no xrefs)");
            }
        }

        println("\n################ decompiling referrers ################");
        for (Function f : toDump) {
            println("\n==== " + f.getName() + " entryRVA=0x" + Long.toHexString(f.getEntryPoint().getOffset() - base) + " ====");
            DecompileResults r = dec.decompileFunction(f, 90, monitor);
            if (r != null && r.decompileCompleted()) {
                String[] lines = r.getDecompiledFunction().getC().split("\n");
                for (int j = 0; j < Math.min(lines.length, 120); j++) println(lines[j]);
                if (lines.length > 120) println("... (" + (lines.length - 120) + " more lines truncated)");
            }
        }
    }
}
