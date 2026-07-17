// KenshiFP M2: look up function RVAs by (demangled) name substring. Used to get
// Ogre::Camera transform setters (setPosition/setDirection/setOrientation/
// lookAt/setFixedYawAxis) so we can drive the camera into first-person.
// Read-only. Arg: comma-separated case-insensitive substrings.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class GetSymbols extends GhidraScript {
    long base = 0x140000000L;

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        String[] needles = (a != null && a.length > 0) ? a[0].split(",")
            : new String[]{ "Camera::setPosition", "Camera::setDirection",
                            "Camera::setOrientation", "Camera::lookAt",
                            "Camera::setFixedYawAxis", "Camera::setNearClipDistance",
                            "Node::setPosition", "Node::setOrientation",
                            "Node::_setDerivedPosition" };
        for (int i = 0; i < needles.length; i++) needles[i] = needles[i].toLowerCase().trim();
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        int shown = 0;
        while (it.hasNext()) {
            Function f = it.next();
            String n = f.getName();
            String nl = n.toLowerCase();
            for (String needle : needles) {
                if (nl.contains(needle)) {
                    long rva = f.getEntryPoint().getOffset() - base;
                    println(n + "  RVA=0x" + Long.toHexString(rva) + "  size=0x" + Long.toHexString(f.getBody().getNumAddresses()));
                    shown++;
                    break;
                }
            }
        }
        if (shown == 0) println("(no matches — names may be FUN_ only; use xref-from-ctor instead)");
    }
}
