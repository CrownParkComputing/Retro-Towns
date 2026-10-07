/* Retro-Towns - turning a compressed disc into one the core can mount.
 *
 * Tsugaru reads a .cue, a .bin, an .iso and a handful of other sheet formats,
 * and it has no zip or CHD reader anywhere in src/ - the GUI it came with does
 * the unpacking, and this app deliberately does not have that GUI.  So an
 * archive has to be exploded onto disk before it can go into the drive.
 *
 * The result goes in the library's own cd/ folder, in a folder named after the
 * game, because that is where a player looks for it and where a re-scan finds
 * it as an ordinary disc.  A private cache directory would work equally well
 * for the machine and leaves the player with a library they cannot read.
 */
#ifndef TOWNS_STAGE_H
#define TOWNS_STAGE_H

#include <cstdint>
#include <functional>
#include <string>

namespace towns {

/* Bytes written so far, out of total (0 when the archive did not say). */
using StageProgress = std::function <void (int64_t done,int64_t total)>;

/*
 * Unpack [image] (a .zip or .chd) into a folder of its own inside [cd_dir] and
 * return the sheet to mount.  Empty with [err] filled in means it did not work.
 *
 * Already unpacked is not unpacked again: the folder is looked at first, and a
 * sheet in it is returned as it stands, so replaying a game costs nothing.
 */
std::string stage_image(const std::string &image,const std::string &cd_dir,
                        std::string &err,
                        const StageProgress &progress=StageProgress());

/* The folder an image's own contents go into - cd_dir plus the file's name
 * without its extension.  Exposed because the shelf has to be able to say
 * where it put something before it has been put there. */
std::string stage_dir_for(const std::string &image,const std::string &cd_dir);

/* True when [image] is already sitting in [cd_dir] as a disc.  The shelf uses
 * it to hide the archive once the game it came from is on the shelf, so that
 * unpacking Turbo OutRun does not leave two of it behind. */
bool is_staged(const std::string &image,const std::string &cd_dir);

/* The floppy image (.d77/.d88/.xdf) a game folder holds, or empty.  A boot or
 * user disk rides beside the CD and must be in FD0 before the CD is mounted,
 * because the machine boots the floppy and the floppy then hands over to the
 * disc. */
std::string best_floppy(const std::string &dir);

} /* namespace towns */
#endif /* TOWNS_STAGE_H */
