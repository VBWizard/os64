# Testing a program you wrote for os64, in QEMU

You have written a utility in `userland/apps/<NAME>/`. It cannot run on this
host: it is an os64 binary and the host is Linux. Booting os64 in QEMU is the
only way to see it actually run, and the only evidence worth reporting.

The virtual machine boots **copies** of the disk images, so nothing you do in
it touches the repository. It answers to a monitor socket on port 55556.

## The loop

    1. make                                     build the OS with your program in it
    2. tools/vmboot                             boot it (about 25 seconds)
    3. tools/vmtype 55556 '<your command>'      type the command into the shell
    4. tools/vmshot /tmp/screen.png             photograph the screen, and look at it
    5. tools/vmget /home/out.txt /tmp/out.txt   fetch a file the program wrote
    6. tools/vmstop                             stop the VM when you are done

Steps 3 to 5 repeat as often as you like while the VM stays up.

## 1. Build

Run `make` from the repository root. Not `make -C userland`: that builds your
program but does not put it on the disk image the VM boots, so the VM would
run the version from before your change. A `make` after a one-file edit takes
well under a minute.

## 2. Boot

    tools/vmboot

It prints `vmboot: up on port 55556 (scratch /tmp/os64-vm-55556)` when the
shell is ready, after about 25 seconds. If it says anything else, read **When
something goes wrong** below.

The VM keeps running until you stop it, so leave it up between tests.

## 3. Run your program

    tools/vmtype 55556 'dd if=/dev/zero of=/home/zeros.bin bs=512 count=4'

`vmtype` types the text as keystrokes and presses Enter, exactly as a person
at the keyboard would. Two consequences:

- It can only send what a US keyboard has a key for. It tells you when it
  skipped a character it has no key for.
- The guest needs a moment to run the command. **Wait about 3 seconds before
  looking at the screen or fetching a file** — `sleep 3` in the same command
  is enough. A command that does real work (megabytes of copying) needs
  longer.

## 4. Look at the screen

    tools/vmshot /tmp/screen.png

Then open the PNG. The console is 1024x768, oldest lines at the top, your
commands and their output at the bottom. This is where a program's messages,
its complaints, and any crash report appear.

## 5. Get the exact bytes

A screenshot cannot tell you whether a program wrote 511 bytes or 512, and
cannot show you a binary at all. So have the program write into `/home`,
flush the guest's disk, and fetch the file:

    tools/vmtype 55556 'sync'
    sleep 2
    tools/vmget /home/zeros.bin /tmp/zeros.bin

`sync` matters: without it the bytes may still be in the guest when the host
reads the disk. Then check the file with ordinary host tools:

    ls -l /tmp/zeros.bin          # the size, which is usually the whole test
    od -An -tx1 -N32 /tmp/zeros.bin
    cmp /tmp/zeros.bin /tmp/expected.bin

`tools/vmls` lists the guest's `/home` (names and sizes) without fetching
anything — useful to check a file was created at all.

To compare what your program produced against a file that shipped with the
system, fetch the original too:

    tools/vmget --root /bin/cat /tmp/cat-original
    cmp /tmp/cat-original /tmp/catcopy

`--root` reads the system's own filesystem; without it the path is inside
`/home`.

## 6. Stop

    tools/vmstop

Always stop the VM when you are finished with it.

## Writing a test that proves something

- **Write test output under `/home`.** It is the only place the host can
  fetch files from.
- **Capture the exit code**, because it is half of a utility's contract:

      tools/vmtype 55556 'mytool --bad-flag'
      sleep 3
      tools/vmtype 55556 'echo rc=$? > /home/rc.txt'
      sleep 2
      tools/vmtype 55556 'sync'
      sleep 2
      tools/vmget /home/rc.txt /tmp/rc.txt

- **Inputs you can rely on:** `/dev/zero`, `/dev/null`, `/dev/full`,
  `/dev/random`, every program in `/bin` and `/tests`, and the text files in
  `/etc`. Make your own with `echo something > /home/in.txt` or
  `cp /bin/cat /home/in.bin`.
- **Test the failures too:** a file that does not exist, a bad flag, an empty
  input, a size that does not divide evenly. Each one: run it, look at the
  message on screen, capture `$?`.

### A worked session: testing `dd`

    make
    tools/vmboot

    tools/vmtype 55556 'dd if=/dev/zero of=/home/zeros.bin bs=512 count=4'
    sleep 3
    tools/vmtype 55556 'echo rc=$? > /home/rc.txt'
    sleep 2
    tools/vmtype 55556 'sync'
    sleep 2
    tools/vmget /home/zeros.bin /tmp/zeros.bin
    tools/vmget /home/rc.txt /tmp/rc.txt

Expect 2048 bytes and `rc=0`. Then prove it copies real bytes faithfully:

    tools/vmtype 55556 'dd if=/bin/cat of=/home/catcopy'
    sleep 5
    tools/vmtype 55556 'sync'
    sleep 2
    tools/vmget /home/catcopy /tmp/catcopy
    tools/vmget --root /bin/cat /tmp/cat-original
    cmp /tmp/cat-original /tmp/catcopy

`cmp` silent means identical, and that is the verdict. Then the awkward
cases, one at a time: a `count=` that stops short of the end of the file, a
`bs=` that does not divide the file evenly, `if=` a file that does not exist,
and `/dev/zero` with no `count=`. Look at the screen after each one.

    tools/vmshot /tmp/screen.png
    tools/vmstop

## When something goes wrong

| What you see | What it means |
|---|---|
| `vmboot: no os64_kernel.iso — run make first` | nothing has been built yet |
| the screen shows `husk: cannot run mytool` | your program is not on the image: you skipped `make` at the repository root, or the build failed |
| `vmget: could not read ...` | the program did not create the file, or wrote it somewhere other than `/home`, or you did not `sync` first. Look at the screenshot for its error message |
| the screen shows the command but no output | the program printed nothing — check it writes to handle 1 and returns from `main` |
| `Segmentation fault: mytool (task N, ...)` on screen | your program crashed; that line carries the exit code |
| `Fatal exception: mytool (task N, #DE) - exit 200` | a CPU fault of your own making — `#DE` is divide by zero |
| `vmboot: no 'boot complete' after 180s` | the OS did not finish booting: read `/tmp/os64-vm-55556/serial.log`, where a kernel panic is printed in full |
| the screen looks stale | you looked too soon; `sleep 3` and take another screenshot |

## Rules

- **Never write to `disk/`.** The VM boots copies in `/tmp`; those images
  belong to the build.
- **Never run `pkill qemu`.** The pattern matches your own command line and
  other people's virtual machines. `tools/vmstop` stops only yours.
- **Use port 55556.** Port 55555 belongs to the person you are working with.
- **Do not run `userland/bin/<NAME>` on this host.** It cannot run here and
  the attempt proves nothing.
- **Do not commit.** Report what you changed and what the tests showed.

## Anything else the monitor can do

    tools/vmcmd 55556 info registers
    tools/vmcmd 55556 info block

`tools/vmcmd` sends one QEMU monitor command and prints the reply. You will
rarely need it; `vmboot`, `vmtype`, `vmshot`, `vmget`, `vmls` and `vmstop`
cover ordinary testing.
