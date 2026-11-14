// test_sbc_host.c — the SBC encoder's proof on the host: kernel/ports/sbc
// (Android's encoder, unpatched, and the boundary in port/sbc_port.c) and
// kernel/src/audio/sbc_frame.c, built by tools/test_sbc_host.sh with the
// port's own switches. What it checks is docs/design/pending/AUDIO_A2DP.md
// § Part E § Proof; kernel/ports/sbc/README.md records what it found.
//
// Every frame the encoder makes is read back by a parser written here from
// the specification's frame layout (the header, the join bits, the scale
// factors, the CRC), which shares nothing with either encoder.
//
//   test_sbc selftest                 settings, side information, random
//                                     frames, restarts, the wrong-length
//                                     tripwire
//   test_sbc guard                    (the build whose frame room is too
//                                     small) the guard must fail the encode
//   test_sbc ceiling                  (the build whose bit-rate search is
//                                     capped) begin must refuse what it
//                                     cannot settle on
//   test_sbc speed                    (the unsanitized build) a frame's cost
//   test_sbc signal NAME RATE OUT     write a test signal, s16le stereo
//   test_sbc encode RATE MODE ALLOC BITPOOL IN OUT
//                                     our stream; prints its frame count
//   test_sbc headers RATE MODE ALLOC BITPOOL FILE
//                                     every frame of FILE says exactly these
//                                     settings and its CRC holds; prints the
//                                     frame count
//   test_sbc compare NAME IN OURS FFMPEG FRAMES
//                                     both decodes against the input
//   test_sbc flip IN OUT BYTE         a copy with one bit of BYTE flipped
//
// MODE is joint or stereo, ALLOC loudness or snr.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio/sbc_frame.h"
#include "sbc_port.h"

#ifdef SBC_PORT_TEST_HOOKS
#include "sbc_switches.h"
#include "sbc_encoder.h"
SBC_ENC_PARAMS *sbc_port_test_params(void);
const uint8_t *sbc_port_test_frame(void);
#endif

#define CHECK(cond, ...) do { if (!(cond)) { \
	fprintf(stderr, "FAIL %s:%d: %s: ", __func__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)

// ── The frame, read from the specification ──────────────────────────────────
//
// sync 0x9C; sampling frequency (2 bits: 16, 32, 44.1, 48 kHz), blocks (2:
// 4, 8, 12, 16), channel mode (2: mono, dual, stereo, joint), allocation (1:
// loudness, SNR), subbands (1: 4, 8); bitpool; CRC; then for joint stereo
// one join bit a subband (the last reserved, 0); then 4-bit scale factors,
// channel by channel; then the samples. The CRC is x^8 + x^4 + x^3 + x^2 + 1
// from 0x0F over the header's second and third bytes, the join bits and the
// scale factors, most significant bit first.

enum { MODE_MONO, MODE_DUAL, MODE_STEREO, MODE_JOINT };

typedef struct frame_info
{
	uint32_t rate;
	uint8_t  blocks, mode, subbands, bitpool;
	bool     snr;
	uint8_t  join[8];
	uint8_t  scf[2][8];
	uint32_t length;
	bool     crc_ok;
} frame_info_t;

static uint32_t bits_at(const uint8_t *p, uint32_t bit, uint32_t count)
{
	uint32_t v = 0;
	for (uint32_t i = 0; i < count; i++)
		v = (v << 1) | ((p[(bit + i) / 8] >> (7 - (bit + i) % 8)) & 1u);
	return v;
}

static uint8_t crc_bits(uint8_t crc, const uint8_t *p, uint32_t from, uint32_t to)
{
	for (uint32_t bit = from; bit < to; bit++)
	{
		uint32_t in = bits_at(p, bit, 1);
		uint32_t top = (crc >> 7) & 1u;
		crc = (uint8_t)(crc << 1);
		if (top ^ in)
			crc ^= 0x1D;
	}
	return crc;
}

// False when p does not hold a whole frame.
static bool parse_frame(const uint8_t *p, size_t avail, frame_info_t *f)
{
	static const uint32_t rates[4] = { 16000, 32000, 44100, 48000 };
	memset(f, 0, sizeof(*f));
	if (avail < 4 || p[0] != 0x9C)
		return false;
	f->rate = rates[p[1] >> 6];
	f->blocks = (uint8_t)(4 * ((p[1] >> 4) & 3) + 4);
	f->mode = (p[1] >> 2) & 3;
	f->snr = (p[1] >> 1) & 1;
	f->subbands = (p[1] & 1) ? 8 : 4;
	f->bitpool = p[2];
	uint32_t channels = f->mode == MODE_MONO ? 1 : 2;
	uint32_t join_bits = f->mode == MODE_JOINT ? f->subbands : 0;
	uint32_t side_end = 32 + join_bits + 4 * f->subbands * channels;
	uint32_t sample_bits = f->blocks * f->bitpool * ((f->mode == MODE_MONO || f->mode == MODE_DUAL) ? channels : 1);
	f->length = (side_end + sample_bits + 7) / 8;
	if (avail < f->length)
		return false;
	for (uint32_t sb = 0; sb < join_bits; sb++)
		f->join[sb] = (uint8_t)bits_at(p, 32 + sb, 1);
	for (uint32_t ch = 0; ch < channels; ch++)
		for (uint32_t sb = 0; sb < f->subbands; sb++)
			f->scf[ch][sb] = (uint8_t)bits_at(p, 32 + join_bits + 4 * (ch * f->subbands + sb), 4);
	uint8_t crc = crc_bits(0x0F, p, 8, 24);
	crc = crc_bits(crc, p, 32, side_end);
	f->crc_ok = crc == p[3];
	return true;
}

