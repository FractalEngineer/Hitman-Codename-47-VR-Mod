// Finds strings by content, then decompiles whatever functions reference them.
//
// Used to read a console command's parser directly instead of guessing syntax:
// locate the command-name string, follow its references to the registration
// code, and dump the decompiled C.
//
// Run with: analyzeHeadless <proj> <name> -process <program> -noanalysis
//           -scriptPath tools/ghidra -postScript DecompileXrefs.java <outDir> <needle>...
//
// @category h47vr

import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.Map;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

public class DecompileXrefs extends GhidraScript {

    private static boolean printable(int b) {
        return b >= 0x20 && b < 0x7F;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DecompileXrefs <outDir> <needle>...");
            return;
        }
        String outDir = args[0];

        // Gather functions to decompile, keyed by entry point, from every needle.
        Map<Address, Function> targets = new LinkedHashMap<>();
        Map<Address, String> why = new LinkedHashMap<>();

        for (int n = 1; n < args.length; ++n) {
            String needle = args[n];
            for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
                if (!block.isInitialized()) {
                    continue;
                }
                long size = block.getSize();
                if (size <= 0 || size > (32L << 20)) {
                    continue;
                }
                byte[] buf = new byte[(int) size];
                block.getBytes(block.getStart(), buf);

                int i = 0;
                while (i < buf.length) {
                    if (!printable(buf[i])) {
                        i++;
                        continue;
                    }
                    int start = i;
                    while (i < buf.length && printable(buf[i])) {
                        i++;
                    }
                    String text = new String(buf, start, i - start, "US-ASCII");
                    if (!text.contains(needle)) {
                        continue;
                    }
                    Address address = block.getStart().add(start);
                    println("  '" + needle + "' at " + address + " : " + text);
                    for (Reference reference : getReferencesTo(address)) {
                        Function function = getFunctionContaining(reference.getFromAddress());
                        if (function != null && !targets.containsKey(function.getEntryPoint())) {
                            targets.put(function.getEntryPoint(), function);
                            why.put(function.getEntryPoint(), needle);
                        }
                    }
                }
            }
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        for (Map.Entry<Address, Function> entry : targets.entrySet()) {
            Function function = entry.getValue();
            String safe = function.getName().replaceAll("[^A-Za-z0-9._-]", "_");
            File out = new File(outDir, currentProgram.getName() + "." + safe + ".c");

            DecompileResults results = decompiler.decompileFunction(function, 120, monitor);
            try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
                w.println("// " + currentProgram.getName() + " : " + function.getName()
                    + " @ " + entry.getKey() + "   (found via '" + why.get(entry.getKey()) + "')");
                w.println();
                if (results != null && results.getDecompiledFunction() != null) {
                    w.println(results.getDecompiledFunction().getC());
                } else {
                    w.println("// decompilation failed");
                }
            }
            println("wrote " + out.getAbsolutePath());
        }

        decompiler.dispose();
    }
}
