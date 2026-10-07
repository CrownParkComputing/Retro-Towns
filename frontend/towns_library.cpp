/* towns_library.cpp -- see towns_library.h. */

#include "towns_library.h"
#include "towns_setup.h"
#include "towns_stage.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace towns {

namespace {

bool ends_with_ci(const std::string &s,const char *suffix)
{
	const size_t n=SDL_strlen(suffix);
	if(s.size()<n)
	{
		return false;
	}
	return 0==SDL_strncasecmp(s.c_str()+s.size()-n,suffix,n);
}

std::string trim(const std::string &s)
{
	size_t b=0,e=s.size();
	while(b<e && (isspace((unsigned char)s[b]) || '-'==s[b] || '_'==s[b]))
	{
		++b;
	}
	while(e>b && (isspace((unsigned char)s[e-1]) || '-'==s[e-1] || '_'==s[e-1]))
	{
		--e;
	}
	return s.substr(b,e-b);
}

std::string strip_extension(const std::string &name)
{
	const size_t dot=name.find_last_of('.');
	return std::string::npos==dot ? name : name.substr(0,dot);
}

/* Case-insensitive search, returning the position or npos. */
size_t find_ci(const std::string &hay,const char *needle,size_t from=0)
{
	if(from>=hay.size())
	{
		return std::string::npos;
	}
	const char *p=SDL_strcasestr(hay.c_str()+from,needle);
	return nullptr!=p ? (size_t)(p-hay.c_str()) : std::string::npos;
}

} /* namespace */

namespace {

/* What a folder of files is mounted as, best first.  A .cue and its .bin are
 * one game and the .cue is the one that says where the tracks are and what
 * sector format they use, so it wins; a bare .iso or .chd is a whole disc and
 * needs no sheet. */
const char *const kSheetExt[]={
	".cue",".ccd",".mds",".chd",".zip",".iso",".d77",".img",".mdf",nullptr };

/* One game's worth of shelf entries from a path, titled by `title` rather than
 * by the filename when the filename is something useless like "disc1.cue". */
void add_image(std::map <std::string,Game> &byTitle,const std::string &path,
               const std::string &name,std::string title,int number)
{
	if(title.empty())
	{
		title=name;
	}
	Disc d;
	d.path  =path;
	d.file  =name;
	d.number=number;
	d.media =media_of(name);

	Game &g=byTitle[title];
	if(g.title.empty())
	{
		g.title=title;
		const char c=(char)SDL_toupper((unsigned char)title[0]);
		g.initial=('A'<=c && c<='Z') ? c : '#';
		g.media=d.media;
	}
	g.discs.push_back(std::move(d));
}

/* The sheet inside a game folder, or empty when the folder holds none. */
std::string sheet_in(const std::string &folder)
{
	int n=0;
	char **found=SDL_GlobDirectory(folder.c_str(),nullptr,SDL_GLOB_CASEINSENSITIVE,&n);
	std::string best;
	int bestRank=-1;
	for(int i=0; nullptr!=found && i<n && nullptr!=found[i]; ++i)
	{
		const std::string name=found[i];
		if(nullptr!=SDL_strchr(name.c_str(),'/'))
		{
			continue;                       /* one level only */
		}
		for(int r=0; nullptr!=kSheetExt[r]; ++r)
		{
			if(ends_with_ci(name,kSheetExt[r]) && (0>bestRank || r<bestRank))
			{
				bestRank=r;
				best=folder+"/"+name;
			}
		}
	}
	if(found)
	{
		SDL_free(found);
	}
	return best;
}

/* Everything one folder declares, added to `byTitle`.  A directory entry is a
 * game whose files are in it; a file entry is an image, or an archive holding
 * one.
 *
 * [skip] names folders that are library, not title: the parent of a sorted
 * collection holds "cd" and "chd", and treating those as games put the same
 * disc on the shelf twice, once under the name of the folder it lives in. */
bool scan_into(const std::string &dir,std::map <std::string,Game> &byTitle,
               const std::set <std::string> &skip)
{
	int n=0;
	char **found=SDL_GlobDirectory(dir.c_str(),nullptr,SDL_GLOB_CASEINSENSITIVE,&n);
	if(nullptr==found)
	{
		return false;
	}
	for(int i=0; i<n && nullptr!=found[i]; ++i)
	{
		const std::string name=found[i];
		if(nullptr!=SDL_strchr(name.c_str(),'/'))
		{
			continue;                       /* one level only */
		}
		const std::string path=dir+"/"+name;
		if(skip.count(path))
		{
			continue;
		}

		SDL_PathInfo info;
		if(SDL_GetPathInfo(path.c_str(),&info) &&
		   SDL_PATHTYPE_DIRECTORY==info.type)
		{
			/* A folder is a game only if a disc image is in it.  Everything
			 * else - a "Scans" folder, an extracted manual, the .wav files a
			 * rip came with - is not a title. */
			const std::string sheet=sheet_in(path);
			if(!sheet.empty())
			{
				std::string title;
				const int number=disc_number_of(name,title);
				/* sheet_in returns this folder plus one of its own entries, so
				 * the file it picked is called what the entry is called. */
				add_image(byTitle,sheet,name,title.empty() ? name : title,number);
			}
			continue;
		}

		if(!is_disc_image(name))
		{
			continue;
		}
		std::string title;
		const int number=disc_number_of(name,title);
		add_image(byTitle,path,name,title,number);
	}
	SDL_free(found);
	return true;
}

} /* namespace */

