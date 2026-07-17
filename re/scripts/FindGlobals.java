// KenshiFP M0: locate the engine global singletons `key` (InputHandler*) and
// `ou` (GameWorld*) declared in kenshi/Globals.h. Strategy: the command-config
// method FUN_140362430 is an InputHandler method (takes `this`); its callers
// load `key` from its global slot and pass it. Also decompile mainLoop's
// caller region to spot `ou`. Read-only.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import java.util.LinkedHashSet;
import java.util.Set;

public class FindGlobals extends GhidraScript {
    long base = 0x140000000L;
    // functions whose CALLERS we want to inspect (to see which global feeds `this`)
    long[] calleeRVAs = { 0x362430L /* InputHandler config/bind method */ };

    @Override
    public void run() throws Exception {
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Set<Function> callers = new LinkedHashSet<>();

        for (long rva : calleeRVAs) {
            Address entry = toAddr(base + rva);
            Function callee = getFunctionAt(entry);
            println("callee FUN @0x" + Long.toHexString(rva) + " = " + (callee == null ? "null" : callee.getName()));
            if (callee == null) continue;
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(entry);
            while (it.hasNext()) {
                Reference r = it.next();
                if (!r.getReferenceType().isCall()) continue;
                Function f = getFunctionContaining(r.getFromAddress());
                if (f != null) { callers.add(f); println("  called from " + f.getName() + " @0x" + Long.toHexString(f.getEntryPoint().getOffset()-base) + " (site " + r.getFromAddress() + ")"); }
            }
        }

        for (Function f : callers) {
            println("\n==== CALLER " + f.getName() + " entryRVA=0x" + Long.toHexString(f.getEntryPoint().getOffset()-base) + " ====");
            DecompileResults r = dec.decompileFunction(f, 90, monitor);
            if (r != null && r.decompileCompleted()) {
                String[] lines = r.getDecompiledFunction().getC().split("\n");
                for (int j = 0; j < Math.min(lines.length, 140); j++) println(lines[j]);
            }
        }
    }
}
