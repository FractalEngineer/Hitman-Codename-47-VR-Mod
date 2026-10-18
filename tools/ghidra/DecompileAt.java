// Decompiles the function containing one or more addresses, materialising the
// containing function first when auto-analysis never defined it.
//
// The console dispatcher's only caller lives in code Ghidra left undefined, so
// "get the function containing this address" is not enough: we have to find
// where the preceding function ends, walk forward, and create the function.
//
// Run with: analyzeHeadless <proj> <name> -process <program> -noanalysis
//           -scriptPath tools/ghidra -postScript DecompileAt.java <outDir> <addr>...
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
import ghidra.program.model.listing.Instruction;

public class DecompileAt extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DecompileAt <outDir> <addr>...");
            return;
        }
        String outDir = args[0];

        Map<Address, Function> targets = new LinkedHashMap<>();
        for (int n = 1; n < args.length; ++n) {
            Address address = toAddr(Long.parseLong(args[n], 16));
            Function function = getFunctionContaining(address);
            if (function == null) {
                Address start = findFunctionStart(address);
                if (start == null) {
                    println("could not locate a function start for " + address);
                    continue;
                }
                function = createFunction(start, null);
                if (function == null) {
                    println("createFunction failed at " + start);
                    continue;
                }
                println("materialised function at " + start + " for " + address);
            }
            if (!targets.containsKey(function.getEntryPoint())) {
                targets.put(function.getEntryPoint(), function);
            }
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        for (Map.Entry<Address, Function> entry : targets.entrySet()) {
            Function function = entry.getValue();
            String safe = function.getName().replaceAll("[^A-Za-z0-9._-]", "_");
            File out = new File(outDir, currentProgram.getName() + ".at." + safe + ".c");
            DecompileResults results = decompiler.decompileFunction(function, 180, monitor);
            try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
                w.println("// " + currentProgram.getName() + " : " + function.getName()
                    + " @ " + entry.getKey());
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

    // Walk forward from the end of the preceding function, disassembling as we
    // go, until we reach the address we were asked about.
    private Address findFunctionStart(Address target) throws Exception {
        Function previous = getFunctionBefore(target);
        Address cursor;
        if (previous != null && previous.getBody() != null) {
            cursor = previous.getBody().getMaxAddress().add(1);
        } else {
            cursor = target;
        }

        long guard = 0;
        while (cursor.compareTo(target) < 0) {
            if (++guard > 200000) {
                return null;
            }
            Instruction instruction = getInstructionAt(cursor);
            if (instruction == null) {
                if (getMemoryBlock(cursor) == null) {
                    return null;
                }
                disassemble(cursor);
                instruction = getInstructionAt(cursor);
                if (instruction == null) {
                    cursor = cursor.add(1);
                    continue;
                }
            }
            cursor = instruction.getMaxAddress().add(1);
        }

        if (getInstructionAt(cursor) == null) {
            disassemble(cursor);
        }
        if (getInstructionAt(cursor) == null) {
            return null;
        }
        return cursor;
    }
}