const char *media_name(Media m)
{
	return Media::Floppy==m ? "Floppy" : "CD";
}

Media media_of(const std::string &filename)
{
	return ends_with_ci(filename,".d77") ? Media::Floppy : Media::Cd;
}

bool is_archived_image(const std::string &filename)
{
	/* Two containers the core cannot open at all.  A .chd is a compressed disc
	 * and Tsugaru has no CHD reader; a .zip is a disc somebody tidied away.
	 * Both are unpacked to a cache folder before the machine is started, so
	 * the core only ever sees a .cue it already understands. */
	return ends_with_ci(filename,".chd") || ends_with_ci(filename,".zip");
}

bool is_disc_image(const std::string &filename)
{
	/*
	 * .cue and .ccd name a sheet whose tracks sit beside them, and those
	 * tracks (.bin, .img) must NOT be listed themselves - a two-track game
	 * would otherwise appear three times, twice unplayable. So .bin is
	 * deliberately absent: a bare .bin with no sheet is not something the
	 * core can sensibly mount either.
	 *
	 * .d77 is a whole floppy, image and all, so it is listed on its own.
	 */
	static const char *const kExt[]={
		".cue",".chd",".zip",".iso",".ccd",".mds",".mdf",".img",".d77",nullptr };
	for(const char *const *e=kExt; *e; ++e)
	{
		if(ends_with_ci(filename,*e))
		{
			/* .img and .mdf are only meaningful with their sheet beside them,
			 * and the sheet is what gets listed.  Accept them only when
			 * nothing else in the name suggests they are a track of
			 * something. */
			if(ends_with_ci(filename,".img") || ends_with_ci(filename,".mdf"))
			{
				return std::string::npos==find_ci(filename,"track");
			}
			return true;
		}
	}
	return false;
}

int disc_number_of(const std::string &filename,std::string &title)
{
	const std::string base=strip_extension(filename);

	/* Every spelling collections actually use.  Ordered longest-first so
	 * "Disc 2 of 4" is not matched as "Disc 2" with " of 4" left behind. */
	static const char *const kWords[]={ "disc","disk","cd",nullptr };

	for(const char *const *w=kWords; *w; ++w)
	{
		size_t at=find_ci(base,*w);
		while(std::string::npos!=at)
		{
			size_t p=at+SDL_strlen(*w);
			while(p<base.size() && (' '==base[p] || '.'==base[p] ||
			                        '_'==base[p] || '-'==base[p]))
			{
				++p;
			}
			if(p<base.size() && isdigit((unsigned char)base[p]))
			{
				int n=0;
				while(p<base.size() && isdigit((unsigned char)base[p]))
				{
					n=n*10+(base[p++]-'0');
				}

				/* Swallow a trailing "of 4" and the bracket the whole thing
				 * sat in, so the title is the title and not "Title (". */
				size_t end=p;
				const size_t of=find_ci(base,"of",end);
				if(std::string::npos!=of && of<=end+1)
				{
					size_t q=of+2;
					while(q<base.size() && ' '==base[q])
					{
						++q;
					}
					while(q<base.size() && isdigit((unsigned char)base[q]))
					{
						++q;
					}
					end=q;
				}

				size_t open=at;
				while(0<open && (' '==base[open-1] || '('==base[open-1] ||
				                 '['==base[open-1] || '-'==base[open-1] ||
				                 '_'==base[open-1]))
				{
					--open;
				}
				while(end<base.size() && (')'==base[end] || ']'==base[end] ||
				                         ' '==base[end]))
				{
					++end;
				}

				const std::string head=base.substr(0,open);
				const std::string tail=end<base.size() ? base.substr(end)
				                                      : std::string();
				title=trim(head+(tail.empty() ? "" : " "+tail));
				if(title.empty())
				{
					title=trim(base);
				}
				return 0<n ? n : 0;
			}
			at=find_ci(base,*w,at+1);
		}
	}

	title=trim(base);
	return 0;
}

