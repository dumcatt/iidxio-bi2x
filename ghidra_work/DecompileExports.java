// Ghidra Java script to decompile all exported functions
// @category Analysis

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.address.Address;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Set;

public class DecompileExports extends GhidraScript {

    @Override
    protected void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        FunctionManager fm = currentProgram.getFunctionManager();
        SymbolTable st = currentProgram.getSymbolTable();

        StringBuilder sb = new StringBuilder();
        sb.append("================================================================================\n");
        sb.append("DECOMPILATION OF: ").append(currentProgram.getName()).append("\n");
        sb.append("================================================================================\n\n");

        // Collect export addresses
        Set<Address> exportAddrs = new HashSet<>();
        sb.append("--- EXPORTED SYMBOLS ---\n");
        SymbolIterator symIter = st.getAllSymbols(false);
        while (symIter.hasNext()) {
            Symbol sym = symIter.next();
            if (sym.isExternalEntryPoint()) {
                sb.append("  EXPORT: ").append(sym.getName())
                  .append(" @ ").append(sym.getAddress()).append("\n");
                exportAddrs.add(sym.getAddress());
            }
        }
        sb.append("\n");

        // Collect functions to decompile
        ArrayList<Function> exportFuncs = new ArrayList<>();
        ArrayList<Function> internalFuncs = new ArrayList<>();

        FunctionIterator funcIter = fm.getFunctions(true);
        while (funcIter.hasNext()) {
            Function f = funcIter.next();
            if (exportAddrs.contains(f.getEntryPoint())) {
                exportFuncs.add(f);
            } else {
                internalFuncs.add(f);
            }
        }

        // Decompile exports first
        for (Function f : exportFuncs) {
            DecompileResults results = decomp.decompileFunction(f, 30, monitor);
            if (results != null && results.decompileCompleted()) {
                sb.append("--- [EXPORT] ").append(f.getName())
                  .append(" @ ").append(f.getEntryPoint()).append(" ---\n");
                sb.append(results.getDecompiledFunction().getC()).append("\n\n");
            } else {
                sb.append("--- FAILED TO DECOMPILE [EXPORT]: ").append(f.getName()).append(" ---\n\n");
            }
        }

        // Decompile relevant internal functions
        String[] keywords = {"aio", "bio", "bi2x", "bi2a", "tdj", "iob", "sci", "usb", "cdc",
                "nmgr", "node", "firm", "poll", "send", "recv", "led", "lamp", "tape",
                "button", "slider", "turntable", "woofer", "iccr", "status", "create",
                "destroy", "manage", "update", "init", "serial", "com", "device", "open",
                "close", "write", "read", "packet", "acio", "checksum", "escape"};

        for (Function f : internalFuncs) {
            String name = f.getName().toLowerCase();
            boolean relevant = false;
            for (String kw : keywords) {
                if (name.contains(kw)) {
                    relevant = true;
                    break;
                }
            }
            if (!relevant) continue;

            DecompileResults results = decomp.decompileFunction(f, 30, monitor);
            if (results != null && results.decompileCompleted()) {
                sb.append("--- [INTERNAL] ").append(f.getName())
                  .append(" @ ").append(f.getEntryPoint()).append(" ---\n");
                sb.append(results.getDecompiledFunction().getC()).append("\n\n");
            }
        }

        decomp.dispose();

        // Write output
        String outPath = "C:\\Users\\papha\\Documents\\bi2xio\\ghidra_work\\output\\"
                + currentProgram.getName() + "_decompiled.txt";
        PrintWriter pw = new PrintWriter(new FileWriter(outPath));
        pw.print(sb.toString());
        pw.close();

        println("[*] Decompilation written to: " + outPath);
    }
}
