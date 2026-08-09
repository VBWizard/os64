// watch — repeatedly ask husk to run one command line.
//
// Parsing the command belongs to the shell.  watch owns only repetition and
// presentation; /bin/husk -c owns pipelines, redirections, expansion, and all
// the syntax those features acquire in the future.

#include "os64/os64.h"

#define DEFAULT_INTERVAL_MS 2000ULL
#define MAX_INTERVAL_MS     86400000ULL

static bool parse_uint(const char *text, uint64_t *value)
{
	if (text == NULL || *text == '\0')
		return false;

	uint64_t n = 0;
	for (const char *p = text; *p != '\0'; p++)
	{
		if (*p < '0' || *p > '9')
			return false;
		uint64_t digit = (uint64_t)(*p - '0');
		if (n > (UINT64_MAX - digit) / 10)
			return false;
		n = n * 10 + digit;
	}
	*value = n;
	return true;
}

// Parse seconds with up to millisecond precision: 2, 0.5, 1.025.  The
// scheduler rounds the resulting milliseconds up to its active tick rate.
static bool parse_interval(const char *text, uint64_t *milliseconds)
{
	if (text == NULL || *text == '\0')
		return false;

	uint64_t seconds = 0;
	int whole_digits = 0;
	const char *p = text;
	while (*p >= '0' && *p <= '9')
	{
		uint64_t digit = (uint64_t)(*p - '0');
		if (seconds > (MAX_INTERVAL_MS / 1000 - digit) / 10)
			return false;
		seconds = seconds * 10 + digit;
		whole_digits++;
		p++;
	}
	if (whole_digits == 0)
		return false;

	uint64_t fraction = 0;
	uint64_t place = 100;
	if (*p == '.')
	{
		p++;
		if (*p < '0' || *p > '9')
			return false;
		while (*p >= '0' && *p <= '9')
		{
			if (place == 0)       // more precision than sleep can express
				return false;
			fraction += (uint64_t)(*p - '0') * place;
			place /= 10;
			p++;
		}
	}
	if (*p != '\0')
		return false;

	uint64_t result = seconds * 1000 + fraction;
	if (result == 0 || result > MAX_INTERVAL_MS)
		return false;
	*milliseconds = result;
	return true;
}

static void print_title(const char *command, uint64_t interval_ms,
	                     uint64_t run, uint64_t repetitions)
{
	if (interval_ms % 1000 == 0)
		os64_printf("Every %lus: %s", (unsigned long)(interval_ms / 1000),
		            command);
	else
		os64_printf("Every %lu.%03lus: %s",
		            (unsigned long)(interval_ms / 1000),
		            (unsigned long)(interval_ms % 1000), command);

	if (repetitions != 0)
		os64_printf("  [%lu/%lu]", (unsigned long)run,
		            (unsigned long)repetitions);
	else
		os64_printf("  [run %lu]", (unsigned long)run);
	os64_puts("\n\n");
}

int main(int argc, char **argv)
{
	bool append = false;
	bool no_title = false;
	bool errexit = false;
	const char *interval_text = NULL;
	const char *repetitions_text = NULL;
	const char *command = NULL;
	const os64_optspec_t specs[] = {
		{'n', "interval", true, "seconds between runs (default 2)",
		 .value_out = &interval_text},
		{'a', "append", false, "append each result instead of clearing",
		 .flag = &append},
		{'t', "no-title", false, "do not print the watch heading",
		 .flag = &no_title},
		{'e', "errexit", false, "stop when the command returns nonzero",
		 .flag = &errexit},
		{'r', "repetitions", true, "stop after this many runs",
		 .value_out = &repetitions_text}
	};
	os64_args_t args = {0};
	os64_args_init(&args, argc, argv, specs, 5);
	args.about = "Run a husk command line repeatedly.";
	args.details = "Quote pipelines and compound commands so the invoking shell "
	               "passes them as one command string.";
	int32_t parsed = os64_args_parse(
		&args, "watch [-aet] [-n SECONDS] [-r COUNT] \"COMMAND\"",
		&command, 1);
	if (parsed == OS64_ARG_HELP)
		return 0;
	if (parsed < 0)
		return 2;
	if (parsed != 1 || command[0] == '\0')
	{
		os64_args_help(&args,
			"watch [-aet] [-n SECONDS] [-r COUNT] \"COMMAND\"");
		return 2;
	}

	uint64_t interval_ms = DEFAULT_INTERVAL_MS;
	if (interval_text != NULL && !parse_interval(interval_text, &interval_ms))
	{
		os64_hprintf(OS64_STDERR,
		             "watch: interval must be greater than 0 and no more than "
		             "86400 seconds (up to 3 decimal places)\n");
		return 2;
	}

	uint64_t repetitions = 0;       // zero means forever
	if (repetitions_text != NULL &&
	    (!parse_uint(repetitions_text, &repetitions) || repetitions == 0))
	{
		os64_hprintf(OS64_STDERR,
		             "watch: repetitions must be a positive integer\n");
		return 2;
	}

	char *husk_argv[] = {"husk", "-c", (char *)command, NULL};
	int32_t command_status = 0;
	for (uint64_t run = 1; repetitions == 0 || run <= repetitions; run++)
	{
		if (!append)
			os64_write(OS64_STDOUT, "\f", 1);
		else if (run > 1)
			os64_puts("\n");       // separate runs even after unterminated output

		if (!no_title)
			print_title(command, interval_ms, run, repetitions);

		int64_t pid = os64_spawn("/bin/husk", husk_argv);
		if (pid < 0)
		{
			os64_hprintf(OS64_STDERR, "watch: cannot launch /bin/husk\n");
			return 1;
		}
		if (os64_wait(pid, &command_status) < 0)
		{
			os64_hprintf(OS64_STDERR, "watch: cannot wait for husk\n");
			return 1;
		}

		if (errexit && command_status != 0)
			return command_status;
		if (repetitions != 0 && run == repetitions)
			break;
		os64_sleep(interval_ms);
	}

	return command_status;
}
