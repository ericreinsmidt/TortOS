/*
 * Derived from NextUI (github.com/LoveRetro/NextUI), GPL-3.0. Modified for PlayOS.
 * This file, and the minarch.elf built from it, are GPL-3.0 -- not PlayOS's 0BSD.
 * See minarch/overrides/README.md and THIRD-PARTY-LICENSES.md.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Modification: Core_open is split in two, and the tag can be stated outright.
 *
 * dlopen'ing a core costs ~170ms and is by far the expensive half of opening
 * one; everything else here is microseconds. PlayOS's resident minarch holds
 * all three cores open for the life of the process and pays that ~170ms once
 * each, at boot, behind the animation -- so the split is what makes launching
 * a GBA game after a NES game cost the same as launching another NES game.
 *
 *   Core_openLib()  dlopen + dlsym.        Expensive. Once per core, ever.
 *   Core_bind()     info, paths, callbacks. Cheap.   Once per game.
 *
 * The halves are NOT merely a tidy-up. Registering the environment callback
 * is what makes a core hand over its option list, so it has to happen per
 * game, immediately before that game's Config_init() -- exactly where upstream
 * has it. Doing it for three cores up front would leave the frontend holding
 * whichever core registered last.
 */
#include <dlfcn.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>

#include "ma_internal.h"
#include "ma_saves.h"
#include "ma_video.h"
#include "ma_audio.h"
#include "ma_input.h"
#include "ma_cheats.h"
#include "ma_core.h"


void Core_getName(char* in_name, char* out_name) {
	strcpy(out_name, basename(in_name));
	char* tmp = strrchr(out_name, '_');
	/* PlayOS: a core whose filename has no '_' used to dereference NULL. */
	if (tmp) tmp[0] = '\0';
}

/* The expensive half: bring the shared object in and resolve its entry points.
 * Nothing here touches frontend state, so it is safe to do for every core at
 * startup and leave them all resident. */
void Core_openLib(const char* core_path) {
	LOG_info("Core_openLib %s\n", core_path);
	core.handle = dlopen(core_path, RTLD_LAZY);

	if (!core.handle) { LOG_error("%s\n", dlerror()); return; }

	core.init = dlsym(core.handle, "retro_init");
	core.deinit = dlsym(core.handle, "retro_deinit");
	core.get_system_info = dlsym(core.handle, "retro_get_system_info");
	core.get_system_av_info = dlsym(core.handle, "retro_get_system_av_info");
	core.set_controller_port_device = dlsym(core.handle, "retro_set_controller_port_device");
	core.reset = dlsym(core.handle, "retro_reset");
	core.run = dlsym(core.handle, "retro_run");
	core.serialize_size = dlsym(core.handle, "retro_serialize_size");
	core.serialize = dlsym(core.handle, "retro_serialize");
	core.unserialize = dlsym(core.handle, "retro_unserialize");
	core.cheat_reset = dlsym(core.handle, "retro_cheat_reset");
	core.cheat_set = dlsym(core.handle, "retro_cheat_set");
	core.load_game = dlsym(core.handle, "retro_load_game");
	core.load_game_special = dlsym(core.handle, "retro_load_game_special");
	core.unload_game = dlsym(core.handle, "retro_unload_game");
	core.get_region = dlsym(core.handle, "retro_get_region");
	core.get_memory_data = dlsym(core.handle, "retro_get_memory_data");
	core.get_memory_size = dlsym(core.handle, "retro_get_memory_size");
}

/* The cheap half: ask the core what it is, work out where its files live, and
 * hand it the frontend callbacks. Run once per game, immediately before that
 * game's Config_init(). */
