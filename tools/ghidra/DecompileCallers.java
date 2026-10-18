// Decompiles the function at one or more addresses, then walks the call graph
// upwards so we can see who calls it and with what arguments.
//
// Used to read a console command's parser directly instead of guessing syntax:
// the dispatcher itself only sees a command name plus a second argument, so the
// *caller* is what splits a typed line into those two pieces.
//
// Run with: analyzeHeadless <proj> <name> -process <program> -noanalysis
//           -scriptPath tools/ghidra -postScript DecompileCallers.java <outDir> <depth> <addr>...
//
// Addresses are absolute (image base + RVA), e.g. 0ffae590 for System.dll RVA 0xE590.
//
// @category h47vr

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayDeque;
import java.util.LinkedHashMap;
import java.util.Map;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileCallers extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 3) {
            println("usage: DecompileCallers <outDir> <depth> <addr>...");
            return;
        }
        String outDir = args[0];
        int depth = Integer.parseInt(args[1]);

        // Walk callers breadth-first, keeping insertion order so the nearest
        // callers are written first.
        Map<Address, Function> found = new LinkedHashMap<>();
        Map<Address, Integer> level = new LinkedHashMap<>();

        ArrayDeque<Function> queue = new ArrayDeque<>();
        for (int n = 2; n < args.length; ++n) {
            Address address = toAddr(Long.parseLong(args[n], 16));
            Function function = getFunctionAt(address);
            if (function == null) {
                function = getFunctionContaining(address);
            }
            if (function == null) {
                println("no function at " + address);
                continue;
            }
            if (!found.containsKey(function.getEntryPoint())) {
                found.put(function.getEntryPoint(), function);
                level.put(function.getEntryPoint(), 0);
                queue.add(function);
            }
        }

        for (int d = 0; d < depth && !queue.isEmpty(); ++d) {
            ArrayDeque<Function> next = new ArrayDeque<>();
            while (!queue.isEmpty()) {
                Function function = queue.poll();
                for (Function caller : function.getCallingFunctions(monitor)) {
                    Address entry = caller.getEntryPoint();
                    if (found.containsKey(entry)) {
                        continue;
                    }
                    found.put(entry, caller);
                    level.put(entry, d + 1);
                    println("caller(d" + (d + 1) + ") " + caller.getName() + " @ " + entry
                        + "  -> " + function.getName());
                    next.add(caller);
                }
            }
            queue = next;
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        for (Map.Entry<Address, Function> entry : found.entrySet()) {
            Function function = entry.getValue();
            String safe = function.getName().replaceAll("[^A-Za-z0-9._-]", "_");
            File out = new File(outDir, currentProgram.getName() + ".d" + level.get(entry.getKey())
                + "." + safe + "." + entry.getKey() + ".c");

            DecompileResults results = decompiler.decompileFunction(function, 180, monitor);
            try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
                w.println("// " + currentProgram.getName() + " : " + function.getName()
                    + " @ " + entry.getKey() + "  (caller depth " + level.get(entry.getKey()) + ")");
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
