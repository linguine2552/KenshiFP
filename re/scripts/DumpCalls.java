// KenshiFP M2: list every CALL site inside given function RVAs with the target
// RVA and the target's name — to resolve the Ogre::Node/Camera transform
// helpers (setPosition/setOrientation/getPosition/lookAt/setDirection) that
// CameraClass::update and the ctor invoke. Read-only.
// Arg: comma-separated function RVAs, e.g. "0x6b0f90,0x6afbf0,0x6afee0".
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.symbol.Reference;

public class DumpCalls extends GhidraScript {
    long base = 0x140000000L;

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long[] rvas = (a != null && a.length > 0) ? parse(a[0]) : new long[]{ 0x6b0f90L };
        for (long rva : rvas) {
            Function f = getFunctionContaining(toAddr(base + rva));
            println("\n==== calls in " + (f==null?"?":f.getName()) + " @0x" + Long.toHexString(rva) + " ====");
            if (f == null) continue;
            AddressSetView body = f.getBody();
            InstructionIterator it = currentProgram.getListing().getInstructions(body, true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                String m = ins.getMnemonicString();
                if (!m.startsWith("CALL") && !m.startsWith("JMP")) continue;
                Reference[] refs = ins.getReferencesFrom();
                for (Reference r : refs) {
                    Address t = r.getToAddress();
                    if (t == null || !t.isMemoryAddress()) continue;
                    Function tf = getFunctionAt(t);
                    String nm = tf != null ? tf.getName() : getSymbolName(t);
                    long trva = t.getOffset() - base;
                    println("  " + ins.getAddress() + " " + m + " -> 0x" + Long.toHexString(trva) + "  " + nm);
                }
            }
        }
    }
    String getSymbolName(Address a){ ghidra.program.model.symbol.Symbol s = getSymbolAt(a); return s==null?"(unnamed)":s.getName(); }
    long[] parse(String s){ String[] p=s.split(","); long[] o=new long[p.length]; for(int i=0;i<p.length;i++) o[i]=Long.parseLong(p[i].replace("0x","").trim(),16); return o; }
}
