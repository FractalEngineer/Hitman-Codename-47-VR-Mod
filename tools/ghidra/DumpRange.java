// Dumps instructions for an address range, with symbols and resolved
// references, so a decompiler that mis-reads a stack argument can be checked
// against the actual machine code.
//
// Run with: analyzeHeadless <proj> <name> -process <program> -noanalysis
//           -scriptPath tools/ghidra -postScript DumpRange.java <outDir> <start> [end|+len]
//
// Addresses are absolute (image base + RVA).
//
// @category h47vr

import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;

public class DumpRange extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DumpRange <outDir> <start> [end|+len]");
            return;
        }
        String outDir = args[0];
        Address start = toAddr(args[1]);

        Address stop;
        if (args.length >= 3 && args[2].startsWith("+")) {
            String length = args[2].substring(1);
            if (length.startsWith("0x")) {
                length = length.substring(2);
            }
            stop = start.add(Long.parseLong(length, 16));
        } else if (args.length >= 3) {
            stop = toAddr(args[2]);
        } else {
            stop = start.add(0x400);
        }

        Listing listing = currentProgram.getListing();
        Instruction insn = listing.getInstructionAt(start);
        if (insn == null) {
            // Auto-analysis may not have defined this region; disassemble on
            // demand, the same way DecompileAt materialises functions.
            disassemble(start);
            insn = listing.getInstructionAt(start);
        }
        if (insn == null) {
            println("no instruction at " + start);
            return;
        }

        File out = new File(outDir);
        out.mkdirs();
        String name = currentProgram.getName() + ".range." + start + ".txt";
        PrintWriter writer = new PrintWriter(new File(out, name));

        String currentFunction = "<none>";
        int count = 0;
        while (insn != null && insn.getAddress().compareTo(stop) < 0 && count < 40000) {
            Function fn = getFunctionContaining(insn.getAddress());
            String fnName = fn == null ? "<none>" : fn.getName();
            if (!fnName.equals(currentFunction)) {
                currentFunction = fnName;
                writer.println();
                writer.println("--- " + fnName + " ---");
            }
            StringBuilder line = new StringBuilder();
            line.append(insn.getAddress()).append("  ").append(insn);
            for (Reference ref : insn.getReferencesFrom()) {
                line.append("   ; -> ").append(ref.getToAddress());
                try {
                    Function target = getFunctionAt(ref.getToAddress());
                    if (target != null) {
                        line.append(" ").append(target.getName());
                    }
                } catch (Exception e) {
                    // a reference into data has no function, which is fine
                }
            }
            writer.println(line);
            ++count;
            insn = insn.getNext();
        }

        writer.close();
        println("wrote " + new File(out, name).getAbsolutePath() + " (" + count + " instructions)");
    }
}
