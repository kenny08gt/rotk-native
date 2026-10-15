// Create functions/names from a TSV (addr, size, name) and tag functions by referenced source-file strings.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import java.io.*;
import java.util.*;

public class ApplySymbols extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        int created = 0;
        try (BufferedReader r = new BufferedReader(new FileReader(args[0]))) {
            String line;
            while ((line = r.readLine()) != null) {
                String[] f = line.split("\t");
                Address a = sp.getAddress(Long.parseLong(f[0], 16));
                disassemble(a);
                Function fn = getFunctionAt(a);
                if (fn == null) fn = createFunction(a, f[2]);
                if (fn != null) { fn.setName(f[2], SourceType.IMPORTED); created++; }
            }
        }
        println("functions named: " + created);
        // Tag functions referencing "Something.c" strings.
        Listing listing = currentProgram.getListing();
        PrintWriter out = new PrintWriter(new FileWriter(args[1]));
        DataIterator it = listing.getDefinedData(true);
        while (it.hasNext()) {
            Data d = it.next();
            if (!(d.getValue() instanceof String)) continue;
            String s = (String) d.getValue();
            if (!s.matches(".*[A-Za-z0-9_]+\\.(c|cpp|h)$")) continue;
            String file = s.replaceAll(".*[\\\\/]", "");
            for (Reference ref : getReferencesTo(d.getAddress())) {
                Function fn = getFunctionContaining(ref.getFromAddress());
                if (fn == null) continue;
                out.println(file + "\t" + fn.getEntryPoint() + "\t" + fn.getName());
                if (fn.getName().startsWith("fn_")) {
                    String mod = file.replaceAll("\\.(c|cpp|h)$", "");
                    fn.setName(mod + "__" + fn.getName(), SourceType.ANALYSIS);
                }
            }
        }
        out.close();
    }
}
