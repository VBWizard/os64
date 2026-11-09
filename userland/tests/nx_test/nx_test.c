// nx_test — the W^X fixture: it PASSES BY DYING, like the heap's crime pair.
//
// Two crimes, chosen by argv:
//
//   nx_test          (or `nx_test stack`) — build a tiny function ON THE
//                    STACK and call it. Smashed-stack shellcode is the oldest
//                    exploit in the book (the Morris worm ran on exactly
//                    this); a kernel that maps stacks PAGE_NO_EXECUTE turns
//                    the call into an instruction-fetch #PF and kills us.
//
//   nx_test text     — write one byte over main()'s own code. With the old
//                    single-RWX-segment link, this SUCCEEDED — every byte of
//                    every program was writable and executable at once. With
//                    the link script's page-aligned permission splits, .text
//                    arrives R+X and the store is a protection violation.
//
// Either way the correct outcome is the segfault kill (exit 139). SURVIVING
// is the failure, and the fixture says so out loud and exits 0x0BAD so
// testrun sees a wrong code rather than a hung test. This is the same
// philosophy as malloctest's doublefree/stomp entries: a tripwire nobody
// tests is a tripwire nobody knows is disconnected.
//
// (Why 0xb8 0x11 ... 0xc3 and not a nop: `mov eax, 0x11; ret` returns a
// recognizable value if it ever DOES run, so a survivor is diagnosable —
// "the stack executed and returned 0x11" beats "something happened".)

#include "os64/os64.h"

typedef int (*tiny_fn_t)(void);

int main(int argc, char **argv, char **envp)
{
	(void)envp;
	const char *mode = (argc > 1) ? argv[1] : "stack";

	if (os64_streq(mode, "text"))
	{
		os64_puts("nx_test: writing one byte over main()'s code...\n");
		*(volatile uint8_t *)(uintptr_t)main = 0x90;   // nop, if it lands
		os64_puts("nx_test: FAIL — wrote to .text and lived (text is writable)\n");
		return 0x0BAD;
	}

	os64_puts("nx_test: calling a function built on the stack...\n");
	uint8_t code[8];
	code[0] = 0xb8;                          // mov eax, imm32
	code[1] = 0x11; code[2] = 0x00; code[3] = 0x00; code[4] = 0x00;
	code[5] = 0xc3;                          // ret

	// The volatile hop keeps the compiler from proving this call absurd and
	// "optimizing" the crime away before the kernel gets to punish it.
	volatile tiny_fn_t f = (tiny_fn_t)(uintptr_t)code;
	int got = f();

	os64_hprintf(OS64_STDERR,
	             "nx_test: FAIL — the stack executed and returned 0x%x (NX is not enforced)\n",
	             (unsigned)got);
	return 0x0BAD;
}
