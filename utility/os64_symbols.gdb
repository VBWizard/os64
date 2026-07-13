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
# The kernel calls debug_task_loaded() every time it finishes loading a program
# image (task.c).  The silent breakpoint below fires there, reads the globals it
# publishes, add-symbol-file's the matching binary, and resumes without stopping.
#
# IMPORTANT: the userland apps now each link at their OWN base (see
# userland/tools/app_bases.py), so several apps' symbols can be loaded AT THE
# SAME TIME without colliding.  That is what makes debugging a PIPELINE possible.
python
import gdb, os

# HISTORY, because this file used to say the opposite:
# Every os64 program used to link at the same base (0x400000). Keeping several
# apps' symbol files loaded at once therefore made GDB's address->symbol lookup
# a coin flip: a stop in one app got LABELED with another app's function (once
# observed: a hit on syscall_smoke's _start displayed as "streq at arg_echo.c").
# The workaround was to keep ONLY the most recently loaded app's symbols and
# remove the previous ones — which was fine while debugging was one-app-at-a-time.
#
# Pipelines ended that. `hello | upper` runs two programs AT ONCE, and unloading
# one to look at the other is useless. So the apps now get unique link bases and
# this autoloader ACCUMULATES: every program that loads keeps its symbols, because
# no two of them can claim the same address anymore. (Which vindicates the old
# 32-bit OS's unique-link-address trick — it was right all along.)
class Os64SymbolAutoload(gdb.Breakpoint):
    def __init__(self):
        super(Os64SymbolAutoload, self).__init__("debug_task_loaded", internal=True)
        self.silent = True
        self.loaded = set()      # (abspath, bias) pairs already add-symbol-file'd

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
            # Skip anything already loaded — userland/bin/app_bases.gdb (sourced
            # by .gdbinit) has usually loaded every app's symbols before the OS
            # even boots, so breakpoints resolve immediately instead of pending.
            if symfile and key not in self.loaded:
                cmd = "add-symbol-file %s" % symfile
                if bias:
                    cmd += " -o %#x" % bias
                gdb.execute(cmd, to_string=True)
                self.loaded.add(key)
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
