// Pin r2/r13 (CodeWarrior small-data bases) and GQR0 (always 0 under the Dolphin OS) across all executable memory so the decompiler resolves SDA globals.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.lang.Register;
import ghidra.program.model.mem.MemoryBlock;
import java.math.BigInteger;

public class SetSDA extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        Register r2 = currentProgram.getRegister("r2"), r13 = currentProgram.getRegister("r13");
        var ctx = currentProgram.getProgramContext();
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (!b.isExecute()) continue;
            ctx.setValue(r2, b.getStart(), b.getEnd(), new BigInteger(a[0], 16));
            ctx.setValue(r13, b.getStart(), b.getEnd(), new BigInteger(a[1], 16));
            ctx.setValue(currentProgram.getRegister("GQR0"), b.getStart(), b.getEnd(), BigInteger.ZERO);
            println("set SDA on " + b.getName());
        }
    }
}
