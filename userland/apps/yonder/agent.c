// agent.c — who yonder says it is (agent.h).

#include "agent.h"
#include "os64/str.h"

// Each spelled as that browser spells it, at about the versions current in
// September 2026; a site that checks the number more closely can be handed
// another in the field. Chrome and Firefox as on Windows 10 and 11, which
// both report NT 10.0; Safari as on an iPhone, which many sites answer
// with a lighter page; Netscape 4, which the vintage-web sites answer as
// the old browser it is; and Lynx, which some sites answer with a
// text-only version.
static const yonder_agent_preset_t kPresets[] = {
    {"yonder", YONDER_AGENT},
    {"Chrome on Windows",
     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
     "Chrome/154.0.0.0 Safari/537.36"},
    {"Firefox on Windows",
     "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:156.0) Gecko/20100101 Firefox/156.0"},
    {"Safari on iPhone",
     "Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 "
     "(KHTML, like Gecko) Version/26.0 Mobile/15E148 Safari/604.1"},
    {"Netscape 4 on Windows 98", "Mozilla/4.08 [en] (Win98; U)"},
    {"Lynx", "Lynx/2.9.2 libwww-FM/2.14 SSL-MM/1.4.1 OpenSSL/3.0.13"},
};

size_t yonder_agent_npresets(void)
{
    return sizeof(kPresets) / sizeof(kPresets[0]);
}

const yonder_agent_preset_t *yonder_agent_preset(size_t i)
{
    return i < yonder_agent_npresets() ? &kPresets[i] : NULL;
}

bool yonder_agent_valid(const char *agent)
{
    if (agent == NULL || agent[0] == ' ' || agent[0] == '\0')
        return false;
    size_t n = 0;
    for (; agent[n] != '\0'; n++)
        if (agent[n] < 0x20 || agent[n] > 0x7e || agent[n] == '#')
            return false;
    return n < YONDER_AGENT_MAX && agent[n - 1] != ' ';
}

const char *yonder_agent_name(const char *agent)
{
    for (size_t i = 0; agent != NULL && i < yonder_agent_npresets(); i++)
        if (os64_streq(agent, kPresets[i].agent))
            return kPresets[i].name;
    return NULL;
}
