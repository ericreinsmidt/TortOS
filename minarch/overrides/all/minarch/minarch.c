/*
 * Derived from NextUI (github.com/LoveRetro/NextUI), GPL-3.0. Modified for PlayOS.
 * This file, and the minarch.elf built from it, are GPL-3.0 -- not PlayOS's 0BSD.
 * See minarch/overrides/README.md and THIRD-PARTY-LICENSES.md.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Modification: a resident mode that holds the GL context and all three cores
 * between games, taking a launch from ~1100ms to ~200ms. See the block comment
 * over the residency code further down.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <msettings.h>

#include <SDL2/SDL_image.h>

#include "notification.h"
#include "ra_integration.h"

#include "ma_internal.h"
#include "ma_cheats.h"
#include "ma_audio.h"
#include "ma_input.h"
#include "ma_options.h"
#include "ma_frontend_opts.h"
#include "ma_saves.h"
#include "ma_video.h"
#include "ma_core.h"
#include "ma_game.h"
#include "ma_environment.h"
#include "ma_config.h"
#include "ma_runframe.h"

///////////////////////////////////////

SDL_Surface* screen;
int quit = 0;
int newScreenshot = 0;
int show_menu = 0;
int simple_mode = 0;
enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

// default frontend options
int screen_scaling = SCALE_ASPECT;
int resampling_quality = 2;
int ambient_mode = 0;
int screen_sharpness = SHARPNESS_SOFT;
int screen_effect = EFFECT_NONE;
int cfg_screenx = 64;
int cfg_screeny = 64;
int overlay = 0; 
int use_core_fps = 0;
int sync_ref = 0;
int show_debug = 0;
int max_ff_speed = 3; // 4x
int ff_audio = 0;
int fast_forward = 0;
int rewind_pressed = 0;
int rewind_toggle = 0;
int last_rewind_pressed = 0;
int ff_toggled = 0;
int ff_hold_active = 0;
int ff_paused_by_rewind_hold = 0;
int rewinding = 0;
int rewind_cfg_enable = MINARCH_DEFAULT_REWIND_ENABLE;
int rewind_cfg_buffer_mb = MINARCH_DEFAULT_REWIND_BUFFER_MB;
int rewind_cfg_granularity = MINARCH_DEFAULT_REWIND_GRANULARITY;
int rewind_cfg_audio = MINARCH_DEFAULT_REWIND_AUDIO;
int rewind_cfg_compress = 1;
int rewind_cfg_lz4_acceleration = MINARCH_DEFAULT_REWIND_LZ4_ACCELERATION;
int rewind_init_ready = 0; // gate Rewind_init from syncFrontend until startup is past Core_load
int overclock = 0; // auto
int has_custom_controllers = 0;
int gamepad_type = 0; // index in gamepad_labels/gamepad_values

// these are no longer constants as of the RG CubeXX (even though they look like it)
int DEVICE_WIDTH = 0;
int DEVICE_HEIGHT = 0;
int DEVICE_PITCH = 0;
int shader_reset_suppressed = 0;

GFX_Renderer renderer;

///////////////////////////////////////

struct Core core;



///////////////////////////////
static struct Special {
	int palette_updated;
} special;
void Special_updatedDMGPalette(int frames) {
	// LOG_info("Special_updatedDMGPalette(%i)\n", frames);
	special.palette_updated = frames; // must wait a few frames
}
static void Special_refreshDMGPalette(void) {
	special.palette_updated -= 1;
	if (special.palette_updated>0) return;
	
	int rgb = getInt("/tmp/dmg_grid_color");
	GFX_setEffectColor(rgb);
}
static void Special_init(void) {
	if (special.palette_updated>1) special.palette_updated = 1;
	// else if (exactMatch((char*)core.tag, "GBC"))  {
	// 	putInt("/tmp/dmg_grid_color",0xF79E);
	// 	special.palette_updated = 1;
	// }
}
void Special_render(void) {
	if (special.palette_updated) Special_refreshDMGPalette();
}
static void Special_quit(void) {
	system("rm -f /tmp/dmg_grid_color");
}
///////////////////////////////

///////////////////////////////

void hdmimon(void) {
	// handle HDMI change
	static int had_hdmi = -1;
	int has_hdmi = GetHDMI();
	if (had_hdmi==-1) had_hdmi = has_hdmi;
	if (has_hdmi!=had_hdmi) {
		had_hdmi = has_hdmi;

		LOG_info("restarting after HDMI change...\n");
		Menu_beforeSleep();
		sleep(4);
		show_menu = 0;
		quit = 1;
	}
}

#define PWR_UPDATE_FREQ 5
#define PWR_UPDATE_FREQ_INGAME 20

/* ---- PlayOS: residency across three cores ---------------------------------
 *
 * Starting a game costs about 1100ms, and almost none of it is the game. On
 * this hardware: the EGL/GL context ~620ms, the core's dlopen ~170ms, audio
 * and settings ~140ms -- against ~36ms to actually open the ROM. All of that
 * except the ROM is the cost of STARTING A PROCESS. So it is paid once, at
 * boot, behind the boot animation, and every launch after it is the 36ms.
 *
 * `minarch.elf --resident NES=/path/fceumm.so PCE=/path/pce.so GBA=/path/mgba.so`
 * stays up and runs one game after another. Between games it blocks on a fifo
 * holding the GL context and all three cores, but nothing exclusive: the audio
 * device is released so the launcher's own sounds keep working, and being
 * blocked it reads no input and draws no frames.
 *
 * THREE CORES, ONE PROCESS. The alternative designs were one resident process
 * per core (three EGL contexts on a device with one framebuffer -- the failure
 * mode is not a slow launch, it is no picture) and a resident that dlclose /
 * dlopens on every system change (~170ms, better than 1100 but paid on every
 * switch). Keeping all three open is the only one where switching from a NES
 * game to a GBA game costs the same as launching another NES game, and the
 * cost of holding them is a few MB of mapped, untouched pages.
 *
 * Three cores in one address space is safe here because of how they are
 * built. Checked with nm -D on the three shipped cores: every defined text
 * symbol in each one is a retro_* entry point and there is nothing else --
 * 45 of 45 in fceumm, 53 of 53 in mednafen_pce_fast, 25 of 25 in mgba. So the
 * only names they could possibly collide on are the entry points, and
 * dlopen's default RTLD_LOCAL keeps even those out of the global namespace,
 * so no core can bind to another's. Only one is ever retro_init'd; the rest
 * are mapped and idle.
 *
 * The launcher falls back to running this program the old way, one game per
 * process, whenever residency is not answering -- which is why the classic
 * path below still exists.
 *
 * PLAYOS_PRELOAD=0 turns preloading off: cores are then opened on first use
 * and kept, which is the same steady state one system at a time but a slower
 * first launch on each. It exists to be measured against, not because it is
 * expected to win.
 */

