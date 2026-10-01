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

#include "ds2sf.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "NDSSystem.h"
#include "SPU.h"
#include "GPU.h"
#include "render3D.h"
#include "rtc.h"
#include "common.h"
#include "mc.h"
#include "debug.h"

// ---------------------------------------------------------------------------------------------
// Globals every DeSmuME frontend must define.
// ---------------------------------------------------------------------------------------------

volatile bool execute = false;

#define SNDCORE_DS2SF 0x2534

static int Ds2sfSndInit(int buffersize);
static void Ds2sfSndDeInit();
static void Ds2sfSndUpdateAudio(s16 *buffer, u32 num_samples);
static u32 Ds2sfSndGetAudioSpace();
static void Ds2sfSndNoop();
static void Ds2sfSndSetVolume(int volume);
static void Ds2sfSndFetchSamples(s16 *sampleBuffer, size_t sampleCount, ESynchMode synchMode, ISynchronizingAudioBuffer *theSynchronizer);

static SoundInterface_struct SNDDs2sf = {
	SNDCORE_DS2SF,
	"ds2sf capture",
	Ds2sfSndInit,
	Ds2sfSndDeInit,
	Ds2sfSndUpdateAudio,
	Ds2sfSndGetAudioSpace,
	Ds2sfSndNoop,
	Ds2sfSndNoop,
	Ds2sfSndSetVolume,
	Ds2sfSndNoop,
	Ds2sfSndFetchSamples,
	NULL,
};

SoundInterface_struct *SNDCoreList[] = {
	&SNDDummy,
	&SNDDs2sf,
	NULL
};

GPU3DInterface *core3DList[] = {
	&gpu3DNull,
	NULL
};

// ---------------------------------------------------------------------------------------------
// Audio capture.
//
// Once per scanline, SPU_Emulate_core mixes that scanline's samples into SPU_core->outbuf and hands
// them to the active sound core's FetchSamples. Synchronous mode makes it actually mix there (in the
// default dual synch/asynch mode the mix is deferred to SPU_Emulate_user, which we never call).
// ---------------------------------------------------------------------------------------------

static std::vector<s16> g_pending; // interleaved stereo
static size_t g_pendingStart = 0;  // index of the first unconsumed value in g_pending

static void PendingClear()
{
	g_pending.clear();
	g_pendingStart = 0;
}

static size_t PendingFrames()
{
	return (g_pending.size() - g_pendingStart) / 2;
}

static int Ds2sfSndInit(int) { PendingClear(); return 0; }
static void Ds2sfSndDeInit() { PendingClear(); }
static void Ds2sfSndUpdateAudio(s16 *, u32) {}
static u32 Ds2sfSndGetAudioSpace() { return 0; }
static void Ds2sfSndNoop() {}
static void Ds2sfSndSetVolume(int) {}

static void Ds2sfSndFetchSamples(s16 *sampleBuffer, size_t sampleCount, ESynchMode, ISynchronizingAudioBuffer *)
{
	g_pending.insert(g_pending.end(), sampleBuffer, sampleBuffer + sampleCount * 2);
}

// ---------------------------------------------------------------------------------------------
// Console output. The core chats on stdout (version banner, slot/mic/wifi init, ROM info), which
// would interleave with the host application's own output. The library is linked with
// -Wl,--wrap=printf,puts,putchar,vprintf (see CMakeLists.txt), so the core's calls land here; they
// are dropped unless DS2SF_VERBOSE is set in the environment, in which case they go to stderr.
// This only affects references inside libds2sf, never the host process.
// ---------------------------------------------------------------------------------------------

static bool CoreOutputEnabled()
{
	static const bool verbose = getenv("DS2SF_VERBOSE") != NULL;
	return verbose;
}

extern "C" {

int __wrap_vprintf(const char *fmt, va_list args)
{
	return CoreOutputEnabled() ? vfprintf(stderr, fmt, args) : 0;
}

int __wrap_printf(const char *fmt, ...)
{
	if (!CoreOutputEnabled())
		return 0;

	va_list args;
	va_start(args, fmt);
	const int written = vfprintf(stderr, fmt, args);
	va_end(args);
	return written;
}

int __wrap_puts(const char *s)
{
	if (CoreOutputEnabled())
	{
		fputs(s, stderr);
		fputc('\n', stderr);
	}
	return 0;
}

int __wrap_putchar(int c)
{
	if (CoreOutputEnabled())
		fputc(c, stderr);
	return c;
}

} // extern "C"

// ---------------------------------------------------------------------------------------------
// Message boxes: the default implementation prints to stdout. Route them like the rest of the core's
// console output.
// ---------------------------------------------------------------------------------------------

static void Ds2sfMsgPrint(const char *fmt, va_list args)
{
	if (!CoreOutputEnabled())
		return;
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
}

static void Ds2sfMsgInfo(const char *fmt, ...) { va_list args; va_start(args, fmt); Ds2sfMsgPrint(fmt, args); va_end(args); }
static bool Ds2sfMsgConfirm(const char *, ...) { return true; }
static void Ds2sfMsgError(const char *fmt, ...) { va_list args; va_start(args, fmt); Ds2sfMsgPrint(fmt, args); va_end(args); }
static void Ds2sfMsgWarn(const char *fmt, ...) { va_list args; va_start(args, fmt); Ds2sfMsgPrint(fmt, args); va_end(args); }

