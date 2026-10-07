#include "towns_setup.h"

#include <algorithm>

#include <SDL3/SDL.h>

namespace towns {

namespace {

bool is_dir(const std::string &p)
{
    SDL_PathInfo info;
    return !p.empty() && SDL_GetPathInfo(p.c_str(),&info)
        && SDL_PATHTYPE_DIRECTORY==info.type;
}

bool is_file(const std::string &p,long long *size=nullptr)
{
    SDL_PathInfo info;
    if(p.empty() || !SDL_GetPathInfo(p.c_str(),&info) || SDL_PATHTYPE_FILE!=info.type)
    {
        return false;
    }
    if(size)
    {
        *size=(long long)info.size;
    }
    return true;
}

/* Does this folder hold anything at all?  Used to prefer a library over an
 * empty folder of the same name. */
bool holds_something(const std::string &dir)
{
    int n=0;
    char **found=SDL_GlobDirectory(dir.c_str(),nullptr,SDL_GLOB_CASEINSENSITIVE,&n);
    const bool any=(nullptr!=found && 0<n);
    if(found)
    {
        SDL_free(found);
    }
    return any;
}

} /* namespace */

const std::vector <RomNeed> &rom_needs()
{
    /* The names are not a preference.  TownsPhysicalMemory::LoadROMImages
     * looks these six up in the folder it is given, and a TOWNS with no
     * FMT_SYS.ROM has nothing to execute.  The sizes are the reference Fujitsu
     * dump's; a different size is reported but not refused, because a
     * differently-revisioned BIOS is legitimately a different size and the
     * machine may still run on it. */
    static const std::vector <RomNeed> kNeeds={
        {"FMT_SYS.ROM",262144,ROM_MUST,  "The system BIOS - without it there is no machine."},
        {"FMT_DOS.ROM",524288,ROM_MUST,  "TOWNS OS, the operating system it boots into."},
        {"FMT_FNT.ROM",262144,ROM_SHOULD,"The font ROM - Japanese text needs it."},
        {"FMT_F20.ROM",524288,ROM_SHOULD,"The 20-dot font ROM."},
        {"FMT_DIC.ROM",524288,ROM_SHOULD,"The dictionary ROM."},
        {"MYTOWNS.ROM",32,     ROM_MAY,  "Machine preferences, as the real one kept them."},
        {"MAR_EX0.ROM",0,      ROM_MAY,  "FM Towns Marty extra ROMs - only for a Marty."},
        {"MAR_EX1.ROM",0,      ROM_MAY,  "FM Towns Marty extra ROMs - only for a Marty."},
        {"MAR_EX2.ROM",0,      ROM_MAY,  "FM Towns Marty extra ROMs - only for a Marty."},
        {"MAR_EX3.ROM",0,      ROM_MAY,  "FM Towns Marty extra ROMs - only for a Marty."},
    };
    return kNeeds;
}

BiosCheck check_bios(const std::string &dir)
{
    BiosCheck c;
    c.dir=dir;
    c.exists=is_dir(dir);
    if(!c.exists)
    {
        return c;
    }

    /* One file can stand in for all of them: the core accepts a concatenated
     * FMT_ALL.ROM and takes the pieces out of it, and some collections ship
     * that way. */
    long long allSize=0;
    c.combined=is_file(dir+"/FMT_ALL.ROM",&allSize) && 0<allSize;

    for(const RomNeed &need : rom_needs())
    {
        RomRow row;
        row.need=need;
        long long size=0;
        if(is_file(dir+"/"+need.name,&size))
        {
            row.found=size;
            row.state=(0==need.size || size==need.size) ? 2 : 1;
        }
        else if(c.combined && ROM_MAY!=need.need)
        {
            /* FMT_ALL.ROM carries the five the core actually loads.  The
             * Marty extras are read separately, so they are not covered. */
            const std::string n=need.name;
            if(0==n.compare(0,4,"FMT_"))
            {
                row.state=2;
            }
        }
        c.rows.push_back(row);
        if(ROM_MUST==need.need && 2!=row.state)
        {
            ++c.missing;
        }
    }

    /* "Usable" is deliberately narrower than "complete": the two ROMs the
     * machine executes, plus a folder that at least exists.  The MAY rows are
     * shown so a Marty owner can see what is missing, not because a Model 2
     * needs them. */
    c.ok=0==c.missing;
    return c;
}

bool is_saf_root(const std::string &s)
{
    return 0 == s.rfind("content://", 0);
}

std::string existing_folder_for(const std::string &root,const std::string &kind)
{
	static const struct {const char *kind;const char *names[8];} kAlias[]={
		{"bios",{"bios","BIOS","ROM","rom","firmware","TownsROM","system",nullptr}},
		{"cd",  {"cd","cds","cdrom","discs","games","Games","roms",nullptr}},
		{"chd", {"chd","chds","cue",nullptr}},
		{"zip", {"zip","zips","compressed",nullptr}},
	};
	if(root.empty())
	{
		return std::string();
	}

	std::string first_existing;
	for(const auto &a : kAlias)
	{
		if(kind!=a.kind)
		{
			continue;
		}
		for(int i=0; a.names[i]; ++i)
		{
			const std::string p=root+"/"+a.names[i];
			if(!is_dir(p))
			{
				continue;
			}
			if(first_existing.empty())
			{
				first_existing=p;
			}
			if(holds_something(p))
			{
				return p;
			}
		}
		break;
	}
	return first_existing;
}

std::string folder_for(const std::string &root,const std::string &kind)
{
	if(root.empty())
	{
		return std::string();
	}
	const std::string existing=existing_folder_for(root,kind);
	return existing.empty() ? root+"/"+kind : existing;
}

bool ensure_layout(const std::string &root)
{
	if(root.empty())
	{
		return false;
	}
	bool ok=true;
	for(const char *kind : {"bios","cd","zip"})
	{
		if(!existing_folder_for(root,kind).empty())
		{
			continue;
		}
		const std::string want=root+"/"+kind;
		if(!is_dir(want) && !SDL_CreateDirectory(want.c_str()))
		{
			ok=false;
		}
	}
	return ok;
}

LayoutReport scan_layout(const std::string &root)
{
	LayoutReport rep;
	rep.root=root;
	rep.root_exists=is_dir(root);
	for(const char *kind : {"bios","cd","zip"})
	{
		FolderReport f;
		f.kind=kind;
		f.path=root.empty() ? std::string() : folder_for(root,kind);
		f.exists=is_dir(f.path);

		if(f.exists)
		{
			int n=0;
			char **found=SDL_GlobDirectory(f.path.c_str(),nullptr,
                                           SDL_GLOB_CASEINSENSITIVE,&n);
			for(int i=0; nullptr!=found && i<n && nullptr!=found[i]; ++i)
			{
				const std::string name=found[i];
				if(nullptr!=SDL_strchr(name.c_str(),'/'))
				{
					continue;                    /* one level only */
				}
				SDL_PathInfo info;
				if(SDL_GetPathInfo((f.path+"/"+name).c_str(),&info))
				{
					if(SDL_PATHTYPE_DIRECTORY==info.type)
					{
						++f.dirs;
					}
					else
					{
						++f.files;
					}
				}
			}
			if(found)
			{
				SDL_free(found);
			}
		}
		rep.folders.push_back(f);
	}
	return rep;
}

std::string find_bios(const std::string &root)
{
    if(root.empty())
    {
        return std::string();
    }
    /* The convention first, then the parent itself: plenty of people keep the
     * six files loose, and a TOWNS ROM folder pointed straight at the app is
     * the shape the core's own command line expects. */
    const std::string inside=folder_for(root,"bios");
    if(check_bios(inside).ok)
    {
        return inside;
    }
    if(check_bios(root).ok)
    {
        return root;
    }
    return std::string();
}

std::string bios_candidate(const std::string &root)
{
    const std::string usable=find_bios(root);
    if(!usable.empty())
    {
        return usable;
    }
    if(root.empty())
    {
        return std::string();
    }
    /* folder_for invents "<root>/bios" when there is nothing there, and an
     * invented folder has nothing to say about which files are missing - so
     * only a folder that exists is worth a checklist. */
    const std::string inside=folder_for(root,"bios");
    if(is_dir(inside))
    {
        return inside;
    }
    return is_dir(root) ? root : std::string();
}

std::vector <std::string> candidate_roots()
{
    std::vector <std::string> out;
#if defined(__ANDROID__)
    /* One line per volume, written by the APK before SDL_main runs.
     *
     * The app's own external directories need no grant and are real paths an
     * emulator can mount, which is the whole reason the wizard points at them
     * instead of offering a picker - a content:// handle cannot be mounted.
     * But SDL answers for ONE volume, and a TOWNS library is gigabytes of CD
     * images, which is not what a handheld's internal storage is for.  Java
     * sees every volume through getExternalFilesDirs, so the list comes from
     * there.  A file rather than a JNI call because it is read once at startup
     * and never changes: no bridge, no thread rules, no lifetime to get wrong.
     */
    if(const char *in=SDL_GetAndroidInternalStoragePath())
    {
        const std::string list=std::string(in)+"/roots.txt";
        size_t len=0;
        if(void *data=SDL_LoadFile(list.c_str(),&len))
        {
            const char *p=static_cast<const char *>(data);
            const char *end=p+len;
            while(p<end)
            {
                const char *nl=p;
                while(nl<end && '\n'!=*nl) { ++nl; }
                if(nl>p && '/'==*p)
                {
                    out.emplace_back(p,(size_t)(nl-p));
                }
                p=nl+1;
            }
            SDL_free(data);
        }
    }
    /* The one SDL does know, for a build whose APK wrote no list - and after
     * the Java answer, because the SD card is the better place for a library. */
    if(const char *ext=SDL_GetAndroidExternalStoragePath())
    {
        out.emplace_back(std::string(ext)+"/FM Towns");
    }
#else
    if(const char *home=SDL_GetUserFolder(SDL_FOLDER_HOME))
    {
        out.push_back(std::string(home)+"FMTowns");
    }
    if(const char *docs=SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS))
    {
        out.push_back(std::string(docs)+"FMTowns");
    }
#endif
    /* Deduplicate, and drop anything that is not a folder: the Java list is
     * written before the directories are checked, and a volume that has since
     * been unmounted is not a place to put a library. */
    {
        std::vector <std::string> uniq;
        for(const auto &r : out)
        {
            if(is_dir(r) && uniq.end()==std::find(uniq.begin(),uniq.end(),r))
            {
                uniq.push_back(r);
            }
        }
        out.swap(uniq);
    }
    return out;
}

