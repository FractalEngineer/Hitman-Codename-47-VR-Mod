// Dumps a vtable-shaped table of x86 function pointers.
//
// Run with: analyzeHeadless <proj> <name> -process <program> -noanalysis
//           -scriptPath tools/ghidra -postScript DumpVtable.java <outDir> <addr> [byteCount]
//
// Addresses are absolute. The script reads dwords, stops at the requested byte
// count, and identifies every slot whose value resolves to a function.
//
// @category h47vr

import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;

public class DumpVtable extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DumpVtable <outDir> <addr> [byteCount]");
            return;
        }

        String outDir = args[0];
        Address start = toAddr(Long.parseLong(args[1], 16));
        int byteCount = args.length >= 3
            ? Integer.parseInt(args[2].replaceFirst("^0[xX]", ""), 16)
            : 0x400;
        Memory memory = currentProgram.getMemory();

        File out = new File(outDir);
        out.mkdirs();
        String safe = start.toString().replaceAll("[^A-Za-z0-9._-]", "_");
        File report = new File(out, currentProgram.getName() + ".vtable." + safe + ".txt");
        try (PrintWriter writer = new PrintWriter(report, "UTF-8")) {
            writer.println("vtable " + start + " (" + byteCount + " bytes)");
            for (int offset = 0; offset + 4 <= byteCount; offset += 4) {
                Address slot = start.add(offset);
                int value;
                try {
                    value = memory.getInt(slot);
                } catch (Exception e) {
                    writer.printf("+0x%03X  unreadable%n", offset);
                    continue;
                }
                if (value == 0) {
                    writer.printf("+0x%03X  00000000%n", offset);
                    continue;
                }

                Address target = toAddr(Integer.toUnsignedLong(value));
                Function function = getFunctionContaining(target);
                if (function != null) {
                    writer.printf("+0x%03X  %08X  %s @ %s%n",
                        offset, value, function.getName(),
                        function.getEntryPoint());
                } else {
                    writer.printf("+0x%03X  %08X  <not a function>%n",
                        offset, value);
                }
            }
        }
        println("wrote " + report.getAbsolutePath());
    }
}