static msgBoxInterface Ds2sfMsgBox = {
	Ds2sfMsgInfo,
	Ds2sfMsgConfirm,
	Ds2sfMsgError,
	Ds2sfMsgWarn,
};

// INFO()/LOG() go through DeSmuME's Logger (to std::cout by default) rather than printf.
static void Ds2sfLogCallback(const Logger &, const char *message)
{
	if (CoreOutputEnabled())
		fputs(message, stderr);
}

static void SilenceCoreLogger()
{
	for (unsigned int channel = 0; channel < 16; channel++)
		Logger::log(channel, __FILE__, __LINE__, Ds2sfLogCallback);
}

// ---------------------------------------------------------------------------------------------
// Instance.
// ---------------------------------------------------------------------------------------------

struct ds2sf
{
	u32 sampleRate;
	bool romLoaded;
	std::string error;
};

static ds2sf *g_instance = NULL;
static bool g_coreInitialised = false;

// The ARM9 runs ~560190 cycles per frame at ~59.8261 frames per second; a frame of silence is used
// when the emulated system produced no audio (see RunOneFrame).
static const double kFramesPerSecond = 59.8261;

static const char *Fail(ds2sf *ds, const std::string &message)
{
	ds->error = message;
	return ds->error.c_str();
}

static const char *DescribeHalt(ds2sf *ds)
{
	const NDSError err = NDS_GetLastError();
	const char *what;

	switch (err.code)
	{
		case NDSError_SystemPoweredOff:                  what = "system powered off"; break;
		case NDSError_JITUnmappedAddressException:       what = "JIT unmapped address exception"; break;
		case NDSError_ARMUndefinedInstructionException:  what = "undefined instruction exception"; break;
		case NDSError_NoError:                           what = "emulation halted"; break;
		default:                                         what = "unknown emulation error"; break;
	}

	char buf[256];
	snprintf(buf, sizeof(buf), "%s (ARM9 PC=%08X instr=%08X, ARM7 PC=%08X instr=%08X)",
		what, err.programCounterARM9, err.instructionARM9, err.programCounterARM7, err.instructionARM7);
	return Fail(ds, buf);
}

static void ApplySettings(u32 sampleRate, int interpolation)
{
	// Direct ("fake") boot with the HLE BIOS and internally generated firmware: no external files.
	// The HLE BIOS implements the SWIs the NitroSDK sound driver uses (SoundBias, Get*Table).
	CommonSettings.UseExtBIOS = false;
	CommonSettings.SWIFromBIOS = false;
	CommonSettings.UseExtFirmware = false;
	CommonSettings.UseExtFirmwareSettings = false;
	CommonSettings.BootFromFirmware = false;
	CommonSettings.ConsoleType = NDS_CONSOLE_TYPE_FAT;

	// Accuracy over speed; matches what the Audacious 2SF plugin pins.
	CommonSettings.use_jit = false;
	CommonSettings.rigorous_timing = true;
	CommonSettings.advanced_timing = true;
	CommonSettings.spu_advanced = true;
	CommonSettings.spuInterpolationMode = interpolation;
	CommonSettings.SPU_sync_mode = ESynchMode_Synchronous;
	CommonSettings.SPU_sync_method = ESynchMethod_N;
	for (size_t i = 0; i < ARRAY_SIZE(CommonSettings.spu_muteChannels); i++)
		CommonSettings.spu_muteChannels[i] = false;
	CommonSettings.spu_captureMuted = false;

	// Never touch the filesystem: ROM is in memory, backup memory stays in RAM.
	CommonSettings.loadToMemory = true;
	CommonSettings.backupDeviceInMemory = true;
	CommonSettings.backupSave = false;
	CommonSettings.autodetectBackupMethod = BackupDeviceAutodetectMethod_Desmume;

	SPU_SetSampleRate(sampleRate);

	// Deterministic output: the RTC starts at a fixed time on every boot and advances with frames.
	rtcSetFixedStartTime(DateTime(2000, 1, 1, 0, 0, 0));
}

// Emulates one video frame. GPU rendering is skipped every frame; the 2D/3D engines still run their
// register/timing side effects (Phase A keeps them; Phase B stubs them out).
static const char *RunOneFrame(ds2sf *ds)
{
	const size_t before = PendingFrames();

	execute = true;
	NDS_SkipNextFrame();
	NDS_exec<false>();

	if (!execute)
		return DescribeHalt(ds);

	// While both CPUs are asleep (nds.sleeping) NDS_exec returns without advancing any hardware, so no
	// audio is produced. The DS outputs silence then, so emit a frame's worth of it; otherwise
	// ds2sf_render could spin forever waiting for samples.
	if (PendingFrames() == before)
	{
		const size_t silentFrames = (size_t)(ds->sampleRate / kFramesPerSecond + 0.5);
		g_pending.insert(g_pending.end(), silentFrames * 2, 0);
	}

	return NULL;
}