#define PLAYOS_MAX_CORES 8
#define PLAYOS_PID "/tmp/playos_res.pid"
#define PLAYOS_REQ "/tmp/playos_req"
#define PLAYOS_REP "/tmp/playos_rep"

extern void Core_openLib(const char* core_path);
extern void Core_bind(const char* core_path, const char* tag_name);
extern void Input_reset(void);
extern void Menu_savePreview(SDL_Surface* frame, int slot);

struct CoreSlot {
	char tag[16];
	char path[MAX_PATH];
	struct Core core;   /* handle + entry points; plain data, copyable */
	int  open;
};

static struct CoreSlot slots[PLAYOS_MAX_CORES];
static int slot_count;

/* The real screen surface, as GFX_init returned it. The quit animation at the
 * end of a game reassigns the global `screen` to a temporary surface and then
 * frees it, leaving it dangling -- which nobody noticed because the process
 * always exited immediately afterwards. A resident process runs Menu_init()
 * again for the next game, and that reads screen->format. Put it back first. */
static SDL_Surface *playos_screen;
static uint32_t playos_req_ms;      /* when the current request arrived */

/* End the running game without ending the process. The launcher's power-button
 * watcher SIGTERMs minarch when minarch is one game per process, which is
 * still right there -- but would take residency down with it. SIGUSR1 ends the
 * game only; the process falls back to waiting on the fifo. */
