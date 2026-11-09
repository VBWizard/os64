// randtest — /dev/random's door, exercised the way BearSSL will use it.
//
// Opens the node, reads a seed's worth twice, and checks the two are
// different and neither is a single repeated byte; writes a contribution
// back (consumed whole, like a write to /dev/null); then reads /sys/random
// and requires `seeded: yes`. It proves the DOOR — the pool's construction
// is proven on the host (tools/test_random_host.sh) and the CPU's
// instructions by /tests/rngprobe; this is the piece between them.
//
// Exit codes, the ring3-fixture convention: one magic success value, a
// distinct code per step for the autopsy.
//   0x5EED0000  success            0x5EED0004  the two reads were identical
//   0x5EED0001  open refused       0x5EED0005  a read was one repeated byte
//   0x5EED0002  first read short   0x5EED0006  write refused or short
//   0x5EED0003  second read short  0x5EED0007  /sys/random missing or not seeded
//   0x5EED0008  a read past a page was not cut to one
//   0x5EED0009  a write past a page was not cut to one

#include "os64/os64.h"

#define RANDTEST_OK           0x5EED0000
#define RANDTEST_NO_OPEN      0x5EED0001
#define RANDTEST_SHORT_1      0x5EED0002
#define RANDTEST_SHORT_2      0x5EED0003
#define RANDTEST_SAME         0x5EED0004
#define RANDTEST_CONSTANT     0x5EED0005
#define RANDTEST_NO_WRITE     0x5EED0006
#define RANDTEST_NOT_SEEDED   0x5EED0007
#define RANDTEST_NO_CAP       0x5EED0008
#define RANDTEST_NO_WRITE_CAP 0x5EED0009

#define SEED_BYTES 32
#define PAGE_BYTES 4096     // the most one read of /dev/random serves (RANDOM.md)

static bool constant(const uint8_t* b, size_t n)
{
	for (size_t i = 1; i < n; i++)
		if (b[i] != b[0])
			return false;
	return true;
}

// libos64 has no substring search yet (nothing has asked); this one is
// the fixture's own.
static bool contains(const char* text, const char* needle)
{
	size_t n = os64_strlen(needle);
	for (const char* p = text; *p; p++)
	{
		size_t i = 0;
		while (i < n && p[i] == needle[i])
			i++;
		if (i == n)
			return true;
	}
	return false;
}

int main(int argc, char** argv)
{
	(void)argc; (void)argv;

	int64_t h = os64_open("/dev/random", "r");
	if (h < 0)
		return RANDTEST_NO_OPEN;

	uint8_t a[SEED_BYTES], b[SEED_BYTES];
	if (os64_read((int32_t)h, a, SEED_BYTES) != SEED_BYTES) { os64_close((int32_t)h); return RANDTEST_SHORT_1; }
	if (os64_read((int32_t)h, b, SEED_BYTES) != SEED_BYTES) { os64_close((int32_t)h); return RANDTEST_SHORT_2; }
	// A request past a page is cut to a page: the device bounds the work
	// it does with the caller's interrupts off, and says so by a short
	// read rather than by refusing.
	static uint8_t big[PAGE_BYTES + 1];
	if (os64_read((int32_t)h, big, sizeof(big)) != PAGE_BYTES) { os64_close((int32_t)h); return RANDTEST_NO_CAP; }
	os64_close((int32_t)h);

	bool same = true;
	for (size_t i = 0; i < SEED_BYTES; i++)
		if (a[i] != b[i]) { same = false; break; }
	if (same)
		return RANDTEST_SAME;
	if (constant(a, SEED_BYTES) || constant(b, SEED_BYTES))
		return RANDTEST_CONSTANT;

	// A contribution: the pool takes it with no credit and no complaint.
	int64_t w = os64_open("/dev/random", "w");
	if (w < 0)
		return RANDTEST_NO_WRITE;
	int64_t wrote = os64_write((int32_t)w, a, SEED_BYTES);
	if (wrote != SEED_BYTES) { os64_close((int32_t)w); return RANDTEST_NO_WRITE; }
	// And a contribution past a page is taken a page at a time, for the
	// same reason a read is served that way.
	wrote = os64_write((int32_t)w, big, sizeof(big));
	os64_close((int32_t)w);
	if (wrote != PAGE_BYTES)
		return RANDTEST_NO_WRITE_CAP;

	// The eyes must agree the door is open for a reason.
	int64_t s = os64_open("/sys/random", "r");
	if (s < 0)
		return RANDTEST_NOT_SEEDED;
	char text[1024];
	int64_t n = os64_read((int32_t)s, text, sizeof(text) - 1);
	os64_close((int32_t)s);
	if (n <= 0)
		return RANDTEST_NOT_SEEDED;
	text[n] = '\0';
	if (!contains(text, "seeded: yes"))
		return RANDTEST_NOT_SEEDED;

	os64_printf("randtest: /dev/random answered %d + %d bytes, distinct, cut a %d-byte ask to a page, took a contribution; /sys/random says seeded\n",
	            SEED_BYTES, SEED_BYTES, PAGE_BYTES + 1);
	return RANDTEST_OK;
}
