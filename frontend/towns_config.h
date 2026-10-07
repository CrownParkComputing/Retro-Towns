/* Retro-Towns - persisted settings.
 *
 * Plain key=value text, the same shape the rest of this estate uses: small,
 * fixable in a text editor when something will not start, and it survives an
 * app update with no migration code.
 *
 * What is here is what an FM TOWNS needs and a Saturn does not. The BIOS is a
 * DIRECTORY of six files rather than one image, because that is how Fujitsu
 * shipped them and how the core loads them. There is a keyboard, because the
 * machine is a home computer and a great many titles are typed at rather than
 * padded. And the two game ports hold a pad or a mouse or a Cyber Stick, which
 * changes what a game will even let you do.
 */
#ifndef TOWNS_CONFIG_H
#define TOWNS_CONFIG_H

#include <string>

#include "towns_pad_map.h"

namespace towns {

/* Bumped whenever the first-run wizard's requirements change - for example when
 * the set of folders it creates changes. A saved app whose setup_version is
 * older than this is asked to confirm its library folder once more on the next
 * launch, because the folders the wizard expects may have changed underneath it. */
inline constexpr int kSetupVersion = 1;

/* Mirrors FTOWNS_PORT_* so the UI can hold one without dragging the bridge
 * header into every file. The values match; the bridge is the authority. */
enum class PortDevice {
    None = 0,
    Pad = 1,
    Mouse = 2,
    CyberStick = 3,
    AnalogPad = 4,
};

const char *port_device_name(PortDevice d);

/*
 * The machine types the core names, in the order the machine page lists them.
 *
 * The blank row is not a model, it means "say nothing" and leave the choice to
 * the core.  Eighteen codes is a lot of letters to choose between, so each row
 * says which generation it belongs to - the core's own grouping, from the
 * declarations behind TownsTypeToStr - and nothing else it might get wrong.
 */
struct TownsModel
{
    const char *name;
    const char *what;
};
extern const TownsModel kTownsModels[];
extern const int kTownsModelCount;

/* True for a name the core will take, and for the empty string. */
bool is_towns_model(const std::string &name);

struct Settings {
    /* ---- the machine ----
     *
     * Defaults are the real hardware's behaviour, not the fastest or the
     * prettiest.
     */
    /* ---- which machine ----
     *
     * An FM TOWNS was sold as roughly eighteen models over eight years, and
     * they are not the same computer: the graphics chip decides whether there
     * is 1MB or 4MB of VRAM, only some shipped a CD drive, and a Marty is not a
     * Model 2 with the cover off.  A title that wants a machine it is not
     * running on does not explain itself - it shows a black screen or refuses
     * the disc - so this is the first thing a player reaches for when a game
     * will not boot.
     *
     * The strings are the core's own vocabulary (TownsTypeToStr), and 0 for a
     * number means "say nothing and let the model decide", which is what the
     * core does with an argument it was not given.
     */
    /* "" = the core's default.  R50 .. MX .. MARTY. */
    std::string towns_type;
    /* CPU clock in MHz.  16 is a Model 1, 40 the later 486 machines. */
    int    cpu_freq=0;
    /* RAM in megabytes.  1, 2, 3, 4, or 5 with an expansion. */
    int    mem_size=0;
    /* Report an i486 as a 386DX.  Some titles check the CPU and misbehave when
     * they find something faster than the box they shipped for. */
    bool   pretend_386dx=false;
    /* The accurate 80386 core. Considerably slower, and some titles only run
     * correctly on it, so it is a choice rather than a tuning knob. */
    bool   high_fidelity=false;
    /* JP keymap for the keys typed into the machine, as opposed to the keys
     * the front end itself answers to. */
    bool   keyboard_jp=false;
    /* 0 = 640x400-ish, 1 = 640x480, 2 = 1024x1024. A hint to the front end
     * about how big to make the window, since the TOWNS changes resolution
     * whenever a program feels like it. */
    int    preferred_mode=0;

    /* ---- video ---- */
    /* How much of the window the picture takes: 0 fills the window, 1..4 is
     * that many screen pixels for every pixel the TOWNS drew.  A 640x480 game
     * in a 1500x950 window is otherwise a postage stamp, and the number is
     * whole because a half-scaled 8x8 font turns to mush. */
    int    scaling=0;
    bool   vsync=true;

    /* ---- audio ---- */
    bool   audio_muted=false;
    /* Host buffer in milliseconds. The core emits 40 ms at a time, so a small
     * figure here only helps if the core is kept ahead of it. */
    int    audio_latency_ms=80;

    /* ---- controllers ---- */
    PortDevice port1=PortDevice::Pad;
    PortDevice port2=PortDevice::Mouse;
    /* Which control on the pad means which of the nine signals the game port
     * carries. Editable, because the hardware the player owns is not the
     * hardware the machine expects. */
    padmap::Map pad=padmap::Map::defaults();
    /* Fraction of full travel before the left stick counts as a direction.
     * SDL hands back whatever the joystick driver reported, unfiltered, and a
     * stick at rest is not reliably zero - the one this was measured on sits
     * around -700. */
    float stick_deadzone=0.35f;
    std::string touch_pad="ftowns";
    /* 0 on a touchscreen, 1 always, 2 never - the same three the rest of the
     * estate offers, so a player who has set up one of these apps finds the
     * control where they left it. */
    int    touch_pad_show=1;
};

struct AppConfig {
    /*
     * The ONE folder this app is given, and the only path stored here.
     *
     * Everything else is inside it - bios/, cd/, chd/ - because Android grants
     * a single directory at a time and a wizard that asked for three would be
     * asking the player to repeat themselves three times for no reason.  On a
     * desktop the same shape is simply what a tidy collection looks like, so
     * there is one arrangement to explain rather than one per platform.
     *
     * Empty until the wizard has been through: the machine cannot start without
     * Fujitsu's BIOS, and this app cannot ship it.
     */
    std::string library_root;
    /* Savestates and CMOS. Empty means "beside the settings file". */
    std::string saves_dir;
    /* Last image played, so reopening the app resumes a shelf position rather
     * than a VM - the machine is powered off, the intention is not. */
    std::string last_disc;

    /* The wizard version this library folder was last confirmed under.  Older
     * than kSetupVersion means "ask again after an update". */
    int setup_version = 0;

    Settings machine;
};

/* False when the file did not exist or was empty, i.e. a first run. */
bool load_app_config(const std::string &path, AppConfig &out);
bool save_app_config(const std::string &path, const AppConfig &cfg);

} /* namespace towns */
#endif /* TOWNS_CONFIG_H */
