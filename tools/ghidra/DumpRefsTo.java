// Lists every reference to one or more addresses, then decompiles the function
// containing each referring location.
//
// Used when a function has no detected callers: that means its address is taken
// somewhere (vtable slot, dispatch table, function pointer), and we need to see
// where.
//
// Run with: analyzeHeadless <proj> <name> -process <program> -noanalysis
//           -scriptPath tools/ghidra -postScript DumpRefsTo.java <outDir> <addr>...
//
// Addresses are absolute (image base + RVA).
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
import ghidra.program.model.symbol.Reference;

public class DumpRefsTo extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DumpRefsTo <outDir> <addr>...");
            return;
        }
        String outDir = args[0];

        Map<Address, String> toDecompile = new LinkedHashMap<>();
        Map<Address, String> why = new LinkedHashMap<>();

        for (int n = 1; n < args.length; ++n) {
            Address target = toAddr(Long.parseLong(args[n], 16));
            Reference[] refs = getReferencesTo(target);
            println("references to " + target + " : " + refs.length);
            for (Reference reference : refs) {
                Address from = reference.getFromAddress();
                Function function = getFunctionContaining(from);
                String where = (function == null) ? "(no function)" : function.getName();
                println("  " + reference.getReferenceType() + " from " + from + " in " + where);
                if (function != null && !toDecompile.containsKey(function.getEntryPoint())) {
                    toDecompile.put(function.getEntryPoint(), function.getName());
                    why.put(function.getEntryPoint(), "ref to " + target + " from " + from);
                }
            }
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        for (Map.Entry<Address, String> entry : toDecompile.entrySet()) {
            Function function = getFunctionAt(entry.getKey());
            if (function == null) {
                continue;
            }
            String safe = function.getName().replaceAll("[^A-Za-z0-9._-]", "_");
            File out = new File(outDir, currentProgram.getName() + ".ref." + safe + ".c");
            DecompileResults results = decompiler.decompileFunction(function, 180, monitor);
            try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
                w.println("// " + currentProgram.getName() + " : " + function.getName()
                    + " @ " + entry.getKey() + "   (" + why.get(entry.getKey()) + ")");
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
