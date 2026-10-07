/* Retro-Towns - the disc shelf.
 *
 * An FM TOWNS game is an IMAGE, not a folder you run: there is no executable to
 * scan for and no "which of these five programs did you mean" - there is a disc,
 * and you put it in the drive.  The folder shape underneath is packaging, which
 * is why a .cue and its .bin are one entry rather than two games, and why a
 * game that ships as a folder of tracks is still one entry.
 *
 * Two things differ from the CD-only machines in the rest of the estate. The
 * TOWNS also booted floppies, so a shelf that only looks for CD images misses
 * a third of the library; and a great many titles are multi-disc, where a
 * shelf that lists "Title (Disc 1)" through "(Disc 4)" as four unrelated games
 * has misunderstood the collection rather than the hardware.
 *
 * What is deliberately NOT here: hard-disk images (.HDI) and the TOWNS' own
 * .TOWNS profile files. A hard disk is not a game you pick from a shelf, it is
 * something a game installs itself onto, and offering it beside a disc would
 * invite players to mount a half-installed system as if it were a title.
 */
#ifndef TOWNS_LIBRARY_H
#define TOWNS_LIBRARY_H

#include <string>
#include <vector>

namespace towns {

/* Which drive an image goes into.  The core takes a CD and a floppy on
 * different command-line switches, so this decides more than a label. */
enum class Media
{
	Cd,
	Floppy,
};

const char *media_name(Media m);

/* One image on disk. */
struct Disc
{
	std::string path;       /* what was found - an image, or an archive holding one */
	std::string file;       /* the file's own name, for when the title is bare */
	int         number=0;   /* 1-based disc number, 0 when the name says none  */
	Media       media=Media::Cd;

	/* When the disc lives behind the Storage Access Framework, these name the
	 * tree and the file inside it; empty for a filesystem path. */
	std::string saf_uri;
	std::string saf_sub;    /* "", "cd", "chd" or "zip" */
	std::string saf_name;
	bool saf_backed() const { return !saf_uri.empty(); }
};

/* One game: its title, and every disc that belongs to it. */
struct Game
{
	std::string title;      /* the shared part of the name, cleaned up         */
	std::vector <Disc> discs;
	char initial='#';
	Media media=Media::Cd;

	const Disc *disc(size_t i) const
	{
		return i<discs.size() ? &discs[i] : nullptr;
	}
	bool multi() const { return 1<discs.size(); }
};

/*
 * Everything under a LIBRARY PARENT, grouped into games.
 *
 * The parent is the one folder the app is given - on Android it is the one
 * folder the Storage Access Framework will grant - and inside it the
 * collection is expected to be sorted:
 *
 *     cd/     one folder per game, each holding the .cue and .bin it mounts,
 *             or a loose .cue/.iso/.ccd/.mds/.d77
 *     chd/    .chd rips, which the core cannot open at all
 *     zip/    zipped rips, same story
 *
 * What comes out of chd/ and zip/ goes into cd/ as a folder of its own, which
 * is why a game that has been played once shows up twice on the shelf if the
 * player also left the unpacked copy lying in the parent - the app does not
 * delete the archive it unpacked, but it does not hide the result either.
 *
 * Loose images directly in the parent are read too, because that is how a
 * single-folder collection looks before anybody has sorted it.
 *
 * Recursion stops one level inside each of those. Going deeper turns the
 * tracks beside a .cue, or a game's own extracted files, into library entries
 * of their own - which is the failure mode this whole file is written to
 * avoid.
 */
std::vector <Game> scan_library(const std::string &parent);

/* The same shelf, read from a Storage Access Framework tree (content://) that
 * has no filesystem path.  Sees the flat images and archives in the parent,
 * cd/, chd/ and zip/; a game that ships as a folder of tracks is not reached
 * here, because SAF lists files one level at a time. */
std::vector <Game> scan_library_saf(const std::string &uri);

/* True when the core cannot mount this path itself and it has to be unpacked
 * first - a .zip, or a .chd, which Tsugaru has no reader for at all. */
bool is_archived_image(const std::string &filename);

/*
 * The disc number a filename declares, and the title with that declaration
 * removed.  Handles the forms collections actually use:
 *
 *     Title (Disc 2)     Title (Disc 2 of 4)     Title (CD2)
 *     Title [Disc 2]     Title - Disc 2          Title (Disk 2)
 *
 * Returns 0 and leaves [title] alone when the name claims no disc number,
 * which is the common case and must not be mangled.
 */
int disc_number_of(const std::string &filename,std::string &title);

/* True when the extension is one the machine can boot from, or one that holds
 * something it can boot from. */
bool is_disc_image(const std::string &filename);

/* Which drive the image with this name belongs in. */
Media media_of(const std::string &filename);

} /* namespace towns */
#endif /* TOWNS_LIBRARY_H */
