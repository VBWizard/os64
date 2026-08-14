// upper.c — THE TEMPLATE FILTER. Read stdin, transform, write stdout.
//
// This is the shape every os64 utility takes, and it is deliberately the
// smallest complete example of it. `ls`, `cat`, `grep`, `wc`, `sort` — they all
// differ only in what happens between the read and the write. Steal this file.
//
// Three things make it a filter, and all three matter:
//
//   1. It reads handle 0 and writes handle 1 and NEVER ASKS WHAT THEY ARE.
//      Run it alone and 0 is the keyboard, 1 is the console. Run it in a
//      pipeline and the shell has quietly pointed them at pipes. This program
//      cannot tell the difference and contains ZERO lines of pipe-awareness.
//      That indirection is the entire reason small programs compose.
//
//   2. It loops until read returns 0. A short read is normal — you get what is
//      available right now, not a filled buffer — so a filter processes what it
//      gets and comes back for more. Zero means end of input: every writer on
//      the other end has closed. It is not a byte in the stream, it is their
//      absence.
//
//   3. It is STREAMING. It never holds the whole input in memory — one buffer,
//      reused. That is what lets `bigfile | upper | grep x` run in constant
//      memory no matter how big the file is, and it is why a pipe's fixed
//      capacity (which blocks a fast writer until the reader catches up) is a
//      feature and not a limit.
//
// Usage:  upper            (type, see it shouted back)
//         hello | upper    (the first os64 pipeline)
//
// File operands use the cat model: transform each in order; no operands means
// stdin. A lone "-" names stdin, so files and a pipeline can be interleaved.

#include "os64/os64.h"

#define BUF_SIZE 512

static int write_all(const char *data, size_t length)
{
	size_t written = 0;
	while (written < length)
	{
		int64_t n = os64_write(OS64_STDOUT, data + written, length - written);
		if (n <= 0)
			return -1;
		written += (size_t)n;
	}
	return 0;
}

static int upper_handle(int32_t handle, const char *name)
{
	char buf[BUF_SIZE];
	int64_t n;

	// The canonical filter loop. Every utility you write will have this spine.
	while ((n = os64_read(handle, buf, sizeof(buf))) > 0)
	{
		// The only interesting line in the program: the transform.
		for (long i = 0; i < n; i++)
			if (buf[i] >= 'a' && buf[i] <= 'z')
				buf[i] = (char)(buf[i] - 'a' + 'A');

		// Write what we transformed — exactly n bytes, no more, no less. If the
		// consumer downstream is slow, this BLOCKS until there is room, which
		// throttles us to its speed. We sleep; we do not spin.
		if (write_all(buf, (size_t)n) < 0)
		{
			os64_hprintf(OS64_STDERR, "upper: error writing standard output\n");
			return -1;
		}
	}

	if (n < 0)
	{
		os64_hprintf(OS64_STDERR, "upper: error reading %s\n", name);
		return -1;
	}
	return 0;
}

static int upper_path(const char *path)
{
	if (os64_streq(path, "-"))
		return upper_handle(OS64_STDIN, "standard input");

	os64_dirent_t entry = {0};
	if (os64_stat(path, &entry) < 0)
	{
		os64_hprintf(OS64_STDERR, "upper: cannot stat '%s'\n", path);
		return -1;
	}
	if (entry.flags & OS64_DE_DIR)
	{
		os64_hprintf(OS64_STDERR, "upper: '%s' is a directory\n", path);
		return -1;
	}

	int32_t handle = (int32_t)os64_open(path, "r");
	if (handle < 0)
	{
		os64_hprintf(OS64_STDERR, "upper: cannot open '%s'\n", path);
		return -1;
	}
	int result = upper_handle(handle, path);
	if (os64_close(handle) < 0)
	{
		os64_hprintf(OS64_STDERR, "upper: cannot close '%s'\n", path);
		result = -1;
	}
	return result;
}

int main(int argc, char **argv, char **envp)
{
	(void)envp;
	os64_args_t args = {0};
	os64_args_init(&args, argc, argv, NULL, 0);
	args.about = "Convert ASCII lowercase text to uppercase.";
	args.details = "With no FILE, or when FILE is -, read standard input.";

	int32_t inputCount = 0;
	int32_t result;
	while ((result = os64_args_next(&args)) != OS64_ARG_END)
	{
		if (result == OS64_ARG_POSITIONAL)
		{
			inputCount++;
			continue;
		}
		if (result == OS64_ARG_HELP)
		{
			os64_args_help(&args, "upper [FILE ...]");
			return 0;
		}
		os64_args_help(&args, "upper [FILE ...]");
		return 2;
	}

	if (inputCount == 0)
		return upper_handle(OS64_STDIN, "standard input") == 0 ? 0 : 1;

	int returnCode = 0;
	os64_args_init(&args, argc, argv, NULL, 0);
	while ((result = os64_args_next(&args)) != OS64_ARG_END)
		if (result == OS64_ARG_POSITIONAL && upper_path(args.value) < 0)
			returnCode = 1;
	return returnCode;
}
