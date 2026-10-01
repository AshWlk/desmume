/*
	ds2sf smoke test: loads a 2SF/mini2SF (following its _lib chain), renders it through libds2sf
	and checks the API contract.

	usage: ds2sf_smoketest FILE [--seconds S] [--rate HZ] [--expect-hz F] [--wav OUT.wav]

	Checks (non-zero exit on failure):
	  - only one instance may exist at a time
	  - the output is not silent
	  - with --expect-hz: the fundamental (positive-going mean crossings) is within 0.5% of F
	  - ds2sf_reset + re-render reproduces the first render bit-for-bit
	  - destroy + create again (at a different sample rate) works, and pitch is unchanged
	  - odd-sized render calls (buffer carry-over between calls) give the same audio as one call

	This is a test tool, so it trusts its input: the PSF parsing is minimal.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zlib.h>

#include "../ds2sf.h"

typedef struct
{
	uint8_t *data;
	size_t size;
	int initial_frames; /* _frames tag, -1 if absent */
} rom_image;

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint8_t *read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc(n > 0 ? n : 1);
	if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
	fclose(f);
	*size = (size_t)n;
	return buf;
}

/* Returns a malloc'd copy of the value for `key` (case-insensitive) in a [TAG] block, or NULL. */
static char *find_tag(const char *tags, const char *key)
{
	size_t keylen = strlen(key);
	const char *line = tags;
	while (line && *line)
	{
		const char *eol = strchr(line, '\n');
		size_t len = eol ? (size_t)(eol - line) : strlen(line);
		const char *eq = memchr(line, '=', len);
		if (eq && (size_t)(eq - line) == keylen && strncasecmp(line, key, keylen) == 0)
		{
			size_t vlen = len - keylen - 1;
			char *value = malloc(vlen + 1);
			memcpy(value, eq + 1, vlen);
			value[vlen] = 0;
			while (vlen && (value[vlen - 1] == '\r' || value[vlen - 1] == ' ')) value[--vlen] = 0;
			return value;
		}
		line = eol ? eol + 1 : NULL;
	}
	return NULL;
}

static void map_section(rom_image *rom, const uint8_t *section, size_t size)
{
	if (size < 8) return;
	uint32_t offset = le32(section), length = le32(section + 4);
	if (8 + (size_t)length > size) { fprintf(stderr, "truncated program section\n"); exit(2); }
	if (rom->size < (size_t)offset + length)
	{
		rom->data = realloc(rom->data, (size_t)offset + length);
		memset(rom->data + rom->size, 0, (size_t)offset + length - rom->size);
		rom->size = (size_t)offset + length;
	}
	memcpy(rom->data + offset, section + 8, length);
}

