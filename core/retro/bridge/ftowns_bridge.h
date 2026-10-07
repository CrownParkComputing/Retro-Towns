/* Retro-Towns - the C ABI between the Tsugaru core and the front end.
 *
 * The front end must not see C++.  Tsugaru is a template-heavy codebase
 * (FMTownsTemplate<CPUCLASS>) whose headers pull in the whole emulator, so
 * every call across this boundary is plain C over a handful of opaque
 * integers.  It also keeps the two halves compilable on different toolchains,
 * which is what lets the same bridge serve the Linux build and the NDK build.
 *
 * Threading, because it is the part that will bite:
 *
 *   VM thread     owned by the bridge.  Runs TownsThread::VMMainLoop.  Calls
 *                 Outside_World::DevicePolling and every Sound method.
 *   UI  thread    owned by the bridge.  Runs TownsUIThread, which is where
 *                 commands are executed against a live VM.
 *   Window thread IS the front end's main loop.  ftowns_pump() is what the
 *                 front end calls once per SDL frame to do the work upstream
 *                 labels "Called from the Window thread".  No fourth thread.
 *   Audio thread  whatever the host OS calls back on (SDL on Linux, AAudio on
 *                 Android).  Reads the mixer; never writes VM state.
 *
 * Nothing here is safe to call from more than one thread except ftowns_read_audio,
 * which exists precisely to be called from the audio callback.
 */
#ifndef FTOWNS_BRIDGE_H
#define FTOWNS_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- return codes ---- */
enum {
	FTOWNS_OK=0,
	FTOWNS_ERR_BAD_ARG=-1,
	FTOWNS_ERR_ALREADY_RUNNING=-2,
	FTOWNS_ERR_NOT_RUNNING=-3,
	FTOWNS_ERR_SETUP=-4       /* ROMs missing, bad disc image, ... */
};

/* ---- VM state, mirroring TownsThread::RUNMODE_* ----
 * Values are translated in the bridge rather than leaked from townsthread.h.
 */
enum {
	FTOWNS_VM_IDLE=0,
	FTOWNS_VM_POWER_OFF=1,
	FTOWNS_VM_PAUSE=2,
	FTOWNS_VM_RUN=3,
	FTOWNS_VM_EXIT=4
};

/* ---- audio format, fixed ----
 * The core produces 44100 Hz signed-16-bit stereo for every channel it has
 * (YM2612 WAVE_SAMPLING_RATE, the buzzer's BUZZER_SAMPLING_RATE, and CD-DA),
 * so the bridge does not resample and neither does the front end.
 */
#define FTOWNS_AUDIO_RATE 44100
#define FTOWNS_AUDIO_CHANNELS 2

/* ---- game-pad buttons ----
 * The FM TOWNS pad had A, B, and eight directions, and games borrowed three
 * more signals for RUN, PAUSE and ZOOM.  There is no third button.
 */
enum {
	FTOWNS_PAD_A     =1u<<0,
	FTOWNS_PAD_B     =1u<<1,
	FTOWNS_PAD_LEFT  =1u<<2,
	FTOWNS_PAD_RIGHT =1u<<3,
	FTOWNS_PAD_UP    =1u<<4,
	FTOWNS_PAD_DOWN  =1u<<5,
	FTOWNS_PAD_RUN   =1u<<6,
	FTOWNS_PAD_PAUSE =1u<<7,
	FTOWNS_PAD_ZOOM  =1u<<8
};

/* Cyber Stick trigger bits, for the port configured as a Cyber Stick. */
enum {
	FTOWNS_TRIG_ONE=1u<<0,
	FTOWNS_TRIG_TWO=1u<<1,
	FTOWNS_TRIG_THREE=1u<<2
};

/* What is plugged into a game port.  A short list on purpose: these are the
 * devices the front end can actually offer, and the bridge maps each onto the
 * TOWNS_GAMEPORTEMU_* the core expects.
 */
enum {
	FTOWNS_PORT_NONE=0,
	FTOWNS_PORT_PAD,        /* physical pad, port 0 by default               */
	FTOWNS_PORT_MOUSE,      /* physical mouse, port 1 by default             */
	FTOWNS_PORT_CYBERSTICK,
	FTOWNS_PORT_ANALOG_PAD
};

/* One frame of host input.  Level state, not events - the core polls. */
typedef struct ftowns_input {
	uint32_t pad[2];              /* FTOWNS_PAD_* bitmask, per TOWNS port    */

	int stick[2][4];              /* Cyber/analog axes, -1000..1000          */
	uint32_t trigger;             /* FTOWNS_TRIG_*                           */

	uint32_t mouseButton;         /* 1=left 2=right 4=middle                 */
	int mouseX,mouseY;            /* absolute, in TOWNS screen pixels        */
	int mouseRelX,mouseRelY;      /* differential, for games that want it    */

	int padAnalog[2][2];          /* analog pad X/Y, -1000..1000             */
} ftowns_input;

/* ---- lifecycle ---- */