static void expect_header(const frame_info_t *f, const sbc_settings_t *s, const char *what)
{
	CHECK(f->rate == s->rate && f->blocks == s->blocks && f->subbands == s->subbands &&
	      f->mode == (s->joint ? MODE_JOINT : MODE_STEREO) && f->snr == s->snr &&
	      f->bitpool == s->bitpool,
	      "%s: header says %u Hz, %u blocks, mode %u, %s, %u subbands, bitpool %u; asked %u Hz %s %s bitpool %u",
	      what, f->rate, f->blocks, f->mode, f->snr ? "snr" : "loudness", f->subbands, f->bitpool,
	      s->rate, s->joint ? "joint" : "stereo", s->snr ? "snr" : "loudness", s->bitpool);
	CHECK(f->crc_ok, "%s: the CRC does not hold", what);
	if (s->joint)
		CHECK(f->join[s->subbands - 1] == 0, "%s: the last join bit is reserved and must be 0", what);
}

// ── Signals ──────────────────────────────────────────────────────────────────

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
	s_rng ^= s_rng >> 12;
	s_rng ^= s_rng << 25;
	s_rng ^= s_rng >> 27;
	return (uint32_t)((s_rng * 0x2545F4914F6CDD1Dull) >> 32);
}
static int16_t rnd_sample(int32_t amplitude)
{
	return (int16_t)((int32_t)(rnd() % (uint32_t)(2 * amplitude + 1)) - amplitude);
}
static int16_t clamp16(double v)
{
	if (v > 32767.0) return 32767;
	if (v < -32768.0) return -32768;
	return (int16_t)lrint(v);
}

// The signals the FFmpeg comparison uses, a whole number of frames long.
#define SIGNAL_FRAMES(rate) ((uint32_t)(((rate) * 3u) / 128u) * 128u)

static int16_t *make_signal(const char *name, uint32_t rate, uint32_t *frames_out)
{
	uint32_t n = SIGNAL_FRAMES(rate);
	int16_t *pcm = calloc((size_t)n * 2, sizeof(int16_t));
	CHECK(pcm != NULL, "out of memory");
	double two_pi = 2.0 * 3.14159265358979323846;
	if (strcmp(name, "sweep") == 0)
	{
		// Logarithmic, 30 Hz to 18 kHz; the right channel at another level
		// and phase so that the two are not one channel twice.
		double f0 = 30.0, f1 = 18000.0, T = (double)n / rate;
		double k = log(f1 / f0);
		for (uint32_t i = 0; i < n; i++)
		{
			double t = (double)i / rate;
			double phase = two_pi * f0 * T / k * (exp(t / T * k) - 1.0);
			pcm[2 * i] = clamp16(16000.0 * sin(phase));
			pcm[2 * i + 1] = clamp16(11000.0 * sin(phase + 1.0));
		}
	}
	else if (strcmp(name, "noise") == 0)
	{
		for (uint32_t i = 0; i < 2 * n; i++)
			pcm[i] = rnd_sample(16000);
	}
	else if (strcmp(name, "square") == 0)
	{
		// Full scale, at frequencies that are not a divisor of the frame.
		for (uint32_t i = 0; i < n; i++)
		{
			pcm[2 * i] = (fmod((double)i * 441.0 / rate, 1.0) < 0.5) ? 32767 : -32768;
			pcm[2 * i + 1] = (fmod((double)i * 1013.0 / rate, 1.0) < 0.5) ? 32767 : -32768;
		}
	}
	else if (strcmp(name, "music") == 0)
	{
		// Notes every 0.375 s, each eight harmonics at 1/k decaying over
		// half a second, the two channels playing different lines.
		static const double notes_l[] = { 220.0, 261.63, 329.63, 392.0, 440.0, 392.0, 329.63, 293.66 };
		static const double notes_r[] = { 110.0, 146.83, 164.81, 130.81 };
		uint32_t step = rate * 3 / 8;
		for (uint32_t i = 0; i < n; i++)
		{
			uint32_t note = i / step;
			double t = (double)(i % step) / rate;
			double env = exp(-t * 6.0);
			double fl = notes_l[note % 8], fr = notes_r[note % 4];
			double l = 0, r = 0;
			for (int h = 1; h <= 8; h++)
			{
				if (fl * h < rate / 2.0)
					l += sin(two_pi * fl * h * t) / h;
				if (fr * h < rate / 2.0)
					r += sin(two_pi * fr * h * t + 0.3 * h) / h;
			}
			pcm[2 * i] = clamp16(9000.0 * env * l);
			pcm[2 * i + 1] = clamp16(9000.0 * env * r);
		}
	}
	else if (strcmp(name, "silence") != 0)
	{
		CHECK(false, "no signal called %s", name);
	}
	*frames_out = n;
	return pcm;
}

// ── Files ────────────────────────────────────────────────────────────────────