extern "C" {

DS2SF_API ds2sf_t *ds2sf_create(const ds2sf_config *config)
{
	if (g_instance != NULL)
		return NULL;

	u32 sampleRate = DS2SF_NATIVE_SAMPLE_RATE;
	int interpolation = DS2SF_INTERP_NONE;
	if (config != NULL)
	{
		if (config->sample_rate != 0)
			sampleRate = config->sample_rate;
		interpolation = config->interpolation;
	}

	if (sampleRate < 8000 || sampleRate > 192000)
		return NULL;
	if (interpolation < DS2SF_INTERP_NONE || interpolation > DS2SF_INTERP_CATMULLROM)
		return NULL;

	msgbox = &Ds2sfMsgBox;
	SilenceCoreLogger();
	ApplySettings(sampleRate, interpolation);

	// The core is initialised once per process and kept alive between instances: upstream frontends
	// never re-run NDS_Init after NDS_DeInit, and every ROM load/reset rebuilds the SPU anyway
	// (picking up the sample rate set above).
	if (!g_coreInitialised)
	{
		if (NDS_Init() != 0)
			return NULL;

		g_coreInitialised = true;
	}

	GPU->Change3DRendererByID(RENDERID_NULL);

	if (SPU_ChangeSoundCore(SNDCORE_DS2SF, 735 * 4) != 0)
		return NULL;
	SPU_SetSynchMode(ESynchMode_Synchronous, ESynchMethod_N);

	execute = false;
	PendingClear();

	ds2sf *ds = new ds2sf;
	ds->sampleRate = sampleRate;
	ds->romLoaded = false;
	g_instance = ds;
	return ds;
}

DS2SF_API void ds2sf_destroy(ds2sf_t *ds)
{
	if (ds == NULL || ds != g_instance)
		return;

	execute = false;
	if (ds->romLoaded)
		NDS_FreeROM();

	PendingClear();
	rtcClearFixedStartTime();

	g_instance = NULL;
	delete ds;
}

DS2SF_API const char *ds2sf_load_rom(ds2sf_t *ds, const uint8_t *rom, size_t size)
{
	if (ds == NULL || ds != g_instance)
		return "invalid ds2sf instance";
	if (rom == NULL || size < 0x200)
		return Fail(ds, "ROM image is smaller than a cartridge header (0x200 bytes)");
	if (size > 0x80000000u)
		return Fail(ds, "ROM image is larger than 2 GiB");

	// Cartridge reads are masked to a power-of-two size; pad so every byte of the image is reachable
	// and the tail reads back as zero (matches the reference 2SF players).
	size_t padded = 1;
	while (padded < size)
		padded <<= 1;

	std::vector<u8> image(padded, 0);
	memcpy(&image[0], rom, size);

	execute = false;
	if (ds->romLoaded)
	{
		NDS_FreeROM();
		ds->romLoaded = false;
	}

	if (NDS_LoadROMFromMemory(&image[0], (u32)padded) < 1)
		return Fail(ds, "DeSmuME rejected the ROM image: not a valid DS cartridge header (set DS2SF_VERBOSE=1 for details)");

	ds->romLoaded = true;
	PendingClear();
	execute = true;
	return NULL;
}

DS2SF_API const char *ds2sf_reset(ds2sf_t *ds)
{
	if (ds == NULL || ds != g_instance)
		return "invalid ds2sf instance";
	if (!ds->romLoaded)
		return Fail(ds, "no ROM loaded");

	NDS_Reset();
	PendingClear();
	execute = true;
	return NULL;
}

DS2SF_API const char *ds2sf_run_frames(ds2sf_t *ds, uint32_t frames)
{
	if (ds == NULL || ds != g_instance)
		return "invalid ds2sf instance";
	if (!ds->romLoaded)
		return Fail(ds, "no ROM loaded");

	for (uint32_t i = 0; i < frames; i++)
	{
		if (const char *err = RunOneFrame(ds))
			return err;
	}

	PendingClear();
	return NULL;
}

DS2SF_API const char *ds2sf_render(ds2sf_t *ds, int16_t *out, size_t count)
{
	if (ds == NULL || ds != g_instance)
		return "invalid ds2sf instance";
	if (count == 0)
		return NULL;
	if (out == NULL)
		return Fail(ds, "output buffer is NULL");
	if (!ds->romLoaded)
		return Fail(ds, "no ROM loaded");

	while (PendingFrames() < count)
	{
		if (const char *err = RunOneFrame(ds))
			return err;
	}

	const size_t values = count * 2;
	memcpy(out, &g_pending[g_pendingStart], values * sizeof(s16));
	g_pendingStart += values;

	// Compact once the consumed prefix dominates, so the buffer stays about one frame long.
	if (g_pendingStart >= g_pending.size() / 2)
	{
		g_pending.erase(g_pending.begin(), g_pending.begin() + g_pendingStart);
		g_pendingStart = 0;
	}

	return NULL;
}

DS2SF_API uint32_t ds2sf_get_sample_rate(const ds2sf_t *ds)
{
	return ds != NULL ? ds->sampleRate : 0;
}

} // extern "C"