/* ---- the picker ---- */

namespace {

struct Pick
{
    bool open=false;
    bool ready=false;
    std::string path;
};
Pick g_pick;

#if !defined(__ANDROID__)
void pick_cb(void *,const char *const *filelist,int)
{
    g_pick.open=false;
    if(filelist && filelist[0])
    {
        g_pick.path=filelist[0];
        g_pick.ready=true;
    }
}
#endif

} /* namespace */

bool pickers_usable()
{
    return true;
}

void begin_pick_folder()
{
#if defined(__ANDROID__)
    saf_pick();
#else
    if(g_pick.open)
    {
        return;
    }
    g_pick.open=true;
    SDL_ShowOpenFolderDialog(pick_cb,nullptr,nullptr,nullptr,false);
#endif
}

bool pick_in_progress()
{
#if defined(__ANDROID__)
    return false;
#else
    return g_pick.open;
#endif
}

bool take_pick(std::string &out)
{
#if defined(__ANDROID__)
    auto t = saf_trees();
    if (!t.empty()) {
        out = t.back().path.empty() ? t.back().uri : t.back().path;
        return true;
    }
    return false;
#else
    if(!g_pick.ready)
    {
        return false;
    }
    g_pick.ready=false;
    out=g_pick.path;
    return true;
#endif
}

} /* namespace towns */