static uint8_t *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	CHECK(f != NULL, "cannot open %s", path);
	CHECK(fseek(f, 0, SEEK_END) == 0, "%s", path);
	long size = ftell(f);
	CHECK(size >= 0 && fseek(f, 0, SEEK_SET) == 0, "%s", path);
	uint8_t *buf = malloc((size_t)size + 1);
	CHECK(buf != NULL, "out of memory");
	CHECK(fread(buf, 1, (size_t)size, f) == (size_t)size, "short read of %s", path);
	fclose(f);
	*len = (size_t)size;
	return buf;
}

static void write_file(const char *path, const void *data, size_t len)
{
	FILE *f = fopen(path, "wb");
	CHECK(f != NULL, "cannot create %s", path);
	CHECK(fwrite(data, 1, len, f) == len && fclose(f) == 0, "short write of %s", path);
}

static sbc_settings_t settings_from(const char *rate, const char *mode, const char *alloc, const char *bitpool)
{
	sbc_settings_t s = { .rate = (uint32_t)atoi(rate), .channels = 2, .blocks = 16, .subbands = 8,
	                     .bitpool = (uint8_t)atoi(bitpool) };
	CHECK(strcmp(mode, "joint") == 0 || strcmp(mode, "stereo") == 0, "mode %s", mode);
	CHECK(strcmp(alloc, "loudness") == 0 || strcmp(alloc, "snr") == 0, "allocation %s", alloc);
	s.joint = strcmp(mode, "joint") == 0;
	s.snr = strcmp(alloc, "snr") == 0;
	CHECK(sbc_settings_valid(&s), "settings %s %s %s %s are not ones the boundary takes", rate, mode, alloc, bitpool);
	return s;
}

// ── 1. Every setting the boundary accepts, and every one it refuses ─────────

static void encode_checked(const sbc_settings_t *s, const int16_t *pcm, uint8_t *out, size_t room, const char *what)
{
	uint32_t bytes = sbc_frame_bytes(s);
	int16_t copy[2 * 128];
	memcpy(copy, pcm, sizeof(copy));
	memset(out, 0x5A, room);
	CHECK(sbc_port_encode(pcm, out, room), "%s: the encode was refused", what);
	CHECK(memcmp(copy, pcm, sizeof(copy)) == 0, "%s: the encoder wrote to the PCM it was given", what);
	for (size_t i = bytes; i < room; i++)
		CHECK(out[i] == 0x5A, "%s: byte %zu past the %u-byte frame was written", what, i, bytes);
#ifdef SBC_PORT_TEST_HOOKS
	CHECK(memcmp(out, sbc_port_test_frame(), bytes) == 0, "%s: the frame copied out is not the one made", what);
#endif
	frame_info_t f;
	CHECK(parse_frame(out, bytes, &f), "%s: not a frame of %u bytes", what, bytes);
	CHECK(f.length == bytes, "%s: the header gives %u bytes, sbc_frame_bytes %u", what, f.length, bytes);
	expect_header(&f, s, what);
}

