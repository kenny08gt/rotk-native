// Export call edges (caller_addr, caller_name, callsite, callee_addr, callee_name) as TSV.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class ExportCalls extends GhidraScript {
    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(getScriptArgs()[0]));
        int n = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            for (var a : f.getBody().getAddresses(true)) {
                for (Reference r : getReferencesFrom(a)) {
                    if (!r.getReferenceType().isCall()) continue;
                    Function callee = getFunctionAt(r.getToAddress());
                    if (callee == null) continue;
                    out.println(f.getEntryPoint() + "\t" + f.getName() + "\t" + a + "\t" + callee.getEntryPoint() + "\t" + callee.getName());
                    n++;
                }
            }
        }
        out.close();
        println("call edges: " + n);
    }
}