/* Start the VM.
 *
 * argv is the Tsugaru command line, verbatim: argv[0] is a program name that
 * is ignored, argv[1] must be the ROM directory (Tsugaru takes it as
 * ROMPath, which is how the core finds FMT_SYS.ROM and friends), and the rest
 * are the usual -CD/-FD0/-SCALE/-MOUSE options the core already understands.
 *
 * Passing the argument vector rather than a struct is deliberate.  The core
 * has years of machine configuration behind TownsARGV, and re-describing it
 * field by field here would only mean the front end supports the subset that
 * got retyped.  The wizard builds an argv; nothing in between needs to know
 * what a machine is made of.
 *
 * Returns FTOWNS_OK, or FTOWNS_ERR_SETUP with the core's own reason already
 * written to the log (see ftowns_set_log_callback).
 */
int ftowns_start(int argc,const char *const *argv);

/* Ask for a clean shutdown and wait for the VM thread to finish.
 * Safe to call when nothing is running. */
void ftowns_stop(void);

int ftowns_is_running(void);
int ftowns_vm_state(void);

/* Why the machine is where it is, as one line of text, for the log.  A core
 * that will not run says so with a run-mode number and nothing else; this
 * adds the stop flags, the device that aborted, and the power-off state.
 * Returns the number of characters written, excluding the terminator. */
unsigned int ftowns_debug_info(char *buf,unsigned int cap);

/* Run / pause / power-off.  These enqueue; they do not touch the VM from the
 * calling thread. */
void ftowns_set_run_mode(int mode);

/* ---- the window thread ---- */

/* Do one round of window-thread work: move the newest rendered image out of
 * the core, run the base interval, and publish.  Call once per front-end
 * frame. */
void ftowns_pump(void);

/* ---- video ---- */

/* Newest frame, RGBA8, top-down, tightly packed at *width* pixels.
 * Returns 1 when a frame newer than the last returned one is available.
 * The pointer is valid until the next ftowns_pump() on this thread. */
int ftowns_poll_frame(unsigned int *width,unsigned int *height,const uint8_t **rgba);

/* Current TOWNS output resolution, which changes when a game switches modes -
 * the front end has to resize its texture, so this is asked for every frame. */
void ftowns_screen_size(unsigned int *width,unsigned int *height);

/* ---- audio ---- */

/* Fill out[] with exactly `frames` interleaved signed-16 stereo frames.
 * Silences whatever no channel is currently playing.  Called from the host
 * audio callback; takes one short mutex and does no allocation.
 *
 * Returns the number of frames written, which is `frames` unless the VM has
 * stopped.
 */
int ftowns_read_audio(int16_t *out,int frames);

/* Drop every pending sample.  Called on pause and on reset so that a resumed
 * VM does not first play the seconds of audio it queued while paused. */
void ftowns_audio_reset(void);

/* How much of what the host pulled was real audio rather than the queue running
 * dry.  `covered` reaching `pulled` means the core kept the queue fed; a
 * shortfall is a buzz, so a front end can warn on it instead of waiting for
 * someone to notice.  Cumulative since ftowns_start.
 *
 * `dropped` counts samples thrown away to keep the queue inside its limit - a
 * note that never plays - and `peak_queued` is the deepest the queue ever got,
 * in frames.  Together they say which way the host and the core are out of step:
 * dry is too slow a core, dropping is too fast a core.  Any argument may be
 * NULL. */
void ftowns_audio_stats(long long *pulled,long long *covered,long long *dropped,long long *peak_queued);

/* ---- input ---- */

/* Publish the level state.  Cheap; call every front-end frame. */
void ftowns_set_input(const ftowns_input *state);

/* Keyboard is event-shaped, not level-shaped, because that is what the TOWNS
 * keyboard controller is: make and break codes for JIS keys.  `jisKey` is a
 * TOWNS_JISKEY_* value; the front end owns the host-key to JIS table.
 *
 * `shift` says the shift key was down for this key, which is how the core's own
 * character translator marks it too - a real TOWNS reports `:` as `;` with the
 * shift bit on, not as a different key, so TBIOS is what turns it into `:`. */
int ftowns_key(int jisKey,int down,int shift);

/* Configure a game port.  Takes effect at the next DevicePolling. */
void ftowns_set_game_port(int port,int device);

/* ---- commands ---- */

/* Queue a Tsugaru command for the UI thread.  `cmd` is a bare command word
 * with no leading '!' - that bang is a CUI typing convention, not part of the
 * command: "RUN", "PAUSE", "POFF", "QUIT", "RESET", "SAVESTATE path",
 * "LOADSTATE path", "CD path", "TYPE ...".
 *
 * This is the escape hatch for anything the bridge has no dedicated call for,
 * and it is how savestates and disc swaps happen without a second
 * implementation of each. */
void ftowns_command(const char *cmd);

/* ---- diagnostics ---- */

/* The core reports problems by printing to stdout.  A front end with no
 * terminal needs those words.  Set once before ftowns_start; callback runs on
 * whichever thread hit the problem, so it must only append to a log. */
typedef void (*ftowns_log_fn)(const char *line,void *ctx);
void ftowns_set_log_callback(ftowns_log_fn fn,void *ctx);

#ifdef __cplusplus
}
#endif

/* } */
#endif