static void every_setting(void)
{
	static const uint32_t rates[] = { 44100, 48000 };
	uint8_t out[1024];
	int16_t pcm[2 * 128];
	uint32_t accepted = 0, longest = 0;
	for (uint32_t r = 0; r < 2; r++)
		for (int joint = 0; joint < 2; joint++)
			for (int snr = 0; snr < 2; snr++)
				for (uint32_t bp = SBC_BITPOOL_MIN; bp <= SBC_BITPOOL_MAX; bp++)
				{
					sbc_settings_t s = { rates[r], 2, joint, 16, 8, snr, (uint8_t)bp };
					char what[96];
					snprintf(what, sizeof(what), "%u Hz %s %s bitpool %u", s.rate,
					         joint ? "joint" : "stereo", snr ? "snr" : "loudness", bp);
					CHECK(sbc_settings_valid(&s), "%s", what);
					CHECK(sbc_port_begin(&s), "%s: begin did not settle on the bitpool", what);
#ifdef SBC_PORT_TEST_HOOKS
					CHECK(sbc_port_test_params()->s16BitPool == (int16_t)bp, "%s: the encoder holds bitpool %d",
					      what, sbc_port_test_params()->s16BitPool);
#endif
					CHECK(sbc_frame_samples(&s) == 128, "%s", what);
					uint32_t bytes = sbc_frame_bytes(&s);
					if (bytes > longest)
						longest = bytes;
					// Three frames: loud noise, silence, loud noise again.
					for (int k = 0; k < 3; k++)
					{
						for (int i = 0; i < 2 * 128; i++)
							pcm[i] = k == 1 ? 0 : rnd_sample(32767);
						encode_checked(&s, pcm, out, sizeof(out), what);
					}
					accepted++;
				}
	CHECK(accepted == 2 * 2 * 2 * (SBC_BITPOOL_MAX - SBC_BITPOOL_MIN + 1), "%u", accepted);
	CHECK(longest == SBC_FRAME_BYTES_MAX, "the longest frame is %u, SBC_FRAME_BYTES_MAX %u", longest, SBC_FRAME_BYTES_MAX);

	// The design's numbers: 48 kHz joint stereo, bitpool 51.
	sbc_settings_t hq = { 48000, 2, true, 16, 8, false, 51 };
	CHECK(sbc_frame_bytes(&hq) == 115, "%u", sbc_frame_bytes(&hq));
	CHECK(sbc_bit_rate(&hq) == 345000, "%u", sbc_bit_rate(&hq));
	sbc_settings_t hq44 = { 44100, 2, true, 16, 8, false, 53 };
	CHECK(sbc_frame_bytes(&hq44) == 119, "%u", sbc_frame_bytes(&hq44));
	CHECK(sbc_bit_rate(&hq44) == 327993, "%u", sbc_bit_rate(&hq44));   // 8 x 119 x 44100 / 128
	sbc_settings_t plain = { 48000, 2, false, 16, 8, false, 51 };
	CHECK(sbc_frame_bytes(&plain) == 114, "%u", sbc_frame_bytes(&plain));

	// Refused: each field off by itself, and nothing.
	sbc_settings_t bad[] = {
		{ 32000, 2, true, 16, 8, false, 51 }, { 16000, 2, true, 16, 8, false, 51 },
		{ 0, 2, true, 16, 8, false, 51 },     { 48001, 2, true, 16, 8, false, 51 },
		{ 44099, 2, true, 16, 8, false, 51 }, { 48000, 1, true, 16, 8, false, 51 },
		{ 48000, 0, true, 16, 8, false, 51 }, { 48000, 3, true, 16, 8, false, 51 },
		{ 48000, 2, true, 4, 8, false, 51 },  { 48000, 2, true, 8, 8, false, 51 },
		{ 48000, 2, true, 12, 8, false, 51 }, { 48000, 2, true, 15, 8, false, 51 },
		{ 48000, 2, true, 0, 8, false, 51 },  { 48000, 2, true, 16, 4, false, 51 },
		{ 48000, 2, true, 16, 0, false, 51 }, { 48000, 2, true, 16, 7, false, 51 },
		{ 48000, 2, true, 16, 8, false, 0 },  { 48000, 2, true, 16, 8, false, 1 },
		{ 48000, 2, true, 16, 8, false, 251 },{ 48000, 2, true, 16, 8, false, 255 },
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
	{
		CHECK(!sbc_settings_valid(&bad[i]), "invalid setting %zu accepted", i);
		CHECK(sbc_frame_bytes(&bad[i]) == 0 && sbc_frame_samples(&bad[i]) == 0 && sbc_bit_rate(&bad[i]) == 0,
		      "invalid setting %zu has a frame", i);
		CHECK(!sbc_port_begin(&bad[i]), "invalid setting %zu begun", i);
		CHECK(!sbc_port_encode(pcm, out, sizeof(out)), "an encode after a refused begin (%zu)", i);
	}
	CHECK(!sbc_settings_valid(NULL) && sbc_frame_bytes(NULL) == 0 && !sbc_port_begin(NULL), "NULL settings");
	printf("PASS every setting: %u accepted (2 rates x 2 modes x 2 allocations x bitpools %u-%u), "
	       "each settled on its bitpool, %u refused; longest frame %u bytes\n",
	       accepted, SBC_BITPOOL_MIN, SBC_BITPOOL_MAX, (unsigned)(sizeof(bad) / sizeof(bad[0])), longest);
}

// ── 3. Side information on known inputs ─────────────────────────────────────
//
// What the frame's meaning implies. In a JOINED subband the first channel
// carries (L + R) / 2 and the second (L - R) / 2 (the specification's joint
// stereo), so:
//   identical channels: a joined subband's second scale factor is 0, since
//     L - R is exactly 0; in plain stereo the two scale factors are equal;
//   opposite channels:  a joined subband's FIRST scale factor is 0;
//   right silent:       an unjoined subband's right scale factor is 0, and a
//     joined one's two scale factors are equal (L + 0 = L - 0).
// What the encoder's decision implies (Android joins a subband when the
// joined scale factors sum to less than the separate ones, as libsbc does):
//   identical channels: every subband but the last with a nonzero scale
//     factor is joined (separately, both channels would pay for it);
//   opposite channels:  an unjoined subband has a zero scale factor in one
//     channel.
// The last subband is never joined: its join bit is reserved.

enum { SIDE_IDENTICAL, SIDE_OPPOSITE, SIDE_RIGHT_SILENT };
static const char *const k_side_names[] = { "identical", "opposite", "right silent" };

static void side_information(void)
{
	uint32_t n;
	int16_t *music = make_signal("music", 48000, &n);
	uint8_t out[1024];
	int16_t pcm[2 * 128];
	for (int kind = 0; kind < 3; kind++)
		for (int joint = 0; joint < 2; joint++)
			for (int snr = 0; snr < 2; snr++)
			{
				sbc_settings_t s = { 48000, 2, joint, 16, 8, snr, 51 };
				char what[96];
				snprintf(what, sizeof(what), "%s, %s, %s", k_side_names[kind], joint ? "joint" : "stereo",
				         snr ? "snr" : "loudness");
				CHECK(sbc_port_begin(&s), "%s", what);
				uint32_t joined = 0, active = 0;
				for (uint32_t frame = 0; frame + 1 < n / 128 && frame < 400; frame++)
				{
					for (int i = 0; i < 128; i++)
					{
						int16_t l = music[2 * (frame * 128 + i)];
						if (l == -32768)
							l = -32767;
						pcm[2 * i] = l;
						pcm[2 * i + 1] = kind == SIDE_IDENTICAL ? l : kind == SIDE_OPPOSITE ? (int16_t)-l : 0;
					}
					encode_checked(&s, pcm, out, sizeof(out), what);
					frame_info_t f;
					CHECK(parse_frame(out, sizeof(out), &f), "%s", what);
					for (int sb = 0; sb < 8; sb++)
					{
						uint8_t a = f.scf[0][sb], b = f.scf[1][sb];
						bool j = joint && f.join[sb];
						joined += j;
						active += a > 0;
						if (kind == SIDE_IDENTICAL)
						{
							if (j)
								CHECK(b == 0, "%s: frame %u subband %d joined, side scale factor %u", what, frame, sb, b);
							else
								CHECK(a == b, "%s: frame %u subband %d scale factors %u %u", what, frame, sb, a, b);
							if (joint && sb < 7)
								CHECK(j == (a > 0), "%s: frame %u subband %d (scale factor %u) %s joined", what,
								      frame, sb, a, j ? "is" : "is not");
						}
						else if (kind == SIDE_OPPOSITE)
						{
							if (j)
								CHECK(a == 0, "%s: frame %u subband %d joined, mid scale factor %u", what, frame, sb, a);
							else if (joint && sb < 7)
								CHECK(a == 0 || b == 0, "%s: frame %u subband %d unjoined at %u %u", what, frame, sb, a, b);
							else
								CHECK(abs((int)a - (int)b) <= 1, "%s: frame %u subband %d scale factors %u %u",
								      what, frame, sb, a, b);
						}
						else
						{
							if (j)
								CHECK(a == b, "%s: frame %u subband %d joined at %u %u", what, frame, sb, a, b);
							else
								CHECK(b == 0, "%s: frame %u subband %d silent channel's scale factor %u", what, frame, sb, b);
						}
					}
				}
				CHECK(active > 0, "%s: the signal never reached a scale factor", what);
				if (joint && kind != SIDE_RIGHT_SILENT)
					CHECK(joined > 0, "%s: nothing was ever joined", what);
				if (!joint)
					CHECK(joined == 0, "%s", what);
				printf("PASS side information, %s: %u subbands joined\n", what, joined);
			}
	free(music);
}

// ── 4. Random PCM, random settings, under the sanitizers ────────────────────

static void random_frames(void)
{
	uint8_t out[1024];
	int16_t pcm[2 * 128];
	uint32_t frames = 0;
	for (int round = 0; round < 400; round++)
	{
		sbc_settings_t s = { (rnd() & 1) ? 48000 : 44100, 2, rnd() & 1, 16, 8, rnd() & 1,
		                     (uint8_t)(SBC_BITPOOL_MIN + rnd() % (SBC_BITPOOL_MAX - SBC_BITPOOL_MIN + 1)) };
		char what[96];
		snprintf(what, sizeof(what), "random round %d (%u Hz %s %s bitpool %u)", round, s.rate,
		         s.joint ? "joint" : "stereo", s.snr ? "snr" : "loudness", s.bitpool);
		CHECK(sbc_port_begin(&s), "%s", what);
		for (int k = 0; k < 12; k++)
		{
			uint32_t kind = rnd() % 7;
			int32_t amp = (int32_t)(1u << (rnd() % 16));
			for (int i = 0; i < 2 * 128; i++)
			{
				switch (kind)
				{
				case 0: pcm[i] = rnd_sample(32767); break;                       // full-scale noise
				case 1: pcm[i] = rnd_sample(amp); break;                          // noise at a random level
				case 2: pcm[i] = (i & 1) ? -32768 : 32767; break;                 // the extremes, L against R
				case 3: pcm[i] = ((i / 2) & 1) ? -32768 : 32767; break;           // Nyquist at full scale
				case 4: pcm[i] = (rnd() % 64 == 0) ? (int16_t)rnd() : 0; break;   // impulses
				case 5: pcm[i] = (int16_t)(rnd() & 1 ? -32768 : 32767); break;    // random extremes
				default: pcm[i] = (int16_t)rnd(); break;                          // any value
				}
			}
			encode_checked(&s, pcm, out, sizeof(out), what);
			frames++;
		}
	}
	printf("PASS random: %u frames over 400 random settings\n", frames);
}

// ── A second begin restarts the encoder ─────────────────────────────────────
//
// Upstream's filter history is static, so what a begin promises is that the
// frames after it depend on nothing encoded before it. The same PCM after a
// begin must give the same bytes whether the encoder last ran other
// settings or these very settings.

#define RESTART_FRAMES 24

static void encode_run(const sbc_settings_t *s, const int16_t *pcm, uint32_t frames, uint8_t *out)
{
	uint32_t bytes = sbc_frame_bytes(s);
	CHECK(sbc_port_begin(s), "begin");
	for (uint32_t f = 0; f < frames; f++)
		CHECK(sbc_port_encode(pcm + (size_t)f * 256, out + (size_t)f * bytes, bytes), "frame %u", f);
}

static void restart(void)
{
	static const uint32_t rates[] = { 44100, 48000 };
	uint32_t n;
	int16_t *music = make_signal("music", 48000, &n);
	int16_t junk[RESTART_FRAMES * 256];
	for (size_t i = 0; i < sizeof(junk) / sizeof(junk[0]); i++)
		junk[i] = (int16_t)rnd();
	static uint8_t first[RESTART_FRAMES * SBC_FRAME_BYTES_MAX], again[RESTART_FRAMES * SBC_FRAME_BYTES_MAX];
	static uint8_t scratch[RESTART_FRAMES * SBC_FRAME_BYTES_MAX];
	for (int r = 0; r < 2; r++)
		for (int joint = 0; joint < 2; joint++)
		{
			sbc_settings_t b = { rates[r], 2, joint, 16, 8, false, 51 };
			sbc_settings_t other = { rates[1 - r], 2, !joint, 16, 8, true, 33 };
			size_t len = (size_t)RESTART_FRAMES * sbc_frame_bytes(&b);
			// From the middle of the music, so that whatever history a
			// begin failed to clear would differ from what it should be.
			const int16_t *x = music + (size_t)50 * 256;

			encode_run(&b, x, RESTART_FRAMES, first);
			encode_run(&other, junk, RESTART_FRAMES, scratch);
			encode_run(&b, x, RESTART_FRAMES, again);
			CHECK(memcmp(first, again, len) == 0, "%u Hz %s: a begin after other settings remembered them",
			      b.rate, joint ? "joint" : "stereo");

			encode_run(&b, junk, RESTART_FRAMES, scratch);
			encode_run(&b, x, RESTART_FRAMES, again);
			CHECK(memcmp(first, again, len) == 0, "%u Hz %s: a begin with the same settings kept the history",
			      b.rate, joint ? "joint" : "stereo");
		}
	free(music);
	printf("PASS restart: after a begin, the same PCM gives the same bytes, whatever ran before "
	       "(other settings or the same; both rates, joint and plain stereo)\n");
}

// ── 6. The tripwires ─────────────────────────────────────────────────────────

static void wrong_length(void)
{
#ifdef SBC_PORT_TEST_HOOKS
	uint8_t out[1024];
	int16_t pcm[2 * 128] = { 0 };
	sbc_settings_t s = { 48000, 2, true, 16, 8, false, 51 };
	CHECK(sbc_port_begin(&s), "begin");
	CHECK(sbc_port_encode(pcm, out, sizeof(out)), "a good frame");
	CHECK(sbc_port_encode(pcm, out, 115), "a capacity of exactly the frame was refused");
	CHECK(!sbc_port_encode(pcm, out, 114), "a capacity one short of the frame was accepted");
	CHECK(!sbc_port_encode(pcm, out, sizeof(out)), "the boundary was usable again after refusing a capacity");
	CHECK(sbc_port_begin(&s), "begin after the capacity refusal");

	// The encoder's bitpool moved under the boundary: its frame is two bytes
	// longer than the settings give.
	memset(out, 0x5A, sizeof(out));
	sbc_port_test_params()->s16BitPool = 52;
	CHECK(!sbc_port_encode(pcm, out, sizeof(out)), "a frame of the wrong length was accepted");
	CHECK(out[0] == 0x5A, "a refused frame was copied out");
	sbc_port_test_params()->s16BitPool = 51;
	CHECK(!sbc_port_encode(pcm, out, sizeof(out)), "the boundary was usable again before a begin");
	CHECK(sbc_port_begin(&s), "begin after the refusal");
	CHECK(sbc_port_encode(pcm, out, sizeof(out)), "a good frame after begin");
	CHECK(!sbc_port_encode(NULL, out, sizeof(out)) && !sbc_port_encode(pcm, out, sizeof(out)), "NULL PCM");
	printf("PASS tripwires: a short capacity and a frame of the wrong length each fail the boundary until begin\n");
#endif
}

static int guard(void)
{
	// This build's frame room is 100 bytes: a 115-byte frame runs 15 bytes
	// into the guard, and the length is right, so only the guard can see it.
	uint8_t out[1024];
	int16_t pcm[2 * 128];
	for (int i = 0; i < 2 * 128; i++)
		pcm[i] = rnd_sample(20000);
	sbc_settings_t big = { 48000, 2, true, 16, 8, false, 51 };
	sbc_settings_t small = { 48000, 2, true, 16, 8, false, 40 };   // 93 bytes: fits
	CHECK(sbc_frame_bytes(&big) == 115 && sbc_frame_bytes(&small) == 93, "frame lengths %u %u",
	      sbc_frame_bytes(&big), sbc_frame_bytes(&small));
	CHECK(sbc_port_begin(&small) && sbc_port_encode(pcm, out, sizeof(out)), "a frame that fits the room");
	CHECK(sbc_port_begin(&big), "begin");
	memset(out, 0x5A, sizeof(out));
	CHECK(!sbc_port_encode(pcm, out, sizeof(out)), "a frame over the guard was accepted");
	CHECK(out[0] == 0x5A, "a refused frame was copied out");
	CHECK(!sbc_port_encode(pcm, out, sizeof(out)), "the boundary was usable again before a begin");
	CHECK(sbc_port_begin(&small) && sbc_port_encode(pcm, out, sizeof(out)), "begin restores a fitting encoder");
	printf("PASS guard: a frame written over the guard bytes fails the boundary until begin\n");
	return 0;
}

static int ceiling(void)
{
	// This build's bit-rate search stops at 200 kbit/s. Bitpool 51 at 48
	// kHz joint stereo needs 345, so the encoder cannot settle on it, and
	// begin must say so rather than run at the bitpool it reached. Bitpool
	// 20 needs 159.
	uint8_t out[1024];
	int16_t pcm[2 * 128] = { 0 };
	sbc_settings_t high = { 48000, 2, true, 16, 8, false, 51 };
	sbc_settings_t low = { 48000, 2, true, 16, 8, false, 20 };
	CHECK(!sbc_port_begin(&high), "begin accepted a bitpool the encoder did not settle on");
	CHECK(!sbc_port_encode(pcm, out, sizeof(out)), "an encode after the refusal");
	CHECK(sbc_port_begin(&low) && sbc_port_encode(pcm, out, sizeof(out)), "a bitpool under the ceiling");
	printf("PASS ceiling: begin refuses a bitpool the encoder did not settle on\n");
	return 0;
}

// ── 5. Speed ─────────────────────────────────────────────────────────────────

#define SPEED_BOUND_US 200.0

static int speed(void)
{
	sbc_settings_t s = { 48000, 2, true, 16, 8, false, 51 };
	CHECK(sbc_port_begin(&s), "begin");
	enum { FRAMES = 20000 };
	static int16_t pcm[16][2 * 128];
	for (int b = 0; b < 16; b++)
		for (int i = 0; i < 2 * 128; i++)
			pcm[b][i] = rnd_sample(20000);
	uint8_t out[1024];
	double best = 1e9;
	for (int rep = 0; rep < 5; rep++)
	{
		struct timespec a, z;
		clock_gettime(CLOCK_MONOTONIC, &a);
		for (int i = 0; i < FRAMES; i++)
			CHECK(sbc_port_encode(pcm[i & 15], out, sizeof(out)), "frame %d", i);
		clock_gettime(CLOCK_MONOTONIC, &z);
		double us = ((double)(z.tv_sec - a.tv_sec) * 1e9 + (double)(z.tv_nsec - a.tv_nsec)) / 1e3 / FRAMES;
		if (us < best)
			best = us;
	}
	// A frame is 128 samples: 2667 us of sound at 48 kHz.
	printf("PASS speed: %.2f us a frame (48 kHz joint stereo, bitpool 51; best of 5 x %d), "
	       "%.2f%% of one core in real time; bound %.0f us\n", best, FRAMES, 100.0 * best / (128e6 / 48000), SPEED_BOUND_US);
	CHECK(best < SPEED_BOUND_US, "a frame costs %.2f us", best);
	return 0;
}

// ── 2. Against FFmpeg ────────────────────────────────────────────────────────
//
// The delay through an encoder and FFmpeg's decoder, in samples: found by
// cross-correlation on the sweep, the noise and the music (their peaks are
// unambiguous), and asserted the same at every setting and both rates, as
// it must be: it is the two filter banks', and they depend on the number of
// subbands alone, eight at every setting here. 73 for both encoders, a
// whole number of samples: on white noise at bitpool 250 the correlation's
// neighbours at 72 and 74 are -0.0006 of the peak. The square waves
// (periodic, so their peak is not unique) use the constant.
#define DELAY_OURS   73
#define DELAY_FFMPEG 73
// Lags searched: over three times the delay, so that the peak found is a
// maximum and not the edge of the search.
#define DELAY_SEARCH 256
// The start-up transient left out of the measure.
#define SKIP_FRAMES  2048
// Silence decodes to within this many least significant bits of zero.
#define SILENCE_LSB  4

static int32_t find_delay(const int16_t *in, const int16_t *out, uint32_t n)
{
	// Both channels over everything after the transient: at the smallest
	// bitpools the decoded signal is mostly error, and a short window lets
	// a neighbouring lag win.
	uint32_t from = SKIP_FRAMES, to = n - DELAY_SEARCH;
	CHECK(from < to, "signal too short");
	double best = -1e300;
	int32_t at = -1;
	for (int32_t d = 0; d < DELAY_SEARCH; d++)
	{
		double score = 0;
		for (uint32_t t = from; t < to; t++)
			for (int ch = 0; ch < 2; ch++)
				score += (double)in[2 * t + ch] * out[2 * (t + (uint32_t)d) + ch];
		if (score > best)
		{
			best = score;
			at = d;
		}
	}
	return at;
}

static double snr_db(const int16_t *in, const int16_t *out, uint32_t n, int32_t delay)
{
	double signal = 0, error = 0;
	for (uint32_t t = SKIP_FRAMES; t + (uint32_t)delay < n; t++)
		for (int ch = 0; ch < 2; ch++)
		{
			double x = in[2 * t + ch], y = out[2 * (t + (uint32_t)delay) + ch];
			signal += x * x;
			error += (y - x) * (y - x);
		}
	if (error == 0)
		return 200.0;
	return 10.0 * log10(signal / error);
}

static int compare(const char *name, const char *in_path, const char *ours_path, const char *ff_path, uint32_t frames)
{
	size_t in_len, ours_len, ff_len;
	int16_t *in = (int16_t *)read_file(in_path, &in_len);
	int16_t *ours = (int16_t *)read_file(ours_path, &ours_len);
	int16_t *ff = (int16_t *)read_file(ff_path, &ff_len);
	uint32_t n = (uint32_t)(in_len / 4);
	CHECK(in_len == (size_t)frames * 128 * 4, "%s: the input is %zu bytes, not %u frames", name, in_len, frames);
	CHECK(ours_len == in_len, "%s: our stream decoded to %zu bytes, %u frames in should give %zu",
	      name, ours_len, frames, in_len);
	CHECK(ff_len == in_len, "%s: FFmpeg's stream decoded to %zu bytes, %u frames in should give %zu",
	      name, ff_len, frames, in_len);

	if (strcmp(name, "silence") == 0)
	{
		int peak = 0, ff_peak = 0;
		for (uint32_t i = 0; i < 2 * n; i++)
		{
			if (abs(ours[i]) > peak) peak = abs(ours[i]);
			if (abs(ff[i]) > ff_peak) ff_peak = abs(ff[i]);
		}
		printf("     %-8s ours peak %d LSB, FFmpeg's %d LSB (bound %d)\n", name, peak, ff_peak, SILENCE_LSB);
		CHECK(peak <= SILENCE_LSB, "silence decoded to %d LSB", peak);
		free(in);
		free(ours);
		free(ff);
		return 0;
	}

	int32_t d_ours = DELAY_OURS, d_ff = DELAY_FFMPEG;
	if (strcmp(name, "square") != 0)
	{
		int32_t found_ours = find_delay(in, ours, n), found_ff = find_delay(in, ff, n);
		CHECK(found_ours == DELAY_OURS, "%s: our delay is %d samples, not the %d stated", name, found_ours, DELAY_OURS);
		CHECK(found_ff == DELAY_FFMPEG, "%s: FFmpeg's delay is %d samples, not the %d stated", name, found_ff, DELAY_FFMPEG);
	}
	double ours_db = snr_db(in, ours, n, d_ours), ff_db = snr_db(in, ff, n, d_ff);
	printf("     %-8s ours %6.2f dB  FFmpeg %6.2f dB  (%+.2f)\n", name, ours_db, ff_db, ours_db - ff_db);
	CHECK(ours_db >= ff_db - 3.0, "%s: ours is %.2f dB, FFmpeg's %.2f dB: more than 3 dB worse", name, ours_db, ff_db);
	free(in);
	free(ours);
	free(ff);
	return 0;
}

// ── The FFmpeg run's helpers ─────────────────────────────────────────────────

static int encode_file(const sbc_settings_t *s, const char *in_path, const char *out_path)
{
	size_t len;
	int16_t *pcm = (int16_t *)read_file(in_path, &len);
	uint32_t frames = (uint32_t)(len / (128 * 4));
	CHECK(len == (size_t)frames * 128 * 4, "%s is not a whole number of frames", in_path);
	uint32_t bytes = sbc_frame_bytes(s);
	uint8_t *sbc = malloc((size_t)frames * bytes + 1);
	CHECK(sbc != NULL, "out of memory");
	CHECK(sbc_port_begin(s), "begin");
	for (uint32_t f = 0; f < frames; f++)
		CHECK(sbc_port_encode(pcm + (size_t)f * 256, sbc + (size_t)f * bytes, bytes), "frame %u", f);
	write_file(out_path, sbc, (size_t)frames * bytes);
	printf("%u\n", frames);
	free(sbc);
	free(pcm);
	return 0;
}

static int headers(const sbc_settings_t *s, const char *path)
{
	size_t len;
	uint8_t *data = read_file(path, &len);
	size_t at = 0;
	uint32_t frames = 0;
	while (at < len)
	{
		frame_info_t f;
		char what[64];
		snprintf(what, sizeof(what), "%s frame %u", path, frames);
		CHECK(parse_frame(data + at, len - at, &f), "%s: not a frame at byte %zu", what, at);
		expect_header(&f, s, what);
		CHECK(f.length == sbc_frame_bytes(s), "%s: %u bytes", what, f.length);
		at += f.length;
		frames++;
	}
	printf("%u\n", frames);
	free(data);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "selftest") == 0)
	{
		every_setting();
		side_information();
		random_frames();
		restart();
		wrong_length();
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "guard") == 0)
		return guard();
	if (argc >= 2 && strcmp(argv[1], "ceiling") == 0)
		return ceiling();
	if (argc >= 2 && strcmp(argv[1], "speed") == 0)
		return speed();
	if (argc == 5 && strcmp(argv[1], "signal") == 0)
	{
		uint32_t n;
		int16_t *pcm = make_signal(argv[2], (uint32_t)atoi(argv[3]), &n);
		write_file(argv[4], pcm, (size_t)n * 4);
		printf("%u\n", n / 128);
		free(pcm);
		return 0;
	}
	if (argc == 8 && strcmp(argv[1], "encode") == 0)
	{
		sbc_settings_t s = settings_from(argv[2], argv[3], argv[4], argv[5]);
		return encode_file(&s, argv[6], argv[7]);
	}
	if (argc == 7 && strcmp(argv[1], "headers") == 0)
	{
		sbc_settings_t s = settings_from(argv[2], argv[3], argv[4], argv[5]);
		return headers(&s, argv[6]);
	}
	if (argc == 7 && strcmp(argv[1], "compare") == 0)
		return compare(argv[2], argv[3], argv[4], argv[5], (uint32_t)atoi(argv[6]));
	if (argc == 5 && strcmp(argv[1], "flip") == 0)
	{
		size_t len, at = (size_t)atol(argv[4]);
		uint8_t *data = read_file(argv[2], &len);
		CHECK(at < len, "byte %zu of a %zu-byte file", at, len);
		data[at] ^= 0x10;
		write_file(argv[3], data, len);
		free(data);
		return 0;
	}
	fprintf(stderr, "usage: see the head of tools/test_sbc_host.c\n");
	return 2;
}
