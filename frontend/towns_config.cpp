#include "towns_config.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <map>
#include <string>

namespace towns {

const char *port_device_name(PortDevice d)
{
    switch (d) {
    case PortDevice::None:       return "Empty";
    case PortDevice::Pad:        return "Game Pad";
    case PortDevice::Mouse:      return "Mouse";
    case PortDevice::CyberStick: return "Cyber Stick";
    case PortDevice::AnalogPad:  return "Analog Game Pad";
    }
    return "Empty";
}

/*
 * Straight from the core's enum: TOWNSEMU groups these by generation in the
 * comments above the type list, and TownsTypeToStr is what names them on the
 * command line. Nothing here is a claim about hardware this app has not read.
 */
const TownsModel kTownsModels[]={
    { "",        "Core default - say nothing" },
    { "R50",     "FMR-50 / FMR-60" },
    { "R50S",    "FMR-50S" },
    { "R70",     "FMR-70" },
    { "MODEL2",  "1st generation: Model 1, Model 2" },
    { "2F",      "2nd generation: 1F, 2F" },
    { "20F",     "3rd generation: 10F, 20F" },
    { "UX",      "FMR-2UX" },
    { "CX",      "FMR-2CX" },
    { "UG",      "FMR-2UG" },
    { "HG",      "FMR-2HG" },
    { "HR",      "FMR-2HR" },
    { "UR",      "FMR-2UR" },
    { "MA",      "FMR-2MA" },
    { "MX",      "FMR-2MX" },
    { "ME",      "FMR-2ME" },
    { "MF",      "FMR-2MF" },
    { "HC",      "FMR-2HC" },
    { "MARTY",   "FM Towns Marty - the console" },
};
const int kTownsModelCount=(int)(sizeof(kTownsModels)/sizeof(kTownsModels[0]));

bool is_towns_model(const std::string &name)
{
    if (name.empty()) return true;
    for (int i = 0; i < kTownsModelCount; ++i)
        if (name == kTownsModels[i].name) return true;
    return false;
}

namespace {

std::string read_file(const std::string &path)
{
    SDL_IOStream *in = SDL_IOFromFile(path.c_str(), "rb");
    if (!in) return std::string();
    const Sint64 size = SDL_GetIOSize(in);
    if (size <= 0 || size > (1 << 20)) { SDL_CloseIO(in); return std::string(); }
    std::string text((size_t)size, '\0');
    const size_t got = SDL_ReadIO(in, text.data(), text.size());
    SDL_CloseIO(in);
    if (got != text.size()) return std::string();
    return text;
}

bool write_file(const std::string &path, const std::string &text)
{
    SDL_IOStream *out = SDL_IOFromFile(path.c_str(), "wb");
    if (!out) return false;
    const bool ok = SDL_WriteIO(out, text.data(), text.size()) == text.size();
    SDL_CloseIO(out);
    return ok;
}

std::map<std::string, std::string> parse_kv(const std::string &text)
{
    std::map<std::string, std::string> kv;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string v = line.substr(eq + 1);
        if (!v.empty() && v.back() == '\r') v.pop_back();
        kv[line.substr(0, eq)] = v;
    }
    return kv;
}

std::string str_of(const std::map<std::string, std::string> &kv, const char *k,
                   const std::string &dflt)
{
    auto it = kv.find(k);
    return it == kv.end() ? dflt : it->second;
}
int int_of(const std::map<std::string, std::string> &kv, const char *k, int dflt)
{
    auto it = kv.find(k);
    return it == kv.end() ? dflt : std::atoi(it->second.c_str());
}
bool bool_of(const std::map<std::string, std::string> &kv, const char *k, bool dflt)
{
    auto it = kv.find(k);
    return it == kv.end() ? dflt : (it->second == "1");
}
float float_of(const std::map<std::string, std::string> &kv, const char *k, float dflt)
{
    auto it = kv.find(k);
    return it == kv.end() ? dflt : (float)std::atof(it->second.c_str());
}

/* "/a/b/" -> "/a", "/a/b" -> "/a".  Empty in, empty out. */
std::string parent_of(const std::string &path)
{
    size_t end = path.find_last_not_of("/\\");
    if (end == std::string::npos) return std::string();
    const size_t sep = path.find_last_of("/\\", end);
    if (sep == std::string::npos) return std::string();
    size_t keep = path.find_last_not_of("/\\", sep);
    return keep == std::string::npos ? std::string() : path.substr(0, keep + 1);
}

/* Clamped on the way in, not trusted. A hand-edited file should make the app
 * behave oddly at worst, never index off the end of an enum. */
PortDevice port_of(const std::map<std::string, std::string> &kv, const char *k,
                   PortDevice dflt)
{
    const int v = int_of(kv, k, (int)dflt);
    return (v >= 0 && v <= (int)PortDevice::AnalogPad) ? (PortDevice)v : dflt;
}

} /* namespace */