/* Load order: _lib (recursively) -> this file -> _lib2, _lib3, ... (psflib's order). */
static void load_2sf(rom_image *rom, const char *path, int depth)
{
	if (depth > 10) { fprintf(stderr, "_lib chain too deep\n"); exit(2); }

	size_t size;
	uint8_t *file = read_file(path, &size);
	if (!file || size < 16 || memcmp(file, "PSF\x24", 4) != 0) { fprintf(stderr, "%s: not a 2SF\n", path); exit(2); }

	uint32_t reserved = le32(file + 4), zsize = le32(file + 8);
	const uint8_t *zdata = file + 16 + reserved;
	size_t tag_offset = 16 + (size_t)reserved + zsize;
	char *tags = NULL;
	if (size > tag_offset + 5 && memcmp(file + tag_offset, "[TAG]", 5) == 0)
	{
		size_t n = size - tag_offset - 5;
		tags = malloc(n + 1);
		memcpy(tags, file + tag_offset + 5, n);
		tags[n] = 0;
	}

	char dir[4096];
	snprintf(dir, sizeof dir, "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash) slash[1] = 0; else dir[0] = 0;

	char *lib = tags ? find_tag(tags, "_lib") : NULL;
	if (lib)
	{
		char libpath[8192];
		snprintf(libpath, sizeof libpath, "%s%s", dir, lib);
		load_2sf(rom, libpath, depth + 1);
		free(lib);
	}

	if (zsize)
	{
		uLongf usize = 1 << 20;
		uint8_t *udata = NULL;
		int err;
		do {
			usize *= 2;
			udata = realloc(udata, usize);
			uLongf got = usize;
			err = uncompress(udata, &got, zdata, zsize);
			if (err == Z_OK) usize = got;
		} while (err == Z_BUF_ERROR);
		if (err != Z_OK) { fprintf(stderr, "%s: zlib error %d\n", path, err); exit(2); }
		map_section(rom, udata, usize);
		free(udata);
	}

	if (tags && depth == 0)
	{
		char *frames = find_tag(tags, "_frames");
		if (frames) { rom->initial_frames = atoi(frames); free(frames); }
	}

	for (int n = 2; tags; n++)
	{
		char key[16];
		snprintf(key, sizeof key, "_lib%d", n);
		char *libn = find_tag(tags, key);
		if (!libn) break;
		char libpath[8192];
		snprintf(libpath, sizeof libpath, "%s%s", dir, libn);
		load_2sf(rom, libpath, depth + 1);
		free(libn);
	}

	free(tags);
	free(file);
}

static void write_wav(const char *path, const int16_t *pcm, size_t frames, uint32_t rate)
{
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); return; }
	uint32_t data_bytes = (uint32_t)(frames * 4);
	uint8_t h[44];
	memcpy(h, "RIFF", 4); uint32_t riff = 36 + data_bytes; memcpy(h + 4, &riff, 4);
	memcpy(h + 8, "WAVEfmt ", 8);
	uint32_t fmt_size = 16; uint16_t fmt = 1, channels = 2, bits = 16, align = 4; uint32_t bps = rate * 4;
	memcpy(h + 16, &fmt_size, 4); memcpy(h + 20, &fmt, 2); memcpy(h + 22, &channels, 2);
	memcpy(h + 24, &rate, 4); memcpy(h + 28, &bps, 4); memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
	memcpy(h + 36, "data", 4); memcpy(h + 40, &data_bytes, 4);
	fwrite(h, 1, 44, f);
	fwrite(pcm, 4, frames, f);
	fclose(f);
}

typedef struct { double rms_l, rms_r, hz; } analysis;

/* Skips the first 100 ms (boot), then measures RMS and the positive-going crossing rate of the left
   channel around its mean. Good enough for the test ROM's square wave. */
static analysis analyse(const int16_t *pcm, size_t frames, uint32_t rate)
{
	analysis a = {0, 0, 0};
	size_t start = rate / 10;
	if (frames <= start + 2) return a;

	double mean = 0;
	for (size_t i = start; i < frames; i++) mean += pcm[i * 2];
	mean /= (double)(frames - start);

	double sl = 0, sr = 0;
	size_t first = 0, last = 0, crossings = 0;
	for (size_t i = start; i < frames; i++)
	{
		sl += (double)pcm[i * 2] * pcm[i * 2];
		sr += (double)pcm[i * 2 + 1] * pcm[i * 2 + 1];
		if (i > start && pcm[(i - 1) * 2] < mean && pcm[i * 2] >= mean)
		{
			if (crossings == 0) first = i;
			last = i;
			crossings++;
		}
	}
	a.rms_l = sqrt(sl / (double)(frames - start));
	a.rms_r = sqrt(sr / (double)(frames - start));
	if (crossings > 1)
		a.hz = (double)(crossings - 1) * rate / (double)(last - first);
	return a;
}

static const char *render_all(ds2sf_t *ds, int16_t *out, size_t frames, size_t chunk)
{
	for (size_t done = 0; done < frames; )
	{
		size_t n = frames - done < chunk ? frames - done : chunk;
		const char *err = ds2sf_render(ds, out + done * 2, n);
		if (err) return err;
		done += n;
	}
	return NULL;
}

static int boot(ds2sf_t *ds, const rom_image *rom)
{
	const char *err = ds2sf_load_rom(ds, rom->data, rom->size);
	if (err) { printf("FAIL: ds2sf_load_rom: %s\n", err); failures++; return 0; }
	if (rom->initial_frames > 0 && (err = ds2sf_run_frames(ds, (uint32_t)rom->initial_frames)))
	{
		printf("FAIL: ds2sf_run_frames: %s\n", err); failures++; return 0;
	}
	return 1;
}

