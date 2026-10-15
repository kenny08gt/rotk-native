// Decompile every function into one C file per 0x10000 address bucket; log failures.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.util.*;

public class DumpDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        File outDir = new File(getScriptArgs()[0]);
        outDir.mkdirs();
        DecompInterface di = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        di.setOptions(opts);
        di.openProgram(currentProgram);
        Map<String, PrintWriter> files = new TreeMap<>();
        int ok = 0, bad = 0, warn = 0;
        PrintWriter fails = new PrintWriter(new FileWriter(new File(outDir, "_failures.txt")));
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            long a = f.getEntryPoint().getOffset();
            String bucket = String.format("%08X.c", a & ~0xFFFFL);
            PrintWriter w = files.computeIfAbsent(bucket, k -> {
                try { return new PrintWriter(new FileWriter(new File(outDir, k))); }
                catch (IOException e) { throw new RuntimeException(e); }
            });
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            if (r != null && r.decompileCompleted()) {
                String c = r.getDecompiledFunction().getC();
                if (c.contains("WARNING")) warn++;
                w.println("// " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses());
                w.println(c);
                ok++;
            } else {
                bad++;
                fails.println(f.getEntryPoint() + "\t" + f.getName() + "\t" + (r == null ? "null" : r.getErrorMessage()));
            }
        }
        for (PrintWriter w : files.values()) w.close();
        fails.close();
        println("decompiled ok=" + ok + " failed=" + bad + " with_warnings=" + warn);
    }
}
