// KenshiFP M1: enumerate functions whose entry is in [lo,hi) and decompile the
// small ones, to identify CameraClass methods by behavior (followObject writes
// the hand at this+0x28; stopFollowing clears it; setZoomDist writes a float;
// setFreeCameraMode writes this+0xBF). Read-only.
// Args: "loRVA,hiRVA[,maxBodyBytes]" e.g. "0x6adc00,0x6ae400,0x180"
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class ListAndDumpRange extends GhidraScript {
    long base = 0x140000000L;

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long lo = 0x6adc00L, hi = 0x6ae400L, maxBytes = 0x200;
        if (a != null && a.length > 0) {
            String[] p = a[0].split(",");
            lo = parse(p[0]); hi = parse(p[1]);
            if (p.length > 2) maxBytes = parse(p[2]);
        }
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            long rva = f.getEntryPoint().getOffset() - base;
            if (rva < lo || rva >= hi) continue;
            long size = f.getBody().getNumAddresses();
            println("\n==== " + f.getName() + " entryRVA=0x" + Long.toHexString(rva) + " size=0x" + Long.toHexString(size) + " ====");
            if (size > maxBytes) { println("(skipped: larger than 0x" + Long.toHexString(maxBytes) + ")"); continue; }
            DecompileResults r = dec.decompileFunction(f, 60, monitor);
            if (r != null && r.decompileCompleted())
                for (String ln : r.getDecompiledFunction().getC().split("\n")) println(ln);
        }
    }
    long parse(String s){ return Long.parseLong(s.replace("0x","").trim(),16); }
}
