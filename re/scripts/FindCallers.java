// KenshiFP: find + decompile the CALLERS of given function RVAs. Used to locate
// where the CameraClass instance is stored (caller of the ctor 0x6afbf0) and
// similar "who constructs / who invokes" questions. Read-only.
// Arg: comma-separated callee RVAs, e.g. "0x6afbf0".
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import java.util.LinkedHashSet;
import java.util.Set;

public class FindCallers extends GhidraScript {
    long base = 0x140000000L;

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long[] callees = (a != null && a.length > 0) ? parse(a[0]) : new long[]{ 0x6afbf0L };
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Set<Function> callers = new LinkedHashSet<>();
        for (long rva : callees) {
            Address entry = toAddr(base + rva);
            Function callee = getFunctionAt(entry);
            println("callee 0x" + Long.toHexString(rva) + " = " + (callee==null?"null":callee.getName()));
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(entry);
            while (it.hasNext()) {
                Reference r = it.next();
                Function f = getFunctionContaining(r.getFromAddress());
                String fn = f==null?"(no func)":f.getName()+" @0x"+Long.toHexString(f.getEntryPoint().getOffset()-base);
                println("  " + r.getReferenceType() + " from " + r.getFromAddress() + " in " + fn);
                if (f != null) callers.add(f);
            }
        }
        for (Function f : callers) {
            println("\n==== CALLER " + f.getName() + " entryRVA=0x" + Long.toHexString(f.getEntryPoint().getOffset()-base) + " ====");
            DecompileResults r = dec.decompileFunction(f, 90, monitor);
            if (r != null && r.decompileCompleted())
                for (String ln : r.getDecompiledFunction().getC().split("\n")) println(ln);
        }
    }
    long[] parse(String s){ String[] p=s.split(","); long[] o=new long[p.length]; for(int i=0;i<p.length;i++) o[i]=Long.parseLong(p[i].replace("0x","").trim(),16); return o; }
}
