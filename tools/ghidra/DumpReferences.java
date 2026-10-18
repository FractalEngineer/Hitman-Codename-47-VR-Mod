// Dumps every call site of the imported GL entry points of interest, grouped by
// the function that contains them.
//
// This is how the renderer's projection, matrix and draw code gets located:
// rather than guessing from function sizes, follow the imports the mod has to
// intervene on and see which internal functions call them.
//
// Run with: analyzeHeadless <proj> <name> -process <program> -noanalysis
//           -scriptPath tools/ghidra -postScript DumpReferences.java <outDir>
//
// @category h47vr

import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class DumpReferences extends GhidraScript {

    private static final String[] TARGETS = {
        // projection and view setup
        "glFrustum", "glOrtho", "glViewport",
        // matrix stack
        "glMatrixMode", "glLoadIdentity", "glPushMatrix", "glPopMatrix",
        "glMultMatrixf", "glTranslatef", "glScalef",
        // geometry submission
        "glBegin", "glEnd", "glDrawElements", "glVertexPointer", "glVertex3fv",
        // textures
        "glTexImage2D", "glTexSubImage2D", "glBindTexture", "glGenTextures",
        // state that matters for stereo
        "glClear", "glDisable", "glEnable", "glCullFace", "glScissor",
        // presentation
        "SwapBuffers"
    };

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outDir = args.length > 0 ? args[0] : ".";
        String program = currentProgram.getName();
        File out = new File(outDir, program.replaceAll("[^A-Za-z0-9._-]", "_") + ".references.txt");

        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            w.println("# " + program + "  image base " + currentProgram.getImageBase());
            long base = currentProgram.getImageBase().getOffset();

            for (String target : TARGETS) {
                SymbolIterator symbols = currentProgram.getSymbolTable().getSymbols(target);
                boolean wroteHeader = false;
                while (symbols.hasNext()) {
                    Symbol symbol = symbols.next();
                    Address address = symbol.getAddress();
                    if (address == null) {
                        continue;
                    }

                    // Group call sites by the function that contains them.
                    Map<String, Integer> byFunction = new TreeMap<>();
                    Map<String, Address> entryPoints = new LinkedHashMap<>();
                    for (Reference reference : getReferencesTo(address)) {
                        Function function = getFunctionContaining(reference.getFromAddress());
                        String key = function == null
                            ? "(outside any function)"
                            : function.getName();
                        byFunction.merge(key, 1, Integer::sum);
                        if (function != null) {
                            entryPoints.putIfAbsent(key, function.getEntryPoint());
                        }
                    }
                    if (byFunction.isEmpty()) {
                        continue;
                    }

                    if (!wroteHeader) {
                        w.println();
                        w.println("== " + target);
                        wroteHeader = true;
                    }
                    for (Map.Entry<String, Integer> entry : byFunction.entrySet()) {
                        Address entryPoint = entryPoints.get(entry.getKey());
                        String where = entryPoint == null
                            ? ""
                            : String.format(" rva 0x%06X", entryPoint.getOffset() - base);
                        w.println(String.format("   %-34s x%-3d%s",
                            entry.getKey(), entry.getValue(), where));
                    }
                }
            }
        }

        println(program + ": references dumped -> " + out.getAbsolutePath());
    }
}