bool load_app_config(const std::string &path, AppConfig &out)
{
    const std::string text = read_file(path);
    if (text.empty()) return false;
    const auto kv = parse_kv(text);

    out.library_root = str_of(kv, "library_root", out.library_root);
    /* One key replaced three. Somebody with an old file should not lose their
     * settings, and the answer is the obvious one: the rom dir was the parent,
     * or its own sibling folders were. */
    if (out.library_root.empty()) {
        const std::string old_rom = str_of(kv, "rom_dir", std::string());
        if (!old_rom.empty()) out.library_root = parent_of(old_rom);
        else out.library_root = str_of(kv, "disc_root", std::string());
    }
    out.saves_dir   = str_of(kv, "saves_dir", out.saves_dir);
    out.last_disc   = str_of(kv, "last_disc", out.last_disc);
    out.setup_version = int_of(kv, "setup_version", out.setup_version);

    Settings &s = out.machine;
    s.towns_type    = str_of(kv, "towns_type", s.towns_type);
    /* Not passed to the core if the core would not recognise it. */
    if (!is_towns_model(s.towns_type)) s.towns_type.clear();
    s.cpu_freq      = int_of(kv, "cpu_freq", s.cpu_freq);
    if (s.cpu_freq < 0 || 200 < s.cpu_freq) s.cpu_freq = 0;
    s.mem_size      = int_of(kv, "mem_size", s.mem_size);
    if (s.mem_size < 0 || 64 < s.mem_size) s.mem_size = 0;
    s.pretend_386dx = bool_of(kv, "pretend_386dx", s.pretend_386dx);
    s.high_fidelity = bool_of(kv, "high_fidelity", s.high_fidelity);
    s.keyboard_jp     = bool_of(kv, "keyboard_jp", s.keyboard_jp);
    s.preferred_mode  = int_of(kv, "preferred_mode", s.preferred_mode);
    if (s.preferred_mode < 0 || s.preferred_mode > 2) s.preferred_mode = 0;
    s.scaling         = int_of(kv, "scaling", s.scaling);
    if (s.scaling < 0 || s.scaling > 4) s.scaling = 0;
    s.vsync           = bool_of(kv, "vsync", s.vsync);
    s.audio_muted     = bool_of(kv, "audio_muted", s.audio_muted);
    s.audio_latency_ms = int_of(kv, "audio_latency_ms", s.audio_latency_ms);
    if (s.audio_latency_ms < 20 || s.audio_latency_ms > 500) s.audio_latency_ms = 80;
    s.port1           = port_of(kv, "port1", s.port1);
    s.port2           = port_of(kv, "port2", s.port2);
    for (int i = 0; i < (int)padmap::SIG_COUNT; ++i) {
        const std::string v = str_of(kv, padmap::kEntries[i].key, s.pad.bind[i]);
        /* A name SDL does not know reads as unbound rather than as whatever
         * control happens to share its index. */
        s.pad.bind[i] = padmap::Map::valid(v) ? v : std::string();
    }
    s.stick_deadzone = float_of(kv, "stick_deadzone", s.stick_deadzone);
    if (s.stick_deadzone < 0.0f || 0.9f < s.stick_deadzone) s.stick_deadzone = 0.35f;
    s.touch_pad       = str_of(kv, "touch_pad", s.touch_pad);
    if (s.touch_pad.empty()) s.touch_pad = "ftowns";
    s.touch_pad_show  = int_of(kv, "touch_pad_show", s.touch_pad_show);
    if (s.touch_pad_show < 0 || s.touch_pad_show > 2) s.touch_pad_show = 1;
    return true;
}

bool save_app_config(const std::string &path, const AppConfig &cfg)
{
    const Settings &s = cfg.machine;
    std::string t = "# Retro-Towns settings\n";
    t += "library_root=" + cfg.library_root + "\n";
    t += "saves_dir=" + cfg.saves_dir + "\n";
    t += "last_disc=" + cfg.last_disc + "\n";
    t += "setup_version=" + std::to_string(cfg.setup_version) + "\n";
    t += "towns_type=" + s.towns_type + "\n";
    t += "cpu_freq=" + std::to_string(s.cpu_freq) + "\n";
    t += "mem_size=" + std::to_string(s.mem_size) + "\n";
    t += "pretend_386dx="; t += s.pretend_386dx ? "1" : "0"; t += "\n";
    t += "high_fidelity="; t += s.high_fidelity ? "1" : "0"; t += "\n";
    t += "keyboard_jp="; t += s.keyboard_jp ? "1" : "0"; t += "\n";
    t += "preferred_mode=" + std::to_string(s.preferred_mode) + "\n";
    t += "scaling=" + std::to_string(s.scaling) + "\n";
    t += "vsync="; t += s.vsync ? "1" : "0"; t += "\n";
    t += "audio_muted="; t += s.audio_muted ? "1" : "0"; t += "\n";
    t += "audio_latency_ms=" + std::to_string(s.audio_latency_ms) + "\n";
    t += "port1=" + std::to_string((int)s.port1) + "\n";
    t += "port2=" + std::to_string((int)s.port2) + "\n";
    for (int i = 0; i < (int)padmap::SIG_COUNT; ++i) {
        t += std::string(padmap::kEntries[i].key) + "=" + s.pad.bind[i] + "\n";
    }
    t += "stick_deadzone=" + std::to_string(s.stick_deadzone) + "\n";
    t += "touch_pad=" + s.touch_pad + "\n";
    t += "touch_pad_show=" + std::to_string(s.touch_pad_show) + "\n";
    return write_file(path, t);
}

} /* namespace towns */