#if defined(__ANDROID__)
#include <jni.h>

namespace towns {
namespace {

jclass bridge_class(JNIEnv *env)
{
    static jclass cached = nullptr;
    if (cached) return cached;
    jclass local = env->FindClass("com/crownpark/retro_towns/SafBridge");
    if (!local) { env->ExceptionClear(); return nullptr; }
    cached = (jclass)env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    return cached;
}

std::string to_std(JNIEnv *env, jstring s)
{
    if (!s) return std::string();
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

std::string call_str(const char *name, const char *sig,
                     const std::vector<std::string> &args)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (!env) return std::string();
    jclass cls = bridge_class(env);
    if (!cls) return std::string();
    jmethodID m = env->GetStaticMethodID(cls, name, sig);
    if (!m) { env->ExceptionClear(); return std::string(); }

    jvalue jargs[4] = {};
    jstring locals[4] = {};
    for (size_t i = 0; i < args.size() && i < 4; ++i) {
        locals[i] = env->NewStringUTF(args[i].c_str());
        jargs[i].l = locals[i];
    }
    jstring r = (jstring)env->CallStaticObjectMethodA(cls, m, jargs);
    if (env->ExceptionCheck()) { env->ExceptionClear(); r = nullptr; }
    std::string out = to_std(env, r);
    if (r) env->DeleteLocalRef(r);
    for (size_t i = 0; i < args.size() && i < 4; ++i)
        if (locals[i]) env->DeleteLocalRef(locals[i]);
    return out;
}

void call_void(const char *name, const char *sig,
               const std::vector<std::string> &args)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (!env) return;
    jclass cls = bridge_class(env);
    if (!cls) return;
    jmethodID m = env->GetStaticMethodID(cls, name, sig);
    if (!m) { env->ExceptionClear(); return; }
    jvalue jargs[4] = {};
    jstring locals[4] = {};
    for (size_t i = 0; i < args.size() && i < 4; ++i) {
        locals[i] = env->NewStringUTF(args[i].c_str());
        jargs[i].l = locals[i];
    }
    env->CallStaticVoidMethodA(cls, m, jargs);
    if (env->ExceptionCheck()) env->ExceptionClear();
    for (size_t i = 0; i < args.size() && i < 4; ++i)
        if (locals[i]) env->DeleteLocalRef(locals[i]);
}

int call_int(const char *name)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (!env) return -1;
    jclass cls = bridge_class(env);
    if (!cls) return -1;
    jmethodID m = env->GetStaticMethodID(cls, name, "()I");
    if (!m) { env->ExceptionClear(); return -1; }
    const jint v = env->CallStaticIntMethod(cls, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return -1; }
    return (int)v;
}

