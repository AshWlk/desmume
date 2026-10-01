/*
	Copyright (C) 2026 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
	libds2sf: a headless, pull-style audio rendering API over the DeSmuME core, for 2SF playback.

	The caller assembles the cartridge ROM image (2SF program sections mapped by offset/size across
	the _lib chain), hands it to ds2sf_load_rom, and then pulls interleaved stereo s16 PCM with
	ds2sf_render. Nothing is threaded and no callbacks are exposed.

	DeSmuME keeps all emulator state in process globals, so only one ds2sf_t may exist at a time;
	ds2sf_create returns NULL while another instance is alive. Instances may be created and destroyed
	repeatedly (sequentially) within one process.

	Every fallible function returns NULL on success, or a human-readable error string that stays valid
	until the next call on the same instance.
*/

#ifndef DS2SF_H
#define DS2SF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  ifdef DS2SF_BUILD
#    define DS2SF_API __declspec(dllexport)
#  else
#    define DS2SF_API __declspec(dllimport)
#  endif
#else
#  define DS2SF_API __attribute__((visibility("default")))
#endif

typedef struct ds2sf ds2sf_t;

enum ds2sf_interpolation
{
	DS2SF_INTERP_NONE       = 0, /* sample-and-hold, as the hardware mixer does */
	DS2SF_INTERP_LINEAR     = 1,
	DS2SF_INTERP_COSINE     = 2,
	DS2SF_INTERP_CATMULLROM = 3
};

/* The DS mixer's native output rate: ARM7 clock (33513982 Hz) / 1024. */
#define DS2SF_NATIVE_SAMPLE_RATE 32728

typedef struct ds2sf_config
{
	uint32_t sample_rate;   /* 0 selects DS2SF_NATIVE_SAMPLE_RATE */
	int32_t  interpolation; /* enum ds2sf_interpolation */
} ds2sf_config;

/* Returns NULL if another instance is alive, the config is invalid, or the core fails to start.
   A NULL config selects the defaults (native rate, no interpolation). */
DS2SF_API ds2sf_t *ds2sf_create(const ds2sf_config *config);
DS2SF_API void ds2sf_destroy(ds2sf_t *ds);

/* Copies `size` bytes of cartridge ROM, padding the copy with zeroes to a power of two, then boots
   it (direct boot, HLE BIOS, generated firmware). Any previously loaded ROM is replaced. */
DS2SF_API const char *ds2sf_load_rom(ds2sf_t *ds, const uint8_t *rom, size_t size);

/* Re-boots the loaded ROM from scratch and discards buffered audio. */
DS2SF_API const char *ds2sf_reset(ds2sf_t *ds);

/* Emulates `frames` video frames (~1/59.8261 s each) and discards the audio they produce.
   This is what the 2SF `_frames` tag asks for. */
DS2SF_API const char *ds2sf_run_frames(ds2sf_t *ds, uint32_t frames);

/* Writes exactly `count` interleaved stereo frames (2 * count int16_t values) to `out`.
   Emulates as many whole video frames as needed; surplus audio is kept for the next call. */
DS2SF_API const char *ds2sf_render(ds2sf_t *ds, int16_t *out, size_t count);

DS2SF_API uint32_t ds2sf_get_sample_rate(const ds2sf_t *ds);

/*
	Per-channel output, for splitting a track into stems.

	With channel output enabled, every rendered frame is also kept per SPU channel (each channel's
	volume-scaled, panned contribution to the mixer), along with a log of the key-ons that start each
	note. Summing the 16 channels and clamping to int16 reproduces the mix, unless SOUNDCNT routes
	channels around the mixer (see DS2SF_ROUTING_OUTPUT_BYPASS).
*/

/* ds2sf_keyon_event.flags */
#define DS2SF_KEYON_CAPTURE_BUFFER 0x01 /* the address lies in a sound-capture destination buffer */

/* A channel starting a note: from `offset` on, the channel plays this sample until its next key-on. */
typedef struct ds2sf_keyon_event
{
	uint32_t offset;  /* frame within the ds2sf_render_channels output at which the note starts */
	uint8_t  channel; /* 0-15 */
	uint8_t  format;  /* 0 PCM8, 1 PCM16, 2 IMA-ADPCM, 3 PSG (channels 8-13 square, 14-15 noise) */
	uint8_t  duty;    /* PSG square duty cycle, 0-7 (format 3 only) */
	uint8_t  flags;   /* DS2SF_KEYON_* */
	uint32_t address; /* SOUNDxSAD, the sample's start address (formats 0-2) */
} ds2sf_keyon_event;

/* ds2sf_get_routing_flags bits, accumulated while channel output is enabled */
#define DS2SF_ROUTING_OUTPUT_BYPASS 0x01 /* SOUNDCNT sent channel 1/3 around the mixer: the channels no longer sum to the mix */
#define DS2SF_ROUTING_CAPTURE       0x02 /* a sound-capture unit ran */

/* Starts (non-zero) or stops (0) keeping per-channel output. Enable it before loading the ROM, or
   at least before rendering: audio already buffered when it is enabled has silent channels. Notes
   still sounding when it is enabled, or across ds2sf_run_frames, are re-reported at the next
   rendered frame. Disabling discards any buffered channel output and key-ons. */
DS2SF_API const char *ds2sf_set_channel_output(ds2sf_t *ds, int enabled);

/* ds2sf_render, plus each channel's output for the same frames. Requires channel output enabled.
   `mix`      (may be NULL): `count` interleaved stereo frames, exactly as ds2sf_render.
   `channels` : 16 * count interleaved stereo frames, channel-major: channel c's frames start at
                channels + c * count * 2.
   `events`   : the key-ons in these frames, ordered by offset (per channel, chronologically). At most
                one is reported per channel per frame, so 16 * count entries always suffice; fewer
                fails the call if the frames hold more key-ons than fit.
   `event_count` receives the number of entries written. */
DS2SF_API const char *ds2sf_render_channels(ds2sf_t *ds, int16_t *mix, int16_t *channels, size_t count,
	ds2sf_keyon_event *events, size_t event_capacity, size_t *event_count);

/* DS2SF_ROUTING_* flags seen since channel output was enabled. */
DS2SF_API uint32_t ds2sf_get_routing_flags(const ds2sf_t *ds);

#ifdef __cplusplus
}
#endif

#endif