/* Re-renders with per-channel output in odd-sized chunks: the mix must be unchanged, the channels
   must sum (clamped) to it, and the test ROM's single PSG key-on must be reported. Then keys the
   note on during ds2sf_run_frames, which must re-report it at the first rendered frame. */
static void check_channel_output(ds2sf_t *ds, const rom_image *rom, const int16_t *expected, size_t frames)
{
	const size_t chunk = 777;
	int16_t *mix = malloc(chunk * 4), *channels = malloc(16 * chunk * 4);
	ds2sf_keyon_event *events = malloc(16 * chunk * sizeof *events);
	size_t mismatched_mix = 0, mismatched_sum = 0, keyons = 0, first_keyon = 0;
	int keyon_channel = -1, keyon_format = -1;
	const char *err = NULL;

	err = ds2sf_reset(ds);
	if (!err) err = ds2sf_set_channel_output(ds, 1);
	if (!err && rom->initial_frames > 0) err = ds2sf_run_frames(ds, (uint32_t)rom->initial_frames);
	for (size_t done = 0; !err && done < frames; done += chunk)
	{
		size_t n = frames - done < chunk ? frames - done : chunk, count = 0;
		err = ds2sf_render_channels(ds, mix, channels, n, events, 16 * n, &count);
		if (err) break;
		for (size_t i = 0; i < n * 2; i++)
		{
			int32_t sum = 0;
			for (int c = 0; c < 16; c++) sum += channels[c * n * 2 + i];
			sum = sum < -32768 ? -32768 : sum > 32767 ? 32767 : sum;
			mismatched_mix += mix[i] != expected[done * 2 + i];
			mismatched_sum += sum != mix[i];
		}
		for (size_t i = 0; i < count; i++, keyons++)
		{
			if (keyons == 0) { first_keyon = done + events[i].offset; keyon_channel = events[i].channel; keyon_format = events[i].format; }
		}
	}
	CHECK(err == NULL, "render with channel output in %zu-frame chunks%s%s", chunk, err ? ": " : "", err ? err : "");
	CHECK(mismatched_mix == 0, "mix is unchanged with channel output enabled (%zu samples differ)", mismatched_mix);
	CHECK(mismatched_sum == 0, "channels sum to the mix (%zu samples differ)", mismatched_sum);
	CHECK(keyons == 1 && keyon_channel == 8 && keyon_format == 3,
		"one key-on reported, PSG on channel 8 (%zu key-ons, first: channel %d format %d at frame %zu)",
		keyons, keyon_channel, keyon_format, first_keyon);

	/* The note keys on within the first frames after boot; key it on during a pre-roll instead. */
	size_t count = 0;
	err = ds2sf_reset(ds);
	if (!err) err = ds2sf_run_frames(ds, 30);
	if (!err) err = ds2sf_render_channels(ds, mix, channels, chunk, events, 16 * chunk, &count);
	CHECK(err == NULL && count == 1 && events[0].offset == 0 && events[0].channel == 8,
		"a note keyed on during ds2sf_run_frames is reported at frame 0 (%zu key-ons, first at %u)",
		count, count ? events[0].offset : 0);

	err = ds2sf_set_channel_output(ds, 0);
	CHECK(err == NULL && ds2sf_render_channels(ds, mix, channels, 16, events, 256, &count) != NULL,
		"render_channels is refused once channel output is disabled");

	free(mix);
	free(channels);
	free(events);
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s FILE [--seconds S] [--rate HZ] [--expect-hz F] [--wav OUT.wav]\n", argv[0]);
		return 2;
	}

	const char *path = argv[1], *wav = NULL;
	double seconds = 3.0, expect_hz = 0;
	uint32_t rate = 0;
	for (int i = 2; i + 1 < argc; i += 2)
	{
		if (!strcmp(argv[i], "--seconds")) seconds = atof(argv[i + 1]);
		else if (!strcmp(argv[i], "--rate")) rate = (uint32_t)atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "--expect-hz")) expect_hz = atof(argv[i + 1]);
		else if (!strcmp(argv[i], "--wav")) wav = argv[i + 1];
	}

	rom_image rom = { NULL, 0, -1 };
	load_2sf(&rom, path, 0);
	printf("%s: assembled ROM image of %zu bytes\n", path, rom.size);

	ds2sf_config config = { rate, DS2SF_INTERP_NONE };
	ds2sf_t *ds = ds2sf_create(&config);
	CHECK(ds != NULL, "ds2sf_create");
	if (!ds) return 1;
	CHECK(ds2sf_create(NULL) == NULL, "second concurrent ds2sf_create is refused");

	const uint32_t sr = ds2sf_get_sample_rate(ds);
	const size_t frames = (size_t)(seconds * sr);
	int16_t *a = malloc(frames * 4), *b = malloc(frames * 4);

	const char *err = ds2sf_render(ds, a, 16);
	CHECK(err != NULL, "render before any ROM is loaded is an error (%s)", err ? err : "no error");
	uint8_t junk[0x200];
	memset(junk, 0xFF, sizeof junk); /* unit code 0xFF: not a DS header */
	err = ds2sf_load_rom(ds, junk, sizeof junk);
	CHECK(err != NULL, "junk ROM header is rejected (%s)", err ? err : "no error");

	if (!boot(ds, &rom)) return 1;
	err = render_all(ds, a, frames, frames);
	CHECK(err == NULL, "render %.1f s at %u Hz in one call%s%s", seconds, sr, err ? ": " : "", err ? err : "");

	analysis an = analyse(a, frames, sr);
	printf("      rms L=%.1f R=%.1f, fundamental %.2f Hz\n", an.rms_l, an.rms_r, an.hz);
	CHECK(an.rms_l > 100 && an.rms_r > 100, "output is not silent");
	if (expect_hz > 0)
		CHECK(fabs(an.hz - expect_hz) <= expect_hz * 0.005, "pitch %.2f Hz matches expected %.2f Hz", an.hz, expect_hz);
	if (wav) write_wav(wav, a, frames, sr);

	err = ds2sf_reset(ds);
	CHECK(err == NULL, "ds2sf_reset");
	if (rom.initial_frames > 0) ds2sf_run_frames(ds, (uint32_t)rom.initial_frames);
	err = render_all(ds, b, frames, 777);
	CHECK(err == NULL, "re-render after reset in 777-frame chunks");
	CHECK(memcmp(a, b, frames * 4) == 0, "reset + chunked re-render is bit-identical to the first render");

	check_channel_output(ds, &rom, a, frames);

	ds2sf_destroy(ds);

	ds2sf_config other = { sr == 44100 ? DS2SF_NATIVE_SAMPLE_RATE : 44100, DS2SF_INTERP_NONE };
	ds = ds2sf_create(&other);
	CHECK(ds != NULL, "create again after destroy (at %u Hz)", other.sample_rate);
	if (ds && boot(ds, &rom))
	{
		const uint32_t sr2 = ds2sf_get_sample_rate(ds);
		const size_t frames2 = (size_t)(seconds * sr2);
		int16_t *c = malloc(frames2 * 4);
		err = render_all(ds, c, frames2, 4096);
		CHECK(err == NULL, "render at %u Hz", sr2);
		analysis an2 = analyse(c, frames2, sr2);
		printf("      rms L=%.1f R=%.1f, fundamental %.2f Hz\n", an2.rms_l, an2.rms_r, an2.hz);
		CHECK(an2.rms_l > 100, "output at %u Hz is not silent", sr2);
		if (an.hz > 0)
			CHECK(fabs(an2.hz - an.hz) <= an.hz * 0.005, "pitch is independent of output sample rate (%.2f vs %.2f Hz)", an2.hz, an.hz);
		free(c);
	}
	ds2sf_destroy(ds);

	free(a);
	free(b);
	free(rom.data);

	printf("%s\n", failures ? "SMOKE TEST FAILED" : "SMOKE TEST PASSED");
	return failures ? 1 : 0;
}
