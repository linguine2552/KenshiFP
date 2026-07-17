// KenshiFP: find all functions that reference a given global address (DAT_).
// Used to identify CameraClass::update (reads the camera-holder global
// DAT_142133308) and to locate the InputHandler `key` global by its users.
// Read-only. Arg: comma-separated global VAs, e.g. "0x142133308".
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import java.util.LinkedHashSet;
import java.util.Set;

public class FindRefsToGlobal extends GhidraScript {
    long base = 0x140000000L;

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long[] targets = (a != null && a.length > 0)
            ? parse(a[0]) : new long[]{ 0x142133308L };
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Set<Function> dump = new LinkedHashSet<>();
        for (long va : targets) {
            Address ga = toAddr(va);
            println("global @ " + ga + " (RVA 0x" + Long.toHexString(va - base) + ")");
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(ga);
            int n = 0;
            while (it.hasNext()) {
                Reference r = it.next();
                Function f = getFunctionContaining(r.getFromAddress());
                String fn = f == null ? "(no func)" : f.getName() + " @0x" + Long.toHexString(f.getEntryPoint().getOffset()-base);
                println("  " + r.getReferenceType() + " from " + r.getFromAddress() + " in " + fn);
                if (f != null) dump.add(f);
                n++;
            }
            if (n == 0) println("  (no refs)");
        }
        for (Function f : dump) {
            println("\n==== " + f.getName() + " entryRVA=0x" + Long.toHexString(f.getEntryPoint().getOffset()-base) + " ====");
            DecompileResults r = dec.decompileFunction(f, 90, monitor);
            if (r != null && r.decompileCompleted())
                for (String ln : r.getDecompiledFunction().getC().split("\n")) println(ln);
        }
    }

    long[] parse(String s) {
        String[] p = s.split(",");
        long[] out = new long[p.length];
        for (int i=0;i<p.length;i++) out[i] = Long.parseLong(p[i].replace("0x","").trim(),16);
        return out;
    }
}
