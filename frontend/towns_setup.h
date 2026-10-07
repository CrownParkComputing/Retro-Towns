/*
 * Retro-Towns — first run, and where things live.
 *
 * Two facts make this file necessary, and neither is interesting on a desktop:
 *
 * AN FM TOWNS NEEDS FUJITSU'S BIOS, and it is six files rather than one. The
 * app cannot ship them, cannot fetch them, and will not start without them, so
 * the first conversation with every new player is "go and find these files".
 * That deserves a screen with a checklist on it rather than an error box,
 * because a checklist says which of the six are missing and an error box says
 * that something is wrong.
 *
 * AND ANDROID DOES NOT GIVE OUT FOLDERS. It grants ONE directory at a time,
 * through the system picker, and only the one that was chosen. So the app asks
 * for a single PARENT and expects this shape inside it:
 *
 *     <parent>/
 *         bios/     FMT_SYS.ROM and the rest
 *         cd/       one folder per game, each holding a .cue and a .bin,
 *                   or a loose .cue/.iso/.ccd/.mds
 *         chd/      .chd rips
 *
 * One grant, one folder to keep tidy. The sub-folders are matched by
 * `folder_for` rather than by exact name, because a library somebody has
 * maintained for twenty years is not named what this app would have named it.
 */
#ifndef TOWNS_SETUP_H
#define TOWNS_SETUP_H

#include <string>
#include <vector>

namespace towns {

/* ---- the BIOS ---- */

enum {
    ROM_MUST=2,   /* without this the machine has nothing to execute        */
    ROM_SHOULD=1, /* part of a complete dump; a game may want what is in it  */
    ROM_MAY=0     /* only matters for a particular model                     */
};

struct RomNeed {
    const char *name;
    long long size;    /* the reference dump's, in bytes                     */
    int need;
    const char *what;  /* one line, for the player to read off the screen    */
};

const std::vector <RomNeed> &rom_needs();

struct RomRow {
    RomNeed need;
    long long found=0;
    /* 0 absent, 1 present but not the size it should be, 2 present and right */
    int state=0;
};

struct BiosCheck {
    std::string dir;
    /* False when [dir] is not a folder at all - a different thing to say than
     * "a folder with the wrong files in it", and the player needs to know
     * which of the two they are looking at. */
    bool exists=false;
    std::vector <RomRow> rows;
    bool ok=false;
    /* True when FMT_ALL.ROM stands in for the separate files. */
    bool combined=false;
    int missing=0;   /* count of ROM_MUST rows that are not state 2 */
};

/* How one candidate folder scores. An empty or absent dir yields a check with
 * nothing in it rather than a failure, because it is drawn as a checklist. */
BiosCheck check_bios(const std::string &dir);

/* ---- where things are ---- */

/*
 * The folder inside [root] that plainly holds [kind], whatever it is called.
 *
 * "bios" also answers to "ROM", "BIOS", "firmware" and "TownsROM"; "cd" to
 * "Games", "roms", "discs" and friends.  A candidate that actually holds
 * something beats one that merely exists, because this app creates the folders
 * it expects when it finds none, and an empty "cd" made on a previous run must
 * not shadow the "Games" folder with ninety images in it for ever.
 *
 * Returns root+"/"+kind when there is nothing, which is then the one to create.
 */
std::string folder_for(const std::string &root,const std::string &kind);

/* Like folder_for, but empty when nothing matching exists yet.  folder_for
 * invents root/<kind> in that case; the wizard needs to know whether the folder
 * is a real one it found or a path it will have to create. */
std::string existing_folder_for(const std::string &root,const std::string &kind);

/* Make the three folders the wizard promises - bios/, cd/ and zip/ - under
 * [root] when they are not there.  An existing folder that plainly holds that
 * kind (for example "Games" for cd) is left alone rather than shadowed by an
 * empty one.  Returns true when the layout is in place, whether it made it or
 * it was already there. */
bool ensure_layout(const std::string &root);

/* The BIOS inside a parent folder - or the parent itself, for somebody whose
 * whole library is the ROM folder. Empty when neither holds a usable set. */
std::string find_bios(const std::string &root);

/*
 * The folder to draw a checklist FOR, usable or not.
 *
 * `find_bios` answers only when the machine can start, which is the right
 * question for the launcher and the wrong one for the wizard: a dump missing
 * one file should be reported as that file, not as "no BIOS found".
 */
std::string bios_candidate(const std::string &root);

/* The folders this platform will let the app read, most useful first. On a
 * desktop these are suggestions; on Android they are the app's own directories,
 * which need no grant at all. */
std::vector <std::string> candidate_roots();

/* ---- what the wizard found ---- */

/* One of the three folders the wizard cares about, as it stands on disk. */
struct FolderReport {
    std::string kind;   /* "bios", "cd" or "zip"                           */
    std::string path;   /* the folder the app will actually use            */
    bool exists = false;
    int files = 0;      /* files directly inside                           */
    int dirs = 0;       /* subfolders directly inside                      */
};

struct LayoutReport {
    std::string root;
    bool root_exists = false;
    std::vector <FolderReport> folders;   /* bios, cd, zip, in that order */
};

/* The three wizard folders, with a one-level count of what each holds.  Cheap
 * enough to run every frame the wizard is showing the review step. */
LayoutReport scan_layout(const std::string &root);

/* ---- the system folder picker ----
 *
 * ASYNCHRONOUS, and it has to be.  SDL's dialogs return at once and call back
 * later, on the main thread; anything that blocked waiting for the answer
 * stopped drawing, stopped answering its own close button, and was reported by
 * the desktop as not responding.  Nothing here may block the loop.
 *
 * On Android the picker is the Storage Access Framework, and the answer is a
 * content:// handle rather than a path - which is why `pickers_usable` says no
 * there and the wizard takes a different route instead of showing a button
 * that appears to do nothing.
 */
void begin_pick_folder();
bool pickers_usable();
bool pick_in_progress();
/* Takes the answer, once.  False when no dialog has finished since the last
 * call, or the player cancelled - neither overwrites what was already set. */
bool take_pick(std::string &out);

/* ---- Android Storage Access Framework (SAF) ---- */
bool saf_available();
struct SafTree {
    std::string uri;
    std::string name;
    std::string path;
};
void saf_pick();
std::vector<SafTree> saf_trees();
void saf_forget(const std::string &uri);
bool saf_ensure_layout(const std::string &uri);
std::string saf_folder_name(const std::string &uri, const std::string &kind);

struct SafEntry { std::string name; long long bytes = 0; };
std::vector<SafEntry> saf_list(const std::string &uri, const std::string &sub);
std::string saf_stage(const std::string &uri, const std::string &sub,
                      const std::string &name, const std::string &dest_dir);
int saf_stage_progress();

} /* namespace towns */

#endif /* TOWNS_SETUP_H */
