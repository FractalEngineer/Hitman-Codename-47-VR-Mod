// Finds every instruction that mentions a particular scalar value as a
// displacement, then decompiles the containing function.
//
// This is how a struct field found by offset gets its readers found: `DumpRefsTo`
// works on addresses, and imports work by name, but `interface+0xa95` is a
// displacement, not a symbol. Searching for the constant finds every place that
// touches it, whether or not it is the one already known.
//
// Run with: analyzeHeadless <proj> <name> -process <program> -noanalysis
//           -scriptPath tools/ghidra -postScript FindScalarRefs.java <outDir> <hex>...
//
// Values are hex without the 0x prefix, for example `a95`.
//
// @category h47vr

import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;

public class FindScalarRefs extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: FindScalarRefs <outDir> <hex>...");
            return;
        }
        String outDir = args[0];

        Set<Long> wanted = new LinkedHashSet<>();
        for (int n = 1; n < args.length; ++n) {
            wanted.add(Long.parseLong(args[n], 16));
        }

        Map<Address, String> functions = new LinkedHashMap<>();
        Map<Address, String> why = new LinkedHashMap<>();

        InstructionIterator instructions = currentProgram.getListing().getInstructions(true);
        while (instructions.hasNext()) {
            Instruction insn = instructions.next();
            for (int op = 0; op < insn.getNumOperands(); ++op) {
                for (Object object : insn.getOpObjects(op)) {
                    if (!(object instanceof Scalar)) {
                        continue;
                    }
                    long value = ((Scalar) object).getUnsignedValue();
                    if (!wanted.contains(value)) {
                        continue;
                    }
                    Function function = getFunctionContaining(insn.getAddress());
                    String where = (function == null) ? "(no function)" : function.getName();
                    println("0x" + Long.toHexString(value) + ": " + insn.getAddress()
                        + "  " + insn + "  in " + where);
                    if (function != null) {
                        Address entry = function.getEntryPoint();
                        functions.putIfAbsent(entry, function.getName());
                        why.putIfAbsent(entry, "scalar 0x" + Long.toHexString(value)
                            + " from " + insn.getAddress());
                    }
                }
            }
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        for (Map.Entry<Address, String> entry : functions.entrySet()) {
            Function function = getFunctionAt(entry.getKey());
            if (function == null) {
                continue;
            }
            String safe = function.getName().replaceAll("[^A-Za-z0-9._-]", "_");
            File out = new File(outDir, currentProgram.getName() + ".scalar." + safe + ".c");
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