static void playos_end_game(int sig) { (void)sig; quit = 1; }

static int slot_find(const char *tag)
{
	int i;
	for (i = 0; i < slot_count; i++)
		if (!strcmp(slots[i].tag, tag)) return i;
	return -1;
}

/* Add a core to the table. Opening it is separate, so the table can be built
 * from the command line and the dlopens deferred (or skipped). */
static int slot_add(const char *tag, const char *path)
{
	int i = slot_find(tag);
	if (i >= 0) return i;
	if (slot_count >= PLAYOS_MAX_CORES) return -1;
	i = slot_count++;
	snprintf(slots[i].tag, sizeof slots[i].tag, "%s", tag);
	snprintf(slots[i].path, sizeof slots[i].path, "%s", path);
	slots[i].open = 0;
	return i;
}

/* dlopen the core in slot i, unless it is already open. ~170ms, logged --
 * this is the number the three-cores-resident design is spending, and the one
 * to look at if it ever needs revisiting. */
static int slot_open(int i)
{
	uint32_t t0;
	if (i < 0 || i >= slot_count) return 0;
	if (slots[i].open) return 1;

	t0 = SDL_GetTicks();
	memset(&core, 0, sizeof core);
	Core_openLib(slots[i].path);
	if (!core.handle) {
		LOG_error("resident: %s: cannot open %s\n", slots[i].tag, slots[i].path);
		return 0;
	}
	memcpy(&slots[i].core, &core, sizeof core);  /* const members: memcpy, not = */
	slots[i].open = 1;
	LOG_info("boot: core_open %-4s %ums\n", slots[i].tag, SDL_GetTicks() - t0);
	return 1;
}

/* Everything the process owns, brought up once. */
static void resident_init(void)
{
	int i;

	screen = GFX_init(MODE_MENU);

	/* Startup is dominated by this one call -- SDL video plus the EGL/GL
	 * context. Logged because it is the number that decides whether launching
	 * can ever be made quick. */
	LOG_info("boot: gfx_init      %ums\n", SDL_GetTicks());

	GFX_initShaders();
	PLAT_initNotificationTexture();

	PAD_init();
	DEVICE_WIDTH = screen->w;
	DEVICE_HEIGHT = screen->h;
	DEVICE_PITCH = screen->pitch;

	/* The lights are handled in the api.c override, not here: GFX_init()
	 * calls LEDS_initLeds() from inside api.c, so dropping this call would
	 * change nothing. Left in place so this file stays close to NextUI's. */
	LEDS_initLeds();
	VIB_init();
	PWR_init();
	/* The power button belongs to the launcher. It watches this process and
	 * ends the game on a press, then shows its own send-off -- but only if
	 * minarch does not get there first. PWR_update() calls PWR_powerOff()
	 * directly on a press, which paints "Powering off" and cuts power
	 * immediately; disabling it makes PWR_powerOff a no-op and leaves the
	 * press for the launcher. Sleep goes with it: BTN_SLEEP is BTN_POWER on
	 * this device, so leaving sleep armed would swallow the same press. */
	PWR_disablePowerOff();
	PWR_disableSleep();
	MSG_init();
	IMG_Init(IMG_INIT_PNG);

	{
		const char *pre = getenv("PLAYOS_PRELOAD");
		if (!pre || strcmp(pre, "0") != 0)
			for (i = 0; i < slot_count; i++) slot_open(i);
	}

	/* Once, before any audio. Re-running InitSettings per game re-initialises
	 * the codec and pops the speaker on every single launch. */
	InitSettings();
	playos_screen = screen;
}

/* Everything a single game needs, set up and torn back down. Returns 0 when
 * the game ran, -1 if it could not be started. */
