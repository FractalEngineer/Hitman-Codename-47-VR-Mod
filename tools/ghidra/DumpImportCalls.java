// Dumps every call site of one or more imported functions, located by import
// name rather than by address. DumpReferences.java does this for the GL entry
// points it knows about; this handles anything else, such as the user32 calls
// the renderer uses to manage the cursor.
//
// Run with: analyzeHeadless <proj> <name> -process <program> -noanalysis
//           -scriptPath tools/ghidra -postScript DumpImportCalls.java <outDir> <name>...
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
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class DumpImportCalls extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DumpImportCalls <outDir> <name>...");
            return;
        }
        String outDir = args[0];

        Map<Address, String> toDecompile = new LinkedHashMap<>();
        Map<Address, String> why = new LinkedHashMap<>();

        for (int n = 1; n < args.length; ++n) {
            String name = args[n];
            int resolved = 0;
            SymbolIterator symbols = currentProgram.getSymbolTable().getSymbols(name);
            while (symbols.hasNext()) {
                Symbol symbol = symbols.next();
                resolved++;
                Address target = symbol.getAddress();
                for (Reference reference : getReferencesTo(target)) {
                    Address from = reference.getFromAddress();
                    Function function = getFunctionContaining(from);
                    String where = (function == null) ? "(no function)" : function.getName();
                    println(name + ": " + reference.getReferenceType() + " from " + from
                        + " in " + where);
                    if (function != null && !toDecompile.containsKey(function.getEntryPoint())) {
                        toDecompile.put(function.getEntryPoint(), function.getName());
                        why.put(function.getEntryPoint(), name + " from " + from);
                    }
                }
            }
            if (resolved == 0) {
                println(name + ": no symbol found");
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
            File out = new File(outDir, currentProgram.getName() + ".imp." + safe + ".c");
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
