// agent.c — who yonder says it is (agent.h).

#include "agent.h"
#include "os64/str.h"

// Each spelled as that browser spells it; a site that checks the number
// more closely can be handed another in the field. Chrome and Firefox as on
// Windows 10 and 11, which both report NT 10.0: their strings and their
// lines are what Chrome 156 and Firefox 158 sent a local listener in
// October 2026, byte for byte, header names in their own case. Safari as
// on an iPhone, which many sites answer with a lighter page: not captured,
// its string as published in September 2026, no client hints (Safari has
// none), and the language list MDN's Accept-Language page gives for
// Chrome and Safari alike. Netscape 4, which the vintage-web
// sites answer as the old browser it is, and Lynx, which some sites answer
// with a text-only version, claim no lines beyond their strings; nor does
// yonder's own.
static const yonder_agent_preset_t kPresets[] = {
    {"yonder", YONDER_AGENT, {NULL, NULL}},
    {"Chrome on Windows",
     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
     "Chrome/156.0.0.0 Safari/537.36",
     {"sec-ch-ua: \"Not:A-Brand\";v=\"8\", \"Chromium\";v=\"156\", \"Google Chrome\";v=\"156\"\r\n"
      "sec-ch-ua-mobile: ?0\r\n"
      "sec-ch-ua-platform: \"Windows\"\r\n",
      "en-US,en;q=0.9"}},
    {"Firefox on Windows",
     "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:158.0) Gecko/20100101 Firefox/158.0",
     {NULL, "en-US,en;q=0.9"}},
    {"Safari on iPhone",
     "Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 "
     "(KHTML, like Gecko) Version/26.0 Mobile/15E148 Safari/604.1",
     {NULL, "en-US,en;q=0.9"}},
    {"Netscape 4 on Windows 98", "Mozilla/4.08 [en] (Win98; U)", {NULL, NULL}},
    {"Lynx", "Lynx/2.9.2 libwww-FM/2.14 SSL-MM/1.4.1 OpenSSL/3.0.13", {NULL, NULL}},
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

const way_identity_t *yonder_agent_identity(const char *agent)
{
    for (size_t i = 0; agent != NULL && i < yonder_agent_npresets(); i++)
        if (os64_streq(agent, kPresets[i].agent))
            return &kPresets[i].identity;
    return NULL;
}
