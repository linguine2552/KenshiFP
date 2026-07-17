// KenshiFP: decompile an arbitrary set of functions by RVA. Read-only.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DumpFns extends GhidraScript {
    long base = 0x140000000L;
    long[] rvas = { 0x788a00L /* GameWorld::mainLoop_GPUSensitiveStuff */ };

    @Override
    public void run() throws Exception {
        String arg = null;
        String[] a = getScriptArgs();
        if (a != null && a.length > 0) arg = a[0];
        long[] targets = rvas;
        if (arg != null) {
            String[] parts = arg.split(",");
            targets = new long[parts.length];
            for (int i = 0; i < parts.length; i++) targets[i] = Long.parseLong(parts[i].replace("0x","").trim(), 16);
        }
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        for (long rva : targets) {
            Address entry = toAddr(base + rva);
            Function f = getFunctionAt(entry);
            if (f == null) f = getFunctionContaining(entry);
            println("\n==== 0x" + Long.toHexString(rva) + " -> " + (f==null?"(no func)":f.getName()+" entryRVA=0x"+Long.toHexString(f.getEntryPoint().getOffset()-base)) + " ====");
            if (f == null) continue;
            DecompileResults r = dec.decompileFunction(f, 120, monitor);
            if (r != null && r.decompileCompleted()) {
                for (String ln : r.getDecompiledFunction().getC().split("\n")) println(ln);
            } else {
                println("(decompile failed)");
            }
        }
    }
}
