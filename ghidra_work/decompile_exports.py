# Ghidra headless script to decompile all exported functions
# @category Analysis
# @runtime Jython

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
import os

def run():
    program = currentProgram
    monitor = ConsoleTaskMonitor()
    
    decomp = DecompInterface()
    decomp.openProgram(program)
    
    # Get all functions
    fm = program.getFunctionManager()
    
    # Get the export table
    st = program.getSymbolTable()
    
    output_lines = []
    output_lines.append("=" * 80)
    output_lines.append("DECOMPILATION OF: %s" % program.getName())
    output_lines.append("=" * 80)
    output_lines.append("")
    
    # First, list all exports
    output_lines.append("--- EXPORTED SYMBOLS ---")
    export_addrs = set()
    for sym in st.getAllSymbols(False):
        if sym.isExternalEntryPoint():
            output_lines.append("  EXPORT: %s @ %s" % (sym.getName(), sym.getAddress()))
            export_addrs.add(sym.getAddress())
    output_lines.append("")
    
    # Decompile all functions, prioritizing exports
    funcs_to_decompile = []
    
    # First pass: exports
    func = fm.getFunctionAt(program.getMinAddress())
    func_iter = fm.getFunctions(True)
    while func_iter.hasNext():
        f = func_iter.next()
        if f.getEntryPoint() in export_addrs:
            funcs_to_decompile.insert(0, f)  # exports first
        else:
            funcs_to_decompile.append(f)
    
    for f in funcs_to_decompile:
        is_export = f.getEntryPoint() in export_addrs
        
        # Only decompile exports and functions they reference
        if not is_export:
            # Check if function name contains relevant keywords
            name = f.getName().lower()
            keywords = ["aio", "bio", "bi2x", "bi2a", "tdj", "iob", "sci", "usb", "cdc", 
                        "nmgr", "node", "firm", "poll", "send", "recv", "read", "write",
                        "led", "lamp", "tape", "button", "slider", "turntable", "woofer",
                        "iccr", "status", "create", "destroy", "manage", "update", "init",
                        "serial", "com", "port", "device", "open", "close"]
            if not any(k in name for k in keywords):
                continue
        
        results = decomp.decompileFunction(f, 30, monitor)
        
        if results and results.decompileCompleted():
            tag = "[EXPORT]" if is_export else "[INTERNAL]"
            output_lines.append("--- %s %s @ %s ---" % (tag, f.getName(), f.getEntryPoint()))
            output_lines.append(results.getDecompiledFunction().getC())
            output_lines.append("")
        else:
            output_lines.append("--- FAILED TO DECOMPILE: %s ---" % f.getName())
            output_lines.append("")
    
    decomp.dispose()
    
    # Write output
    out_dir = r"C:\Users\papha\Documents\bi2xio\ghidra_work\output"
    out_path = os.path.join(out_dir, program.getName() + "_decompiled.txt")
    with open(out_path, "w") as fout:
        fout.write("\n".join(output_lines))
    
    print("[*] Decompilation written to: %s" % out_path)

run()