std::vector<std::vector<std::string>> rows(const std::string &text, int fields)
{
    std::vector<std::vector<std::string>> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty()) continue;
        std::vector<std::string> row;
        size_t p = 0;
        for (int i = 0; i < fields; ++i) {
            size_t tab = (i == fields - 1) ? std::string::npos : line.find('\t', p);
            row.push_back(line.substr(p, tab == std::string::npos
                                          ? std::string::npos : tab - p));
            if (tab == std::string::npos) break;
            p = tab + 1;
        }
        out.push_back(std::move(row));
    }
    return out;
}

} /* namespace */

bool saf_available() { return true; }
void saf_pick() { call_void("pick", "()V", {}); }
void saf_forget(const std::string &uri) { call_void("forget", "(Ljava/lang/String;)V", { uri }); }

bool saf_ensure_layout(const std::string &uri)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (!env) return false;
    jclass cls = bridge_class(env);
    if (!cls) return false;
    jmethodID m = env->GetStaticMethodID(cls, "ensureLayout", "(Ljava/lang/String;)Z");
    if (!m) { env->ExceptionClear(); return false; }
    jstring s = env->NewStringUTF(uri.c_str());
    const jboolean ok = env->CallStaticBooleanMethod(cls, m, s);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(s);
    return ok == JNI_TRUE;
}

std::vector<SafTree> saf_trees()
{
    std::vector<SafTree> out;
    for (auto &r : rows(call_str("trees", "()Ljava/lang/String;", {}), 2)) {
        if (r.size() < 2) continue;
        SafTree t;
        t.uri  = r[0];
        t.name = r[1];
        t.path = call_str("realPath", "(Ljava/lang/String;)Ljava/lang/String;", { t.uri });
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<SafEntry> saf_list(const std::string &uri, const std::string &sub)
{
    std::vector<SafEntry> out;
    const std::string text = call_str(
        "list", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", { uri, sub });
    for (auto &r : rows(text, 2)) {
        if (r.size() < 2) continue;
        SafEntry e;
        e.name  = r[0];
        e.bytes = SDL_strtoll(r[1].c_str(), nullptr, 10);
        out.push_back(std::move(e));
    }
    return out;
}

std::string saf_stage(const std::string &uri, const std::string &sub,
                      const std::string &name, const std::string &dest_dir)
{
    return call_str("stage",
                    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                    "Ljava/lang/String;)Ljava/lang/String;",
                    { uri, sub, name, dest_dir });
}

int saf_stage_progress() { return call_int("stageProgress"); }

std::string saf_stage_bios(const std::string &uri, const std::string &dest_dir)
{
    SDL_CreateDirectory(dest_dir.c_str());

    /* The bios/ folder whole - it is a handful of ROMs, never a CD image. */
    for (const SafEntry &e : saf_list(uri, "bios")) {
        saf_stage(uri, "bios", e.name, dest_dir);
    }

    /* Loose ROM files at the root, for a collection nobody has sorted yet. */
    for (const SafEntry &e : saf_list(uri, "")) {
        bool rom = false;
        for (const RomNeed &need : rom_needs()) {
            if (0 == SDL_strcasecmp(e.name.c_str(), need.name)) { rom = true; break; }
        }
        if (rom) {
            saf_stage(uri, "", e.name, dest_dir);
        }
    }
    return dest_dir;
}

std::string saf_folder_name(const std::string &uri, const std::string &kind)
{
    const std::string n = call_str(
        "folderName", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
        { uri, kind });
    return n.empty() ? kind : n;
}

} /* namespace towns */

#else

namespace towns {
bool saf_available() { return false; }
void saf_pick() {}
std::vector<SafTree> saf_trees() { return {}; }
void saf_forget(const std::string &) {}
bool saf_ensure_layout(const std::string &) { return false; }
std::string saf_folder_name(const std::string &, const std::string &k) { return k; }
std::vector<SafEntry> saf_list(const std::string &, const std::string &) { return {}; }
std::string saf_stage(const std::string &, const std::string &, const std::string &, const std::string &) { return {}; }
int saf_stage_progress() { return -1; }
std::string saf_stage_bios(const std::string &, const std::string &) { return {}; }
} /* namespace towns */

#endif