static int run_one_game(int slot, char *rom_path)
{
	if (slot < 0 || slot >= slot_count) return -1;
	if (!slot_open(slot)) return -1;

	/* Restore the screen the previous game's quit animation left dangling. */
	if (playos_screen) screen = playos_screen;

	/* Per-game globals. A resident process runs this more than once, so
	 * anything the frame loop accumulates has to start clean. */
	quit = 0; show_menu = 0; newScreenshot = 0;
	fast_forward = 0; ff_toggled = 0; ff_hold_active = 0;
	rewind_pressed = 0; rewind_toggle = 0; last_rewind_pressed = 0;
	rewinding = 0; ff_paused_by_rewind_hold = 0;
	rewind_init_ready = 0; shader_reset_suppressed = 0;

	/* Set by the environment callback, and only by cores that bother. A core
	 * that says nothing would otherwise inherit the last core's answer:
	 * libretro's default pixel format is 0RGB1555, so a silent core after an
	 * XRGB8888 one gets its video read through the wrong converter, and a
	 * core with no disk-control interface would keep the previous core's
	 * function pointers and be called through them with no game loaded. All
	 * three shipped cores set the format, so this is insurance -- but it is
	 * exactly the kind of thing residency turns from impossible into subtle. */
	fmt = RETRO_PIXEL_FORMAT_0RGB1555;
	memset(&disk_control_ext, 0, sizeof disk_control_ext);
	has_custom_controllers = 0;
	gamepad_type = 0;

	/* Were statics in NextUI, where the process only ever ran one game. As
	 * locals they reset per game, so a launch cannot flash the settings line
	 * left over from the last one. */
	int last_volume = -1, last_brightness = -1, last_colortemp = -1;

	/* Point the frontend at this core and let it register its callbacks and
	 * its option list, immediately before the Config_* calls that read them --
	 * exactly where upstream has it, which is the whole reason Core_open is
	 * split in two. The dlopen it used to do here has already happened. */
	memcpy(&core, &slots[slot].core, sizeof core);
	setenv("PLAYOS_TAG", slots[slot].tag, 1);
	Core_bind(slots[slot].path, slots[slot].tag);
	Input_reset();   /* the button map is per core; see ma_input.c */

	Game_open(rom_path); // nes tries to load gamegenie setting before this returns ffs
	if (!game.is_open) {
		/* Upstream jumped to its teardown here and then exited. A resident
		 * process goes back to waiting instead, so a ROM that will not open
		 * has to leave nothing behind for the next game -- Game_open can have
		 * allocated the ROM buffer before giving up. Nothing after this point
		 * has run, so Game_close is the whole of the cleanup. */
		LOG_warn("could not open %s\n", rom_path);
		Game_close();
		return -1;
	}

	simple_mode = exists(SIMPLE_MODE_PATH);

	// restore options
	Config_load(); // before init?
	Config_init();
	Config_readOptions(); // cores with boot logo option (eg. gb) need to load options early

	Core_init();

	// Initialize RetroAchievements after core.init() but before Core_load()
	RA_setMemoryAccessors(core.get_memory_data, core.get_memory_size);
	RA_init();

	Menu_setCoreVersionDesc(core.version);
	Core_load();

	Input_init(NULL);
	Config_readOptions(); // but others load and report options later (eg. nes)
	Config_readControls(); // restore controls (after the core has reported its defaults)

	// Mute audio during startup to avoid pops
	SND_overrideMute(1);
	SND_init(core.sample_rate, core.fps);
	SND_registerDeviceWatcher(Audio_onSinkChanged);
	/* InitSettings() used to sit here. It belongs to the process, not to one
	 * game: re-running it per game re-inits the codec and pops the speaker.
	 * Moved to resident_init(), which is also why it now runs before audio. */
	Menu_init();
	Notification_init();

	{
		char* rom_path_for_ra = game.tmp_path[0] ? game.tmp_path : game.path;
		RA_loadGame(rom_path_for_ra, game.data, game.size, core.tag);
	}

	/* The auto-resume. The launcher writes slot 9 into /tmp/resume_slot.txt
	 * before every launch, and this loads it if that state exists -- so a game
	 * always comes up exactly where it was left. State_resume() consumes the
	 * file, so a game started any other way still starts fresh. */
	State_resume();
	Menu_initState(); // make ready for state shortcuts

	PWR_disableAutosleep();
	PWR_updateFrequency(PWR_UPDATE_FREQ, 0);

	// force a vsync immediately before loop for better frame pacing
	GFX_clearAll();
	GFX_clearLayers(0);
	GFX_clear(screen);
	GFX_flip(screen);

	Special_init(); // after config

	chooseSyncRef();

	int has_pending_opt_change = 0;

	initShaders();
	Config_readOptions();
	applyShaderSettings();
	int rewind_initialized = Rewind_init(core.serialize_size ? core.serialize_size() : 0);
	rewind_init_ready = 1;
	if (rewind_initialized && core.serialize_size) Rewind_on_state_change();
	Config_free();

	if (playos_req_ms)
		LOG_info("resident: %s up in %ums\n", slots[slot].tag,
		         SDL_GetTicks() - playos_req_ms);
	else
		LOG_info("total startup time %ims\n\n", SDL_GetTicks());

	// we started in performance mode, now reset to the desired mode
	setOverclock(overclock);

	while (!quit) {
		GFX_startFrame();

		run_frame();

		RA_doFrame();

		Notification_update(SDL_GetTicks());

		// Poll for volume/brightness changes and raise the settings line
		{
			int cur_volume = GetVolume();
			int cur_brightness = GetBrightness();
			int cur_colortemp = GetColortemp();

			if (last_volume == -1) {
				// First frame - just cache, don't show the indicator
				last_volume = cur_volume;
				last_brightness = cur_brightness;
				last_colortemp = cur_colortemp;
			} else {
				/* PlayOS: these raise the launcher's thin top line, not
				 * NextUI's corner pill -- render_system_indicator is
				 * overridden in common/notification.c. Colortemp is tracked
				 * but never shown; PlayOS does not change it. */
				if (cur_volume != last_volume) {
					last_volume = cur_volume;
					Notification_showSystemIndicator(SYSTEM_INDICATOR_VOLUME);
				}
				if (cur_brightness != last_brightness) {
					last_brightness = cur_brightness;
					Notification_showSystemIndicator(SYSTEM_INDICATOR_BRIGHTNESS);
				}
				last_colortemp = cur_colortemp;
			}
		}

		Notification_renderToLayer(5);  // Always call - handles cleanup when inactive

		if (has_pending_opt_change) {
			has_pending_opt_change = 0;
			if (Core_updateAVInfo()) {
				LOG_info("AV info changed, reset sound system");
				SND_resetAudio(core.sample_rate, core.fps);
			}
			chooseSyncRef();
		}

		if (show_menu) {
			PWR_updateFrequency(PWR_UPDATE_FREQ,1);
			Menu_loop();
			RA_idle();
			PWR_updateFrequency(PWR_UPDATE_FREQ_INGAME,0);
			has_pending_opt_change = config.core.changed;
			chooseSyncRef();
		}

		Audio_checkAndResetIfNeeded();

		hdmimon();
	}

	/* The autosave, and the only place it happens. Quit from the menu, the
	 * power button, or a signal from the launcher all arrive here, so there is
	 * no exit that can forget to save and none that saves twice. Slot 9 is
	 * what the next launch resumes from. */
	State_autosave();

	int cw, ch;
	unsigned char* pixels = GFX_GL_screenCapture(&cw, &ch);

	renderer.dst = pixels;
	SDL_Surface* rawSurface = SDL_CreateRGBSurfaceWithFormatFrom(
		pixels, cw, ch, 32, cw * 4, SDL_PIXELFORMAT_ABGR8888
	);
	/* The same grab, written out as the autosave's preview. The launcher puts
	 * it on the game's card, so a game you have played shows the frame you
	 * stopped on. Free with the fade's capture -- no second screen read. */
	Menu_savePreview(rawSurface, AUTO_RESUME_SLOT);
	SDL_Surface* converted = SDL_ConvertSurfaceFormat(rawSurface, screen->format->format, 0);
	screen = converted;
	SDL_FreeSurface(rawSurface);
	free(pixels);
	GFX_animateSurfaceOpacity(converted, 0, 0, cw, ch, 255, 0, CFG_getMenuTransitions() ? 200 : 20, 1);
	SDL_FreeSurface(converted);
	/* `screen` pointed at that temporary surface and now points at freed
	 * memory. Upstream exited next so nothing noticed; here the caller clears
	 * the screen the moment this returns -- GFX_clear is an SDL_FillRect
	 * straight through the pointer -- and the next game's Menu_init() reads
	 * screen->format. Put the real one back here, at the point it goes
	 * stale, rather than at the top of the next game. */
	if (playos_screen) screen = playos_screen;

	Video_cleanup();

	PLAT_clearTurbo();

	/* Menu_init() allocates a full-screen surface for every game it is called
	 * for, and this is what frees it: skipping it leaks a screen's worth of
	 * memory per launch, which one game per process never had to care about. */
	Menu_quit();
	Perf_setCPUMonitorEnabled(0);

	/* Per-game teardown. The GL context, the pad, the loaded cores and the
	 * settings all stay: that is the entire point. QuitSettings() used to sit
	 * in here and belongs to the process; it moved to resident_quit(). */
	RA_unloadGame();
	RA_quit();
	Notification_quit();
	Game_close();
	Rewind_free();
	Core_unload();
	Core_quit();
	Config_quit();
	Special_quit();
	SND_quit();   /* frees the ALSA device while this process lives on, so the
	               * launcher's own sounds work again; SND_init takes it back */

	memcpy(&slots[slot].core, &core, sizeof core);
	return 0;
}