std::vector <Game> scan_library(const std::string &parent)
{
	std::vector <Game> games;
	if(parent.empty())
	{
		return games;
	}

	std::set <std::string> visited;
	std::map <std::string,Game> byTitle;

	/* The parent, and the folders the collection is expected to be sorted
	 * into.  The parent is scanned as well, because a library before anybody
	 * has sorted it is a folder of images, and refusing to see those would
	 * leave a working collection looking empty. */
	const std::string bios =folder_for(parent,"bios");
	const std::string cd   =folder_for(parent,"cd");
	const std::string chd  =folder_for(parent,"chd");
	const std::string zip  =folder_for(parent,"zip");

	/* Seen from the parent, these are the shelves - not titles. */
	std::set <std::string> skip;
	for(const std::string &s : {bios,cd,chd,zip})
	{
		if(!s.empty() && s!=parent)
		{
			skip.insert(s);
		}
	}

	const std::string dirs[]={parent,cd,chd,zip};
	for(const std::string &dir : dirs)
	{
		if(dir.empty() || !visited.insert(dir).second)
		{
			continue;
		}
		if(!scan_into(dir,byTitle,skip))
		{
			continue;
		}
	}

	games.reserve(byTitle.size());
	for(auto &kv : byTitle)
	{
		Game &g=kv.second;
		/* An archive whose disc is already unpacked in cd/ is packaging, not a
		 * second disc of the same game - which is what the grouping above,
		 * with no idea of the folder it came from, would otherwise decide. */
		const size_t before=g.discs.size();
		g.discs.erase(std::remove_if(g.discs.begin(),g.discs.end(),
		                             [&](const Disc &d)
		                             {
			                             return is_staged(d.path,cd);
		                             }),
		              g.discs.end());
		if(g.discs.empty())
		{
			continue;
		}
		if(before!=g.discs.size())
		{
			g.media=g.discs.front().media;
		}
		/* In disc order, so "next disc" means the next one.  A game whose
		 * files declare no numbers keeps the order the filesystem gave. */
		std::sort(g.discs.begin(),g.discs.end(),
		          [](const Disc &a,const Disc &b)
		          {
			          if(a.number!=b.number)
			          {
				          return a.number<b.number;
			          }
			          return 0>SDL_strcasecmp(a.file.c_str(),b.file.c_str());
		          });
		games.push_back(std::move(g));
	}
	std::sort(games.begin(),games.end(),[](const Game &a,const Game &b)
	          {
		          return 0>SDL_strcasecmp(a.title.c_str(),b.title.c_str());
	          });
	return games;
}

std::vector <Game> scan_library_saf(const std::string &uri)
{
	std::vector <Game> games;
	if(uri.empty())
	{
		return games;
	}

	std::map <std::string,Game> byTitle;

	/* The same four shelves scan_library walks; "" is the parent itself.
	 * saf_list skips directories, so a game that ships as a folder of tracks
	 * is not seen here - the filesystem path covers that shape. */
	for(const char *sub : {"","cd","chd","zip"})
	{
		for(const SafEntry &e : saf_list(uri,sub))
		{
			if(!is_disc_image(e.name))
			{
				continue;
			}
			std::string title;
			const int number=disc_number_of(e.name,title);
			if(title.empty())
			{
				title=e.name;
			}

			Disc d;
			d.file=e.name;
			d.number=number;
			d.media=media_of(e.name);
			d.saf_uri=uri;
			d.saf_sub=sub;
			d.saf_name=e.name;
			d.path="saf://"+uri+"/"+sub+"/"+e.name;

			Game &g=byTitle[title];
			if(g.title.empty())
			{
				g.title=title;
				const char c=(char)SDL_toupper((unsigned char)title[0]);
				g.initial=('A'<=c && c<='Z') ? c : '#';
				g.media=d.media;
			}
			g.discs.push_back(std::move(d));
		}
	}

	games.reserve(byTitle.size());
	for(auto &kv : byTitle)
	{
		Game &g=kv.second;
		std::sort(g.discs.begin(),g.discs.end(),
		          [](const Disc &a,const Disc &b)
		          {
		          if(a.number!=b.number)
		          {
		          return a.number<b.number;
		          }
		          return 0>SDL_strcasecmp(a.file.c_str(),b.file.c_str());
		          });
		games.push_back(std::move(g));
	}
	std::sort(games.begin(),games.end(),[](const Game &a,const Game &b)
	          {
	          return 0>SDL_strcasecmp(a.title.c_str(),b.title.c_str());
	          });
	return games;
}


} /* namespace towns */
