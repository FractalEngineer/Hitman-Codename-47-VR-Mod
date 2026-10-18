// Extracts reverse-engineering anchors from an analysed program.
//
// Written for analyzeHeadless: it runs once per imported program and writes one
// report per program into the directory given as the first script argument.
//
// The point is not to decompile anything yet. The binaries leak Z:\ build paths
// and MSVC RTTI names, so string anchors plus their cross-references locate the
// camera, renderer, input and weapon code that the hook map needs.
//
// @category h47vr

import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.Map;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

public class ExtractAnchors extends GhidraScript {

    private static final String[] INTERESTING = {
        "Z:\\",
        "_ZCAMERA_", "ZCAMERA", "MouseConCam", "SniperCamera", "BinocularCamera",
        "FollowCam", "MainCamera",
        "_ZITEM_", "FireShot", "ZItemActivate", "BasePickupTarget", "ZItemAmmo",
        "RunEngine", "EditPrint",
    };

    private static final long MAX_BLOCK_BYTES = 32L << 20;

    private static boolean printable(int b) {
        return b >= 0x20 && b < 0x7F;
    }

    private static boolean interesting(String s) {
        for (String needle : INTERESTING) {
            if (s.contains(needle)) {
                return true;
            }
        }
        return false;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outDir = args.length > 0 ? args[0] : ".";
        String program = currentProgram.getName();
        String safe = program.replaceAll("[^A-Za-z0-9._-]", "_");
        File out = new File(outDir, safe + ".anchors.txt");

        Map<String, Integer> rttiClasses = new LinkedHashMap<>();
        int anchors = 0;
        int refs = 0;

        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            w.println("# " + program);
            w.println("# image base " + currentProgram.getImageBase());
            w.println();

            Memory mem = currentProgram.getMemory();
            for (MemoryBlock block : mem.getBlocks()) {
                if (!block.isInitialized()) {
                    continue;
                }
                long size = block.getSize();
                if (size <= 0 || size > MAX_BLOCK_BYTES) {
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
                    int runLength = i - start;
                    if (runLength < 4) {
                        continue;
                    }

                    String text = new String(buf, start, runLength, "US-ASCII");

                    // MSVC RTTI type descriptors start with .?AV (class) or .?AU (struct).
                    if (text.startsWith(".?AV") || text.startsWith(".?AU")) {
                        rttiClasses.merge(text, 1, Integer::sum);
                    }

                    if (!interesting(text)) {
                        continue;
                    }

                    Address address = block.getStart().add(start);
                    anchors++;
                    w.println("STRING\t" + address + "\t" + text);

                    for (Reference reference : getReferencesTo(address)) {
                        Address from = reference.getFromAddress();
                        Function function = getFunctionContaining(from);
                        refs++;
                        w.println("  REF\t" + from + "\t"
                            + (function == null
                                ? "(not inside a function)"
                                : function.getName() + " @ " + function.getEntryPoint()));
                    }
                }
            }

            w.println();
            w.println("# RTTI type descriptors: " + rttiClasses.size());
            for (String name : rttiClasses.keySet()) {
                w.println("RTTI\t" + name);
            }
        }

        println(program + ": " + anchors + " anchors, " + refs + " references, "
            + rttiClasses.size() + " RTTI descriptors -> " + out.getAbsolutePath());
    }
}