static void resident_quit(void)
{
	int i;
	QuitSettings();
	for (i = 0; i < slot_count; i++) {
		if (!slots[i].open) continue;
		memcpy(&core, &slots[i].core, sizeof core);
		Core_close();
		slots[i].open = 0;
	}
	MSG_quit();
	PWR_quit();
	VIB_quit();
	SND_removeDeviceWatcher();
	PAD_quit();
	GFX_quit();
	Menu_waitScreenshot();
}

/* A fifo opened for reading alone blocks until a writer appears, and returns
 * EOF the moment the last writer leaves -- which turns a simple request/reply
 * into a race that deadlocks whichever side opens first. Holding a write
 * descriptor of our own removes both behaviours: the read just blocks until a
 * line arrives, for as long as this process lives. Both ends do the same. */
static int playos_fifo_open(const char *path)
{
	unlink(path);
	if (mkfifo(path, 0666) != 0) return -1;
	return open(path, O_RDWR);
}

static bool playos_read_line(int fd, char *out, size_t n)
{
	size_t i = 0;
	char c;
	while (i + 1 < n) {
		ssize_t r = read(fd, &c, 1);
		if (r <= 0) return false;
		if (c == '\n') break;
		out[i++] = c;
	}
	out[i] = 0;
	return true;
}

