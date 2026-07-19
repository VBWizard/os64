// args.c — libos64 argument parsing. The design case against getopt is in
// os64/args.h; this file is just the small machine that honors it.
//
// Pure computation over caller-owned state: no syscalls (except in
// os64_args_help, which prints), no allocation, argv never modified. Like
// fmt.c, that makes it host-testable with plain gcc — tools/test_fmt_host.c
// drives both.

#include "os64/args.h"
#include "os64/fmt.h"

static int a_streq_n(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (a[i] != b[i] || a[i] == '\0')
			return 0;
	return b[n] == '\0';
}

void os64_args_init(os64_args_t *a, int argc, char **argv,
                    const os64_optspec_t *specs, int nspecs)
{
	a->argc = argc;
	a->argv = argv;
	a->specs = specs;
	a->nspecs = nspecs;
	a->about = NULL;       // opt-in: set it yourself after init (see args.h)
	a->index = 1;          // argv[0] is the program name, not an argument
	a->bundle = NULL;
	a->no_more_opts = 0;
	a->value = NULL;
}

static const os64_optspec_t *find_short(const os64_args_t *a, char c)
{
	for (int i = 0; i < a->nspecs; i++)
		if (a->specs[i].letter == c)
			return &a->specs[i];
	return NULL;
}

static const os64_optspec_t *find_long(const os64_args_t *a, const char *name, size_t len)
{
	for (int i = 0; i < a->nspecs; i++)
		if (a->specs[i].name != NULL && a_streq_n(name, a->specs[i].name, len))
			return &a->specs[i];
	return NULL;
}

// Deliver one short option (from a lone "-x" or inside a "-la" bundle).
static int deliver_short(os64_args_t *a, char c, const char *token)
{
	const os64_optspec_t *spec = find_short(a, c);
	if (spec == NULL)
	{
		if (c == 'h')
			return OS64_ARG_HELP;   // unclaimed -h is a plea for help
		a->value = token;
		return OS64_ARG_ERROR;
	}
	if (spec->takes_value)
	{
		// A value option must not sit mid-bundle ("-ol file" is ambiguous
		// noise; "-o file" or "--out=file" say what they mean).
		if (a->bundle != NULL && *a->bundle != '\0')
		{
			a->value = token;
			return OS64_ARG_ERROR;
		}
		if (a->index >= a->argc)
		{
			a->value = token;
			return OS64_ARG_ERROR;   // "-o" with nothing after it
		}
		a->value = a->argv[a->index++];
	}
	return spec->letter;
}

int os64_args_next(os64_args_t *a)
{
	a->value = NULL;

	// Continue an in-progress "-la" bundle first.
	if (a->bundle != NULL)
	{
		if (*a->bundle != '\0')
		{
			char c = *a->bundle++;
			return deliver_short(a, c, a->argv[a->index - 1]);
		}
		a->bundle = NULL;
	}

	if (a->index >= a->argc)
		return OS64_ARG_END;

	char *tok = a->argv[a->index];

	// After "--", or for anything not option-shaped (including a lone "-",
	// which conventionally names stdin): positional.
	if (a->no_more_opts || tok[0] != '-' || tok[1] == '\0')
	{
		a->index++;
		a->value = tok;
		return OS64_ARG_POSITIONAL;
	}

	if (tok[1] == '-')
	{
		// "--" alone: end of options, consumed silently.
		if (tok[2] == '\0')
		{
			a->index++;
			a->no_more_opts = 1;
			return os64_args_next(a);
		}

		// "--name" or "--name=value".
		a->index++;
		const char *name = tok + 2;
		size_t len = 0;
		while (name[len] != '\0' && name[len] != '=')
			len++;

		if (len == 4 && a_streq_n(name, "help", 4) && find_long(a, "help", 4) == NULL)
			return OS64_ARG_HELP;

		const os64_optspec_t *spec = find_long(a, name, len);
		if (spec == NULL)
		{
			a->value = tok;
			return OS64_ARG_ERROR;
		}
		if (spec->takes_value)
		{
			if (name[len] == '=')
				a->value = name + len + 1;
			else if (a->index < a->argc)
				a->value = a->argv[a->index++];
			else
			{
				a->value = tok;
				return OS64_ARG_ERROR;
			}
		}
		else if (name[len] == '=')
		{
			a->value = tok;   // "--all=x" when all takes nothing: nonsense
			return OS64_ARG_ERROR;
		}
		return spec->letter;
	}

	// "-x" or a "-la" bundle: consume the token, walk its letters.
	a->index++;
	a->bundle = tok + 2;
	return deliver_short(a, tok[1], tok);
}

void os64_args_help(const os64_args_t *a, const char *usage)
{
    if (a->about != NULL)
        os64_printf("%s\n", a->about);
    os64_printf("  usage: %s\n", usage);
    if (a->specs == NULL)
		return;
	for (int i = 0; i < a->nspecs; i++)
	{
		const os64_optspec_t *s = &a->specs[i];
		// A blank row (letter 0, no name) prints NOTHING — it's what a
		// well-meaning `specs[] = {{}}` produces, and rendering it printed
		// "-" followed by character zero (pwd found this on day one).
		if (s->letter == '\0' && s->name == NULL)
			continue;
		// "  -l, --long   help text" — generated from the table, so the help
		// and the parser can never disagree about what exists.
		if (s->name != NULL)
			os64_printf("  -%c, --%-12s %s\n", s->letter, s->name,
			            s->help ? s->help : "");
		else
			os64_printf("  -%c    %-12s %s\n", s->letter, "",
			            s->help ? s->help : "");
	}
}