void Core_bind(const char* core_path, const char* tag_name) {
	LOG_info("Core_bind\n");
	if (!core.handle) { LOG_error("Core_bind: no core open\n"); return; }

	void (*set_environment_callback)(retro_environment_t);
	void (*set_video_refresh_callback)(retro_video_refresh_t);
	void (*set_audio_sample_callback)(retro_audio_sample_t);
	void (*set_audio_sample_batch_callback)(retro_audio_sample_batch_t);
	void (*set_input_poll_callback)(retro_input_poll_t);
	void (*set_input_state_callback)(retro_input_state_t);
	
	set_environment_callback = dlsym(core.handle, "retro_set_environment");
	set_video_refresh_callback = dlsym(core.handle, "retro_set_video_refresh");
	set_audio_sample_callback = dlsym(core.handle, "retro_set_audio_sample");
	set_audio_sample_batch_callback = dlsym(core.handle, "retro_set_audio_sample_batch");
	set_input_poll_callback = dlsym(core.handle, "retro_set_input_poll");
	set_input_state_callback = dlsym(core.handle, "retro_set_input_state");
	
	struct retro_system_info info = {};
	core.get_system_info(&info);
	

	LOG_info("Block Extract: %d\n", info.block_extract);

	Core_getName((char*)core_path, (char*)core.name);
	sprintf((char*)core.version, "%s (%s)", info.library_name, info.library_version);
	{
		/* PlayOS: the tag decides where saves, states and per-game configs
		 * live. Upstream infers it from a parenthesised (TAG) in the ROM's
		 * filename, falling back to the folder under ROMS_PATH -- which makes
		 * "Sonic (USA).pce" its own console. PlayOS states it outright, per
		 * system, so a folder rename cannot orphan a save. */
		const char* forced = getenv("PLAYOS_TAG");
		snprintf((char*)core.tag, sizeof core.tag, "%s",
		         (forced && *forced) ? forced : tag_name);
	}
	strcpy((char*)core.extensions, info.valid_extensions);
	
	core.need_fullpath = info.need_fullpath;
	
	LOG_info("core: %s version: %s tag: %s (valid_extensions: %s need_fullpath: %i)\n", core.name, core.version, core.tag, info.valid_extensions, info.need_fullpath);
	
	sprintf((char*)core.config_dir, USERDATA_PATH "/%s-%s", core.tag, core.name);
	sprintf((char*)core.states_dir, SHARED_USERDATA_PATH "/%s-%s", core.tag, core.name);
	sprintf((char*)core.saves_dir, SDCARD_PATH "/Saves/%s", core.tag);
	sprintf((char*)core.bios_dir, SDCARD_PATH "/Bios/%s", core.tag);
	sprintf((char*)core.cheats_dir, SDCARD_PATH "/Cheats/%s", core.tag);
	sprintf((char*)core.overlays_dir, SDCARD_PATH "/Overlays/%s", core.tag);
	
	char cmd[512];
	sprintf(cmd, "mkdir -p \"%s\"; mkdir -p \"%s\"", core.config_dir, core.states_dir);
	system(cmd);

	set_environment_callback(environment_callback);
	set_video_refresh_callback(video_refresh_callback);
	set_audio_sample_callback(audio_sample_callback);
	set_audio_sample_batch_callback(audio_sample_batch_callback);
	set_input_poll_callback(input_poll_callback);
	set_input_state_callback(input_state_callback);
}

/* Both halves, back to back: what upstream called Core_open, and still what
 * the one-game-per-process fallback path uses. */
void Core_open(const char* core_path, const char* tag_name) {
	Core_openLib(core_path);
	Core_bind(core_path, tag_name);
}

void Core_init(void) {
	LOG_info("Core_init\n");
	core.init();
	core.initialized = 1;
}

void Core_applyCheats(struct Cheats *cheats)
{
	if (!cheats)
		return;

	if (!core.cheat_reset || !core.cheat_set)
		return;

	core.cheat_reset();
	for (int i = 0; i < cheats->count; i++) {
		if (cheats->cheats[i].enabled) {
			core.cheat_set(i, cheats->cheats[i].enabled, cheats->cheats[i].code);
		}
	}
}

int Core_updateAVInfo(void) {
	struct retro_system_av_info av_info = {};
	core.get_system_av_info(&av_info);

	double a = av_info.geometry.aspect_ratio;
	if (a<=0) a = (double)av_info.geometry.base_width / av_info.geometry.base_height;

	int changed = (core.fps != av_info.timing.fps || core.sample_rate != av_info.timing.sample_rate || core.aspect_ratio != a);

	core.fps = av_info.timing.fps;
	core.sample_rate = av_info.timing.sample_rate;
	core.aspect_ratio = a;

	if (changed) LOG_info("aspect_ratio: %f (%ix%i) fps: %f\n", a, av_info.geometry.base_width,av_info.geometry.base_height, core.fps);

	return changed;
}

void Core_load(void) {
	LOG_info("Core_load\n");
	struct retro_game_info game_info;
	game_info.path = game.tmp_path[0]?game.tmp_path:game.path;
	game_info.data = game.data;
	game_info.size = game.size;
	LOG_info("game path: %s (%i)\n", game_info.path, game.size);
	core.load_game(&game_info);

	if (Cheats_load())
		Core_applyCheats(&cheatcodes);

	SRAM_read();
	RTC_read();
	// NOTE: must be called after core.load_game!
	core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD); // set a default, may update after loading configs
	Core_updateAVInfo();
}
void Core_reset(void) {
	core.reset();
	Rewind_on_state_change();
}
void Core_unload(void) {
	// Disabling this is a dumb hack for bluetooth, we should really be using 
	// bluealsa with --keep-alive=-1 - but SDL wont reconnect the stream on next start.
	// Reenable as soon as we have a more recent SDL available, if ever.
	//SND_quit();
}
void Core_quit(void) {
	if (core.initialized) {
		SRAM_write();
		Cheats_free();
		RTC_write();
		core.unload_game();
		core.deinit();
		core.initialized = 0;
	}
}
void Core_close(void) {
	if (core.handle) dlclose(core.handle);
}
