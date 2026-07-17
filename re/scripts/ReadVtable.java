// KenshiFP M3: read a live vtable (RVA) captured in-game, follow each slot's
// thunk to the real function, and decompile a chosen slot range so we can
// identify CharMovement::setDestination(Vec3, UpdatePriority, bool). Read-only.
// Arg: "vtableRVA,firstSlot,lastSlot" e.g. "0x16fcc88,14,19"
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;

public class ReadVtable extends GhidraScript {
    long base = 0x140000000L;

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long vt = 0x16fcc88L; int lo = 14, hi = 19;
        if (a != null && a.length > 0) {
            String[] p = a[0].split(",");
            vt = Long.parseLong(p[0].replace("0x","").trim(),16);
            if (p.length > 1) lo = Integer.parseInt(p[1].trim());
            if (p.length > 2) hi = Integer.parseInt(p[2].trim());
        }
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        for (int i = 0; i <= hi + 2; i++) {
            Address slot = toAddr(base + vt + (long)i * 8);
            long target = getLong(slot) & 0xffffffffffffffffL;
            long trva = target - base;
            Function real = resolveThunk(toAddr(target));
            String nm = real == null ? "?" : ("0x" + Long.toHexString(real.getEntryPoint().getOffset()-base) + " " + real.getName());
            println("vt[" + i + "] off=0x" + Long.toHexString(i*8L) + " thunk RVA 0x" + Long.toHexString(trva) + " -> real " + nm);
        }
        println("\n################ decompiling slots " + lo + ".." + hi + " ################");
        for (int i = lo; i <= hi; i++) {
            Address slot = toAddr(base + vt + (long)i * 8);
            long target = getLong(slot);
            Function real = resolveThunk(toAddr(target));
            if (real == null) { println("vt[" + i + "]: unresolved"); continue; }
            println("\n==== vt[" + i + "] real=0x" + Long.toHexString(real.getEntryPoint().getOffset()-base) + " " + real.getName() + " ====");
            DecompileResults r = dec.decompileFunction(real, 60, monitor);
            if (r != null && r.decompileCompleted()) {
                String[] lines = r.getDecompiledFunction().getC().split("\n");
                for (int j = 0; j < Math.min(lines.length, 55); j++) println(lines[j]);
            }
        }
    }

    // Follow a one-instruction JMP thunk to its target function.
    Function resolveThunk(Address a) {
        Function f = getFunctionAt(a);
        if (f != null && f.isThunk()) return f.getThunkedFunction(true);
        Instruction ins = getInstructionAt(a);
        if (ins != null && ins.getMnemonicString().startsWith("JMP")) {
            Address[] flows = ins.getFlows();
            if (flows != null && flows.length == 1) {
                Function t = getFunctionAt(flows[0]);
                if (t != null) return t;
            }
        }
        return f;
    }
}