/* One request per game: "<tag>\t<core path>\t<rom path>". The core path is
 * carried even though the tag is usually enough, so a system the resident was
 * not started with still runs -- it opens the core on the spot and keeps it. */
static int playos_apply_request(char *line, char *rom, size_t rom_n)
{
	char *core_path, *rom_path;

	rom[0] = 0;
	core_path = strchr(line, '\t');
	if (!core_path) return -1;
	*core_path++ = 0;
	rom_path = strchr(core_path, '\t');
	if (!rom_path) return -1;
	*rom_path++ = 0;
	if (!line[0] || !rom_path[0]) return -1;

	snprintf(rom, rom_n, "%s", rom_path);
	return slot_add(line, core_path);
}

static int resident_loop(void)
{
	char line[MAX_PATH * 3], rom[MAX_PATH];
	int fd_req, fd_rep;

	signal(SIGUSR1, playos_end_game);

	{	/* How the launcher reaches this process to end a game. Written
		 * BEFORE the fifos exist, because the fifos are what the launcher
		 * takes as "there is a resident to talk to" -- the other order leaves
		 * a window where it will send a game and then have no way to stop
		 * it, which reads as a black screen with a dead power button. */
		FILE *pf = fopen(PLAYOS_PID, "w");
		if (pf) { fprintf(pf, "%ld\n", (long)getpid()); fclose(pf); }
	}

	fd_req = playos_fifo_open(PLAYOS_REQ);
	fd_rep = playos_fifo_open(PLAYOS_REP);
	if (fd_req < 0 || fd_rep < 0) {
		LOG_error("resident: cannot create fifos\n");
		unlink(PLAYOS_PID);
		return EXIT_FAILURE;
	}

	resident_init();
	LOG_info("boot: resident     %ums (%d cores), waiting for a game\n",
	         SDL_GetTicks(), slot_count);

	for (;;) {
		/* Blocks here between games: no frames, no input, no audio device. */
		if (!playos_read_line(fd_req, line, sizeof line)) break;
		if (!strcmp(line, "exit")) break;
		if (!line[0]) continue;

		playos_req_ms = SDL_GetTicks();
		{
			int slot = playos_apply_request(line, rom, sizeof rom);
			if (slot < 0 || !rom[0]) {
				LOG_error("resident: bad request\n");
			} else {
				LOG_info("resident: %s loading %s\n", slots[slot].tag, rom);
				run_one_game(slot, rom);
			}
		}
		playos_req_ms = 0;

		/* Leave the screen black rather than on the game's last frame: the
		 * launcher draws its own return animation over the top of this. */
		GFX_clearAll();
		GFX_clear(screen);
		GFX_flip(screen);

		if (write(fd_rep, "done\n", 5) != 5)
			LOG_error("resident: reply failed\n");
		LOG_info("resident: idle again, waiting\n");
	}

	resident_quit();
	close(fd_req); close(fd_rep);
	unlink(PLAYOS_REQ); unlink(PLAYOS_REP); unlink(PLAYOS_PID);
	return EXIT_SUCCESS;
}

