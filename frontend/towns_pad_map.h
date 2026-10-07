/* towns_pad_map.h -- what a control on a real pad means to the TOWNS.
 *
 * The FM TOWNS game port carries nine signals: four directions, A and B, and
 * the three the machine read off the controller alongside them - RUN, PAUSE and
 * ZOOM. That is the whole vocabulary, and it is much smaller than the pad
 * plugged into the front end, so somebody has to say which of the player's
 * controls becomes which of the machine's. This is that decision, stored as
 * data rather than as a table in the main loop.
 *
 * A binding is kept as an SDL control *name* - "south", "leftshoulder",
 * "return" - not an index. Two reasons. The config file stays readable and
 * fixable in a text editor, and a binding survives the pad it was made on
 * being unplugged: it names a control, not a device handle that goes stale
 * between runs.
 */
#ifndef TOWNS_PAD_MAP_H
#define TOWNS_PAD_MAP_H

#include <SDL3/SDL.h>
#include <imgui.h>

#include <array>
#include <string>

#include "ftowns_bridge.h"

namespace padmap {

enum Signal {
    SIG_UP, SIG_DOWN, SIG_LEFT, SIG_RIGHT,
    SIG_A, SIG_B,
    SIG_RUN, SIG_PAUSE, SIG_ZOOM,
    SIG_COUNT
};

/* The UI label, the key it persists under, and the bit the bridge is told
 * about, in one table so the three cannot drift apart. */
struct Entry {
    const char *label;
    const char *key;
    const char *meaning;   /* what games usually do with it */
    unsigned int bit;
};
extern const Entry kEntries[SIG_COUNT];

struct Map {
    std::array <std::string,SIG_COUNT> bind;  /* empty = unbound */

    /* The bindings that make sense out of the box: the two face buttons are
     * A and B, START is RUN because that is the line the machine treats as
     * "go", and the stick and D-pad both drive the directions. */
    static Map defaults(void);

    /* Which of the machine's signals this pad is currently holding.  Not
     * const on the gamepad: that is how SDL reads a button. */
    unsigned int mask(SDL_Gamepad *pad) const;
    /* Which signal a keyboard key is bound to, or -1 for none. */
    int signal_for_key(SDL_Scancode sc) const;

    /* "South (B)" for the UI, "Unbound" when there is nothing there. */
    std::string describe(int sig) const;
    /* Names a control the way SDL spells it, for the capture dialog. */
    static std::string name_of(const SDL_Event &ev);

    /* True when `text` is a control this app can bind.  Checked on the way
     * in, because the file is hand-editable and an unparseable binding must
     * read as "unbound" rather than as whatever index happens to be zero. */
    static bool valid(const std::string &text);
};

/* Take a control name from an SDL event for whichever signal is waiting for
 * one.  Called from the event pump, so it must not touch the interface:
 * `armed` is the signal being captured, or -1, and it is cleared here when the
 * press arrives.  Returns true when the map changed. */
bool capture(Map &map,const SDL_Event &ev,int &armed);

/* The settings page: a row per signal with its current binding and the
 * buttons that arm a capture.  Returns true when the map changed. */
bool editor(Map &map,int &armed);

/* The picture on the settings page.  `armed` gets a ring around it. */
void draw_controller(ImDrawList *dl,const ImVec2 &pos,const ImVec2 &size,
                     const Map &map,int armed);

} /* namespace padmap */

#endif /* TOWNS_PAD_MAP_H */
