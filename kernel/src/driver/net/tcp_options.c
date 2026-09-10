// tcp_options.c — see tcp_options.h. No kernel headers: this file is
// compiled on the host by tools/test_tcp_options_host.sh.

#include "driver/net/tcp_options.h"

void tcp_syn_options_parse(const uint8_t* opts, size_t len, tcp_syn_options_t* out)
{
	out->mss = 0;
	out->wscale_sent = false;
	out->wscale = 0;

	size_t o = 0;
	while (o < len)
	{
		uint8_t kind = opts[o];
		if (kind == TCP_OPT_END)
			break;
		if (kind == TCP_OPT_NOP)
		{
			o++;
			continue;
		}
		if (o + 1 >= len)
			break;                         // a kind with no room for its length
		uint8_t olen = opts[o + 1];
		if (olen < 2 || o + olen > len)
			break;                         // malformed, or runs past the header
		if (kind == TCP_OPT_MSS && olen == 4)
			out->mss = (uint16_t)((opts[o + 2] << 8) | opts[o + 3]);
		else if (kind == TCP_OPT_WSCALE && olen == 3)
		{
			out->wscale_sent = true;
			out->wscale = opts[o + 2];
		}
		o += olen;
	}
}

size_t tcp_syn_options_write(uint8_t* out, uint16_t mss, uint8_t wscale)
{
	out[0] = TCP_OPT_MSS;
	out[1] = 4;
	out[2] = (uint8_t)(mss >> 8);
	out[3] = (uint8_t)mss;
	out[4] = TCP_OPT_NOP;
	out[5] = TCP_OPT_WSCALE;
	out[6] = 3;
	out[7] = wscale;
	return TCP_SYN_OPTIONS_LEN;
}

uint16_t tcp_window_field(uint32_t window, uint8_t shift)
{
	uint32_t field = window >> shift;
	return (uint16_t)(field > 0xFFFF ? 0xFFFF : field);
}

uint32_t tcp_window_scaled(uint16_t field, uint8_t shift)
{
	return (uint32_t)field << shift;
}