int main(int argc , char* argv[]) {
	if (argc < 2)
		return EXIT_FAILURE;

	PWR_setCPUSpeed(CPU_SPEED_PERFORMANCE); // start in performance mode for fast loading
	PWR_pinToCores(CPU_CORE_PERFORMANCE); // thread affinity

	/* Resident: minarch.elf --resident TAG=/path/to/core.so ...
	 * Stays up, one game after another, so only the first pays for the GL
	 * context and none after the first pays for a core. */
	if (!strcmp(argv[1], "--resident")) {
		int i;
		for (i = 2; i < argc; i++) {
			char spec[MAX_PATH * 2];
			char *eq;
			snprintf(spec, sizeof spec, "%s", argv[i]);
			eq = strchr(spec, '=');
			if (!eq) { LOG_error("resident: expected TAG=/path/core.so, got %s\n", argv[i]); continue; }
			*eq = 0;
			slot_add(spec, eq + 1);
		}
		if (!slot_count) { LOG_error("resident: no cores given\n"); return EXIT_FAILURE; }
		return resident_loop();
	}

	/* Classic: minarch.elf <core> <rom>, one game per process. Kept because
	 * it is the launcher's fallback when residency is unavailable, and
	 * because it is the only way to run a game with the launcher gone. */
	if (argc < 3) return EXIT_FAILURE;
	{
		char tag[MAX_PATH];
		const char *forced = getenv("PLAYOS_TAG");
		int slot;

		if (forced && *forced) snprintf(tag, sizeof tag, "%s", forced);
		else getEmuName(argv[2], tag);
		LOG_info("rom_path: %s\n", argv[2]);

		slot = slot_add(tag, argv[1]);
		resident_init();
		{
			char rom[MAX_PATH];
			snprintf(rom, sizeof rom, "%s", argv[2]);
			run_one_game(slot, rom);
		}
		resident_quit();
	}
	return EXIT_SUCCESS;
}
