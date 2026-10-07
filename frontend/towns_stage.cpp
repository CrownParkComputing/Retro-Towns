#include "towns_stage.h"
#include "towns_library.h"

#ifdef TOWNS_HAVE_MINIZIP

/* Packaged as <minizip/unzip.h> on a desktop and taken from zlib's contrib
 * directory on Android, where there is no minizip package to install. */
#if defined(__ANDROID__)
#include "minizip/unzip.h"
#else
#include <minizip/unzip.h>
#endif

#endif /* TOWNS_HAVE_MINIZIP */

#include <SDL3/SDL.h>

#include <cctype>
#include <cstdio>
#include <vector>

namespace towns {

namespace {

std::string lower(std::string s)
{
	for(char &c : s)
	{
		c=(char)std::tolower((unsigned char)c);
	}
	return s;
}

std::string trim(const std::string &s)
{
	size_t a=0,b=s.size();
	while(a<b && std::isspace((unsigned char)s[a])) ++a;
	while(b>a && std::isspace((unsigned char)s[b-1])) --b;
	return s.substr(a,b-a);
}

/* ".cue" from "/a/b/Game.CUE".  Empty when the name has no extension. */
std::string ext_of(const std::string &path)
{
	const size_t dot=path.find_last_of('.');
	const size_t slash=path.find_last_of("/\\");
	if(std::string::npos==dot) return std::string();
	if(std::string::npos!=slash && slash>dot) return std::string();
	return lower(path.substr(dot));
}

std::string leaf_of(const std::string &path)
{
	const size_t slash=path.find_last_of("/\\");
	return std::string::npos==slash ? path : path.substr(slash+1);
}

std::string stem_of(const std::string &path)
{
	const std::string leaf=leaf_of(path);
	const size_t dot=leaf.find_last_of('.');
	return std::string::npos==dot ? leaf : leaf.substr(0,dot);
}

/* "/a/b/c" -> "/a/b", and "/a" -> "/a" is not needed here, so no prizes. */
std::string folder_of(const std::string &path)
{
	const size_t slash=path.find_last_of("/\\");
	return std::string::npos==slash ? std::string(".") : path.substr(0,slash);
}

/*
 * The sheet to mount out of a folder, best first.
 *
 * A .cue is the answer whenever there is one - it is the only format that says
 * what the sector size is and where the audio tracks live.  A bare .bin comes
 * last, and only when nothing better exists, because mounting raw sectors
 * without their sheet is how a dump plays as a wall of noise.
 */
std::string best_sheet(const std::string &dir)
{
	static const char *kFirst[]={".cue",".ccd",".mds",".iso",".bin",nullptr};
	for(const char **e=kFirst; *e; ++e)
	{
		const std::string pattern="*"+std::string(*e);
		int n=0;
		char **found=SDL_GlobDirectory(dir.c_str(),pattern.c_str(),
		                               SDL_GLOB_CASEINSENSITIVE,&n);
		if(nullptr==found)
		{
			continue;
		}
		std::string hit;
		for(int i=0; i<n && found[i]; ++i)
		{
			/* One level only: a sub-folder's sheets belong to a different game. */
			if(nullptr!=SDL_strchr(found[i],'/'))
			{
				continue;
			}
			if(hit.empty())
			{
				hit=dir+"/"+found[i];
			}
		}
		SDL_free(found);
		if(!hit.empty())
		{
			return hit;
		}
	}
	return std::string();
}

bool is_dir(const std::string &path)
{
	SDL_PathInfo info;
	return !path.empty() && SDL_GetPathInfo(path.c_str(),&info) &&
	       SDL_PATHTYPE_DIRECTORY==info.type;
}

/*
 * Written only when every file landed.
 *
 * Without it an unpacking that was interrupted - power pulled, disk full, the
 * player killing the app halfway through a five-hundred megabyte rip - leaves
 * a folder holding a .cue and half a .bin, which the next run would mount as
 * though it were a complete disc.
 */
bool already_staged(const std::string &dir)
{
	SDL_PathInfo info;
	const std::string mark=dir+"/.retrotowns-staged-v2";
	return SDL_GetPathInfo(mark.c_str(),&info) && SDL_PATHTYPE_FILE==info.type;
}

void mark_staged(const std::string &dir,const std::string &from)
{
	SDL_IOStream *out=SDL_IOFromFile((dir+"/.retrotowns-staged-v2").c_str(),"wb");
	if(nullptr==out)
	{
		return;
	}
	SDL_WriteIO(out,from.data(),from.size());
	SDL_CloseIO(out);
}

/* Recursively delete a cache folder.  SDL removes one file or one empty
 * directory, so the tree is unwound from the leaves. */
void remove_tree(const std::string &dir)
{
	int n=0;
	char **found=SDL_GlobDirectory(dir.c_str(),nullptr,SDL_GLOB_CASEINSENSITIVE,&n);
	if(found)
	{
		for(int i=0; i<n && found[i]; ++i)
		{
			if(nullptr!=SDL_strchr(found[i],'/'))
			{
				continue;
			}
			const std::string p=dir+"/"+found[i];
			SDL_PathInfo info;
			if(SDL_GetPathInfo(p.c_str(),&info) && SDL_PATHTYPE_DIRECTORY==info.type)
			{
				remove_tree(p);
			}
			else
			{
				SDL_RemovePath(p.c_str());
			}
		}
		SDL_free(found);
	}
	SDL_RemovePath(dir.c_str());
}

#ifdef TOWNS_HAVE_MINIZIP

/* Every data file a .cue declares, in the order it declares them. */
std::vector <std::string> cue_names(const std::string &cue)
{
	std::vector <std::string> out;
	size_t pos=0;
	while(pos<cue.size())
	{
		size_t eol=cue.find('\n',pos);
		if(std::string::npos==eol)
		{
			eol=cue.size();
		}
		const std::string body=trim(cue.substr(pos,eol-pos));
		pos=eol+1;

		if("file"!=lower(body.substr(0,4)) ||
		   !(body.size()>5 && std::isspace((unsigned char)body[4])))
		{
			continue;
		}
		const std::string rest=trim(body.substr(5));
		std::string name;
		if(rest.size()>1 && '"'==rest[0])
		{
			const size_t end=rest.find('"',1);
			if(std::string::npos==end)
			{
				continue;
			}
			name=rest.substr(1,end-1);
		}
		else
		{
			name=trim(rest.substr(0,rest.find(' ')));
		}
		if(!name.empty())
		{
			out.push_back(name);
		}
	}
	return out;
}

/* One entry, named by what it is called inside the archive, written flat. */
bool copy_entry(unzFile z,const std::string &entry,const std::string &dir,
                int64_t *done,int64_t total,const StageProgress &progress)
{
	const std::string leaf=leaf_of(entry);
	if(leaf.empty())
	{
		return false;
	}
	if(UNZ_OK!=unzOpenCurrentFile(z))
	{
		return false;
	}
	std::vector <char> buf(1u<<20);
	SDL_IOStream *out=SDL_IOFromFile((dir+"/"+leaf).c_str(),"wb");
	bool ok=(nullptr!=out);
	while(ok)
	{
		const int got=unzReadCurrentFile(z,buf.data(),(unsigned)buf.size());
		if(0>=got)
		{
			break;
		}
		ok = SDL_WriteIO(out,buf.data(),(size_t)got)==(size_t)got;
		if(ok)
		{
			*done+=got;
			if(progress)
			{
				progress(*done,total);
			}
		}
	}
	if(out)
	{
		SDL_CloseIO(out);
	}
	unzCloseCurrentFile(z);
	if(!ok)
	{
		SDL_RemovePath((dir+"/"+leaf).c_str());
	}
	return ok;
}

/*
 * Explode a zip disc into [dir].
 *
 * The archive is read through its .cue rather than dumped whole: these rips
 * carry marker files beside the disc, some carry a patch folder as well, and
 * the sheet is the only thing that says which of those bytes are the game.  An
 * archive with no sheet in it falls back to taking every image out of it and
 * letting the caller judge what came of it.
 */
std::string unzip_disc(const std::string &zip,const std::string &dir,
                       std::string &err,const StageProgress &progress)
{
	unzFile z=unzOpen64(zip.c_str());
	if(nullptr==z)
	{
		err="Could not read "+leaf_of(zip)+" as a zip.";
		return std::string();
	}

	std::vector <std::string> images;   /* disc files, by archive name */
	std::vector <std::string> floppies; /* boot/user disks, by archive name */
	std::string sheet,sheet_name;       /* the .cue's own text */
	int64_t sheet_size=0;

	if(UNZ_OK==unzGoToFirstFile(z))
	{
		do
		{
			unz_file_info64 info;
			char name[1024]={};
			if(UNZ_OK!=unzGetCurrentFileInfo64(z,&info,name,sizeof(name)-1,nullptr,0,nullptr,0))
			{
				break;
			}
			const std::string ext=ext_of(name);
			if(".cue"==ext && sheet.empty())
			{
				if(UNZ_OK==unzOpenCurrentFile(z))
				{
					std::vector <char> buf((size_t)info.uncompressed_size+1);
					const int got=unzReadCurrentFile(z,buf.data(),(unsigned)buf.size()-1);
					unzCloseCurrentFile(z);
					if(0<got)
					{
						sheet.assign(buf.data(),(size_t)got);
						sheet_name=leaf_of(name);
						sheet_size=got;
					}
				}
			}
			else if(".bin"==ext || ".iso"==ext || ".img"==ext || ".mdf"==ext ||
			        ".sub"==ext || ".ccd"==ext || ".mds"==ext ||
			        ".wav"==ext || ".mp3"==ext)
			{
				images.push_back(name);
			}
			else if(".d77"==ext || ".d88"==ext || ".xdf"==ext)
			{
				floppies.push_back(name);
			}
		}
		while(UNZ_OK==unzGoToNextFile(z));
	}

	/* What the sheet names, or everything if there is no sheet.  Matched on the
	 * last component, because a rip may well pack the disc under a folder. */
	std::vector <std::string> want;
	const std::vector <std::string> declared=cue_names(sheet);
	for(const std::string &image : images)
	{
		for(const std::string &name : declared)
		{
			if(lower(image)==lower(name) || lower(leaf_of(image))==lower(name))
			{
				want.push_back(image);
				break;
			}
		}
	}
	if(sheet.empty())
	{
		want=images;
	}

	int64_t total=sheet_size;
	for(const std::string &name : want)
	{
		unz_file_info64 info;
		if(UNZ_OK==unzLocateFile(z,name.c_str(),0) &&
		   UNZ_OK==unzGetCurrentFileInfo64(z,&info,nullptr,0,nullptr,0,nullptr,0))
		{
			total+=(int64_t)info.uncompressed_size;
		}
	}
	for(const std::string &name : floppies)
	{
		unz_file_info64 info;
		if(UNZ_OK==unzLocateFile(z,name.c_str(),0) &&
		   UNZ_OK==unzGetCurrentFileInfo64(z,&info,nullptr,0,nullptr,0,nullptr,0))
		{
			total+=(int64_t)info.uncompressed_size;
		}
	}

	if(!SDL_CreateDirectory(dir.c_str()) && !is_dir(dir))
	{
		err="Could not create "+dir+" - check the disk.";
		unzClose(z);
		return std::string();
	}

	int64_t done=0;
	bool ok=true;
	if(!sheet.empty())
	{
		SDL_IOStream *out=SDL_IOFromFile((dir+"/"+sheet_name).c_str(),"wb");
		ok = nullptr!=out &&
		     SDL_WriteIO(out,sheet.data(),sheet.size())==sheet.size();
		if(out)
		{
			SDL_CloseIO(out);
		}
		done+=sheet_size;
		if(progress)
		{
			progress(done,total);
		}
	}
	for(const std::string &name : want)
	{
		if(UNZ_OK!=unzLocateFile(z,name.c_str(),0))
		{
			continue;
		}
		ok = copy_entry(z,name,dir,&done,total,progress) && ok;
	}
	for(const std::string &name : floppies)
	{
		if(UNZ_OK!=unzLocateFile(z,name.c_str(),0))
		{
			continue;
		}
		ok = copy_entry(z,name,dir,&done,total,progress) && ok;
	}
	unzClose(z);

	if(!ok)
	{
		err=leaf_of(zip)+" could not be unpacked - the archive is damaged, or the disk is full.";
		return std::string();
	}
	const std::string mounted=best_sheet(dir);
	if(mounted.empty())
	{
		err=leaf_of(zip)+" holds no disc image this machine can read.";
		return std::string();
	}
	mark_staged(dir,zip);
	return mounted;
}

#endif /* TOWNS_HAVE_MINIZIP */

#ifdef TOWNS_HAVE_LIBCHDR

extern "C" {
#include "libchdr/chd.h"
}

/*
 * A CHD is a block map, not a disc image: the sectors are there, but nothing
 * in Tsugaru can ask for them.  So the whole disc is walked in track order and
 * written out as a raw 2352-byte-per-sector .bin with a .cue beside it, which
 * is exactly what a CDRWin rip on the shelf looks like.
 *
 * The frame numbering inside the archive and the frame numbering inside the
 * .bin are deliberately different: a CHD starts every track on a four-frame
 * boundary and the padding holds nothing, so it is dropped rather than written,
 * and the sheet's INDEX values are computed from what was actually written.
 */
struct ChdTrack
{
	int number=0;
	int frames=0;
	int pregap=0;
	int postgap=0;
	std::string type,subtype,pgtype;
};

bool chd_tracks(chd_file *f,std::vector <ChdTrack> &out)
{
	for(uint32_t i=0; i<99; ++i)
	{
		char meta[512]={};
		uint32_t len=0;
		ChdTrack t;
		char type[64]={},sub[64]={},pgtype[64]={},pgsub[64]={};

		if(CHDERR_NONE==chd_get_metadata(f,CDROM_TRACK_METADATA2_TAG,i,
		                                 meta,sizeof(meta)-1,&len,nullptr,nullptr))
		{
			if(8!=sscanf(meta,CDROM_TRACK_METADATA2_FORMAT,
			             &t.number,type,sub,&t.frames,&t.pregap,pgtype,pgsub,&t.postgap))
			{
				return false;
			}
		}
		else if(CHDERR_NONE==chd_get_metadata(f,CDROM_TRACK_METADATA_TAG,i,
		                                      meta,sizeof(meta)-1,&len,nullptr,nullptr))
		{
			if(4!=sscanf(meta,CDROM_TRACK_METADATA_FORMAT,&t.number,type,sub,&t.frames))
			{
				return false;
			}
		}
		else
		{
			break;
		}
		t.type=type;
		t.subtype=sub;
		t.pgtype=pgtype;
		out.push_back(t);
	}
	return !out.empty();
}

/* The archive's name for a track, spelled the way a .cue spells it. */
std::string cue_type(const std::string &t)
{
	const std::string s=lower(t);
	if("audio"==s)      { return "AUDIO"; }
	if("mode1/2352"==s) { return "MODE1/2352"; }
	if("mode2/2352"==s) { return "MODE2/2352"; }
	if("cdi/2352"==s)   { return "CDI/2352"; }
	if("mode1_raw"==s)  { return "MODE1/2352"; }
	if("mode2_raw"==s)  { return "MODE2/2352"; }
	if("raw"==s)        { return "MODE1/2352"; }
	return t.empty() ? std::string("MODE1/2352") : t;
}

std::string cue_msf(int64_t frames)
{
	char buf[32]={};
	SDL_snprintf(buf,sizeof(buf),"%02d:%02d:%02d",
	             (int)(frames/(75*60)),(int)(frames/75)%60,(int)frames%75);
	return buf;
}

std::string cue_num(int n)
{
	char buf[16]={};
	SDL_snprintf(buf,sizeof(buf),"%02d",n);
	return buf;
}

std::string chd_disc(const std::string &chd,const std::string &dir,
                     std::string &err,const StageProgress &progress)
{
	chd_file *f=nullptr;
	if(CHDERR_NONE!=chd_open(chd.c_str(),CHD_OPEN_READ,nullptr,&f))
	{
		err="Could not read "+leaf_of(chd)+" as a CHD.";
		return std::string();
	}
	const chd_header *h=chd_get_header(f);

	std::vector <ChdTrack> tracks;
	if(!chd_tracks(f,tracks))
	{
		chd_close(f);
		err=leaf_of(chd)+" holds no CD track list - it is a hard-disk or DVD image.";
		return std::string();
	}
	/* 2352 is the raw sector: sync, header, data, ECC and subchannel all in
	 * place, which is the only thing a .cue can describe honestly.  A CHD
	 * usually stores 2448 - the same 2352 with 96 bytes of C2 parity tacked on
	 * the end, which a .cue has no way to ask for and the machine does not
	 * want, so it is dropped on the way out.  Anything smaller than 2352 is a
	 * cooked dump with the framing already stripped. */
	const int64_t sector =
	    2448==h->unitbytes || 2352==h->unitbytes ? 2352 : 0;
	if(0==sector || 0==h->hunkbytes || 0!=h->hunkbytes%h->unitbytes)
	{
		chd_close(f);
		err=leaf_of(chd)+" is not stored as raw sectors ("+
		    std::to_string(h->unitbytes)+" bytes each) - re-dump it without -cooked.";
		return std::string();
	}
	if(!SDL_CreateDirectory(dir.c_str()) && !is_dir(dir))
	{
		chd_close(f);
		err="Could not create "+dir+" - check the disk.";
		return std::string();
	}

	const int64_t unit=h->unitbytes,per=h->hunkbytes/h->unitbytes;
	const int64_t total=(int64_t)h->logicalbytes/unit;
	std::vector <char> hunk((size_t)h->hunkbytes);
	int64_t cached=-1;

	const std::string bin=stem_of(chd)+".bin";
	SDL_IOStream *out=SDL_IOFromFile((dir+"/"+bin).c_str(),"wb");
	if(nullptr==out)
	{
		chd_close(f);
		err="Could not write "+dir+"/"+bin+".";
		return std::string();
	}

	std::string cue="FILE \""+bin+"\" BINARY\r\n";
	std::vector <char> swap((size_t)sector);
	int64_t chd_frame=0,written=0;
	bool ok=true;
	for(const ChdTrack &t : tracks)
	{
		const bool audio="audio"==lower(t.type);
		cue+="  TRACK "+cue_num(t.number)+" "+cue_type(t.type)+"\r\n";
		/* A CHD's track length includes its stored pre-gap, so the sheet says
		 * where the gap begins (INDEX 00) as well as where the music does
		 * (INDEX 01).  PREGAP would be a lie: it means "generate this", and
		 * these frames are in the file. */
		if(0<t.pregap)
		{
			cue+="    INDEX 00 "+cue_msf(written)+"\r\n";
		}
		cue+="    INDEX 01 "+cue_msf(written+t.pregap)+"\r\n";

		for(int fr=0; fr<t.frames && ok; ++fr)
		{
			const int64_t want=chd_frame+fr;
			const int64_t hunknum=want/per;
			if(hunknum!=cached)
			{
				if(CHDERR_NONE!=chd_read(f,(uint32_t)hunknum,hunk.data()))
				{
					ok=false;
					break;
				}
				cached=hunknum;
			}
			const char *src=hunk.data()+(want-per*hunknum)*unit;
			if(audio)
			{
				/* Audio is stored big-endian in the archive, which is how a CD
				 * carries it, and a .bin holds little-endian PCM. */
				for(int64_t k=0;k<sector;k+=2)
				{
					swap[(size_t)k]=src[k+1];
					swap[(size_t)k+1]=src[k];
				}
				ok = SDL_WriteIO(out,swap.data(),(size_t)sector)==(size_t)sector;
			}
			else
			{
				ok = SDL_WriteIO(out,src,(size_t)sector)==(size_t)sector;
			}
			if(ok && progress)
			{
				progress((written+fr+1)*sector,total*sector);
			}
		}
		if(!ok)
		{
			break;
		}
		written+=t.frames;
		chd_frame+=t.frames;
		if(0!=chd_frame%4)
		{
			chd_frame+=4-chd_frame%4;   /* the archive's own track padding */
		}
	}
	SDL_CloseIO(out);
	chd_close(f);

	if(!ok)
	{
		SDL_RemovePath((dir+"/"+bin).c_str());
		err=leaf_of(chd)+" could not be decoded - the archive is damaged, or the disk is full.";
		return std::string();
	}
	const std::string cue_name=stem_of(chd)+".cue";
	SDL_IOStream *sheet=SDL_IOFromFile((dir+"/"+cue_name).c_str(),"wb");
	if(nullptr==sheet ||
	   SDL_WriteIO(sheet,cue.data(),cue.size())!=cue.size())
	{
		if(sheet)
		{
			SDL_CloseIO(sheet);
		}
		err="Could not write "+dir+"/"+cue_name+".";
		return std::string();
	}
	SDL_CloseIO(sheet);

	mark_staged(dir,chd);
	return dir+"/"+cue_name;
}

#endif /* TOWNS_HAVE_LIBCHDR */

} /* namespace */

std::string best_floppy(const std::string &dir)
{
	/* A boot or user disk is a floppy beside the CD, named .d77, .d88 or .xdf.
	 * The core mounts it in FD0 before the CD, so the bootloader can run and
	 * then hand control to the disc. */
	static const char *const kFloppy[]={".d77",".d88",".xdf",nullptr};
	for(const char *const *e=kFloppy; *e; ++e)
	{
		const std::string pattern="*"+std::string(*e);
		int n=0;
		char **found=SDL_GlobDirectory(dir.c_str(),pattern.c_str(),
		                               SDL_GLOB_CASEINSENSITIVE,&n);
		if(nullptr==found)
		{
			continue;
		}
		std::string hit;
		for(int i=0; i<n && found[i]; ++i)
		{
			if(nullptr!=SDL_strchr(found[i],'/'))
			{
				continue;   /* one level only */
			}
			if(hit.empty())
			{
				hit=dir+"/"+found[i];
			}
		}
		SDL_free(found);
		if(!hit.empty())
		{
			return hit;
		}
	}
	return std::string();
}

/* An extraction written before the floppy code is stale: it holds the CD
 * but not the boot disk, and the shelf must not keep offering it.  Removes
 * every folder under [cd_dir] that carries the old marker but not the new
 * one, so the next boot re-extracts the archive whole. */
void clean_stale_stages(const std::string &cd_dir)
{
	if(cd_dir.empty())
	{
		return;
	}
	int n=0;
	char **found=SDL_GlobDirectory(cd_dir.c_str(),nullptr,SDL_GLOB_CASEINSENSITIVE,&n);
	for(int i=0; nullptr!=found && i<n && found[i]; ++i)
	{
		if(nullptr!=SDL_strchr(found[i],'/'))
		{
			continue;
		}
		const std::string dir=cd_dir+"/"+found[i];
		SDL_PathInfo info;
		if(!SDL_GetPathInfo(dir.c_str(),&info) || SDL_PATHTYPE_DIRECTORY!=info.type)
		{
			continue;
		}
		const std::string v1=dir+"/.retrotowns-staged";
		const std::string v2=dir+"/.retrotowns-staged-v2";
		SDL_PathInfo a,b;
		if(SDL_GetPathInfo(v1.c_str(),&a) && !SDL_GetPathInfo(v2.c_str(),&b))
		{
			remove_tree(dir);
		}
	}
	if(found)
	{
		SDL_free(found);
	}
}

std::string stage_dir_for(const std::string &image,const std::string &cd_dir)
{
	if(cd_dir.empty())
	{
		return folder_of(image)+"/"+stem_of(image);
	}
	return cd_dir+"/"+stem_of(image);
}

bool is_staged(const std::string &image,const std::string &cd_dir)
{
	if(!is_archived_image(image))
	{
		return false;
	}
	const std::string dir=stage_dir_for(image,cd_dir);
	return already_staged(dir) && !best_sheet(dir).empty();
}

std::string stage_image(const std::string &image,const std::string &cd_dir,
                        std::string &err,const StageProgress &progress)
{
	if(!is_archived_image(image))
	{
		return image;            /* the core can mount this one itself */
	}
	const std::string dir=stage_dir_for(image,cd_dir);
	if(already_staged(dir))
	{
		const std::string have=best_sheet(dir);
		if(!have.empty())
		{
			return have;
		}
	}
	/* Re-extracting: clear whatever the previous (floppy-less) run left. */
	remove_tree(dir);

#ifdef TOWNS_HAVE_MINIZIP
	if(".zip"==ext_of(image))
	{
		return unzip_disc(image,dir,err,progress);
	}
#endif
#ifdef TOWNS_HAVE_LIBCHDR
	if(".chd"==ext_of(image))
	{
		return chd_disc(image,dir,err,progress);
	}
#endif
	const std::string ext=ext_of(image);
#ifdef TOWNS_HAVE_MINIZIP
	const bool zip_ok=true;
#else
	const bool zip_ok=false;
#endif
#ifdef TOWNS_HAVE_LIBCHDR
	const bool chd_ok=true;
#else
	const bool chd_ok=false;
#endif
	if(".zip"==ext && !zip_ok)
	{
		err="This build has no zip reader - install minizip and rebuild.";
	}
	else if(".chd"==ext && !chd_ok)
	{
		err="This build has no CHD reader - core/vendor/libchdr is not built in.";
	}
	else
	{
		err="This build cannot unpack "+leaf_of(image)+".";
	}
	return std::string();
}

} /* namespace towns */
