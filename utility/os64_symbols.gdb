# os64 shared GDB setup — sourced by BOTH ./.gdbinit (CLI gdb) and
# .vscode/launch.json setupCommands (VS Code cppdbg), so app-symbol
# autoloading behaves identically everywhere.  Keep session-specific stuff
# (target remote, kernel symbol-file, personal breakpoints) OUT of this file.

# add-symbol-file normally asks "y or n"; under VS Code's MI console prompts
# can't be answered ("input not from terminal") and get auto-declined, which
# silently throws breakpoints away.  Never prompt.
set confirm off

# Breakpoints named before their program's symbols exist should wait for the
# symbols instead of failing (they resolve the moment the autoloader below
# add-symbol-file's the app).
set breakpoint pending on

# ── Automatic per-program symbol loading ─────────────────────────────────────
# The kernel calls debug_task_loaded(path, load_bias) every time it finishes
# loading a program image (task.c).  The silent breakpoint below fires there,
# reads the arguments, add-symbol-file's kernel/bin/<basename> (offset by the
# load bias for PIE binaries), and resumes without stopping.  Every app's
# debug info is available the moment the OS creates the task — no manual
# loading, no unique-link-address tricks; all binaries happily at 0x400000.
python
import gdb, os

# Every os64 program links at the same base (0x400000), so keeping several
# apps' symbol files loaded at once makes GDB's address->symbol lookup a coin
# flip: a stop in one app gets LABELED with another app's function (observed:
# a hit on syscall_smoke's _start displayed as "streq at arg_echo.c:64").
# Since debugging is one-app-at-a-time in practice, the autoloader keeps ONLY
# the most recently loaded app's symbols: previous ones are removed when a new
# program loads.  Breakpoints against a not-yet-loaded (or unloaded) app
# simply go pending and resolve when it (re)appears.
class Os64SymbolAutoload(gdb.Breakpoint):
    def __init__(self):
        super(Os64SymbolAutoload, self).__init__("debug_task_loaded", internal=True)
        self.silent = True
        self.current = None      # (abspath, bias) of the app symbols now loaded

    def stop(self):
        try:
            # Read the kernel-image GLOBALS, not function arguments — globals
            # live at fixed, always-mapped kernel addresses, so this works
            # under any CR3 and any frame state (argument parsing did not).
            path = gdb.parse_and_eval("kDebugTaskLoadedPath").string()
            bias = int(gdb.parse_and_eval("kDebugTaskLoadedBias"))
            name = os.path.basename(path)
            # Search both binary output dirs: the kernel test fixtures live in
            # kernel/bin, real userland apps (hello, and everything after it)
            # in userland/bin. Without userland/bin here, an app's symbols
            # silently never load — which is exactly why /bin/hello showed no
            # source in the debugger.
            symfile = None
            for d in (os.path.join("kernel", "bin"), os.path.join("userland", "bin")):
                cand = os.path.abspath(os.path.join(d, name))
                if os.path.exists(cand):
                    symfile = cand
                    break
            key = (symfile, bias)
            if symfile and key != self.current:
                if self.current is not None:
                    gdb.execute("remove-symbol-file %s" % self.current[0],
                                to_string=True)
                cmd = "add-symbol-file %s" % symfile
                if bias:
                    cmd += " -o %#x" % bias
                gdb.execute(cmd, to_string=True)
                self.current = key
                gdb.write("[os64] symbols: %s%s\n"
                          % (symfile, (" (+%#x)" % bias) if bias else ""))
        except Exception as exc:
            gdb.write("[os64] symbol autoload failed: %s\n" % exc)
        return False    # never actually stop here

Os64SymbolAutoload()

# $os64_app() — convenience function returning the exename of the task the
# CURRENT GDB THREAD's CPU is running ("/syscall_smoke", "/logd", ...).  Goes
# through the kernel's own kCoreLocalStorage array indexed by CPU (gdb thread
# N == CPU N-1 under QEMU) rather than $gs_base, which not every QEMU/gdb
# combination exposes — and a breakpoint condition that ERRORS makes gdb stop
# anyway, silently defeating the filter.  Use it to make an address breakpoint
# fire for ONE app even where apps' link addresses overlap:
#     hbreak syscall_smoke.c:_start if $_streq($os64_app(), "/syscall_smoke")
class Os64App(gdb.Function):
    def __init__(self):
        super(Os64App, self).__init__("os64_app")

    def invoke(self):
        try:
            cpu = gdb.selected_thread().num - 1
            cls = gdb.parse_and_eval(
                "((core_local_storage_t *)kCoreLocalStorage)[%d]" % cpu)
            task = cls["task"]
            if int(task) == 0:
                return ""
            return task["exename"].string()
        except Exception:
            return ""

Os64App()
end
