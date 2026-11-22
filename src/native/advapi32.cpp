// ADVAPI32 for the native host: an in-memory registry seeded with the game's settings (language as written by
// the installer; Language Packs/*.reg), and CryptoAPI stubs.
#include <dirent.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "win.h"

namespace native {

namespace {

struct Value {
    uint32_t type;
    std::string data;
};

std::mutex g_mutex;
std::map<std::string, std::map<std::string, Value>> g_keys;  // lower-case full path -> values
std::map<uint32_t, std::string> g_open;                       // handle -> path
uint32_t g_next = 0x2000;

std::string Lower(std::string s) {
    for (char& ch : s) ch = char(tolower((unsigned char)ch));
    return s;
}

std::string RootName(uint32_t h) {
    switch (h) {
        case 0x80000000u: return "hkcr";
        case 0x80000001u: return "hkcu";
        case 0x80000002u: return "hklm";
        case 0x80000003u: return "hku";
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_open.find(h);
    return it == g_open.end() ? std::string() : it->second;
}

// The game's language folder (Data/Locale/<xx-yy>): `locale` from the settings unless empty or "auto"; otherwise the
// system language (SPORERE_SYSTEM_LOCALE from the Android app, e.g. "pt-BR", or LANG on a desktop, e.g. "ru_RU.UTF-8")
// when the game has it, exactly or by language (pt -> pt-br, en -> en-us, zh -> zh-cn); else English.
std::string PickLocale() {
    std::string want = Setting("locale", "");
    for (char& ch : want) ch = char(tolower((unsigned char)ch));
    if (!want.empty() && want != "auto") return want;
    std::vector<std::string> have;
    std::string dir = std::string(g_game_dir ? g_game_dir : ".") + "/Data/Locale";
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.' && strlen(e->d_name) == 5 && e->d_name[2] == '-') have.push_back(e->d_name);
        closedir(d);
    }
    auto has = [&](const std::string& l) { return std::find(have.begin(), have.end(), l) != have.end(); };
    const char* sys = getenv("SPORERE_SYSTEM_LOCALE");
    if (!sys || !*sys) sys = getenv("LANG");
    std::string s = sys ? sys : "";
    for (char& ch : s) ch = ch == '_' ? '-' : char(tolower((unsigned char)ch));
    std::string full = s.substr(0, 5), lang = s.substr(0, 2);
    std::string pick;
    if (full.size() == 5 && has(full)) {
        pick = full;
    } else if (lang.size() == 2) {
        static const char* const kPreferred[] = {"en-us", "pt-br", "zh-cn", "es-es", "fr-fr", "de-de"};
        for (const char* p : kPreferred)
            if (lang == std::string(p, 2) && has(p)) pick = p;
        if (pick.empty())
            for (const std::string& l : have)
                if (l.compare(0, 2, lang) == 0) pick = l;
    }
    if (pick.empty()) pick = has("en-us") || have.empty() ? "en-us" : have.front();
    Log("language: %s (system %s)", pick.c_str(), s.empty() ? "unknown" : s.c_str());
    return pick;
}

void Seed() {
    static bool done = false;
    if (done) return;
    done = true;
    auto sz = [](const char* s) { return Value{1, std::string(s) + '\0'}; };
    auto dw = [](uint32_t v) { return Value{4, std::string(reinterpret_cast<const char*>(&v), 4)}; };
    // What the GOG installer writes (32-bit view), with the game root at kGuestGameDir. The language: the setting
    // (sporere.ini `locale = en-us`, the settings panel), else the phone's language when the game has it.
    static const std::string locale = PickLocale();
    // the parent key too: the game opens it and enumerates the subkeys to find the installed packs (without it
    // Galactic Adventures counted as not installed: base Data/Config instead of DataEP1's, no GA menu item, and
    // reading back the GA keys in Preferences.prop threw "unknown property")
    g_keys["hklm\\software\\electronic arts"];
    auto& k = g_keys["hklm\\software\\electronic arts\\spore"];
    k["appdir"] = sz("Spore");
    k["datadir"] = sz("C:\\Game\\Data");
    k["installcompleted"] = dw(1);
    k["installloc"] = sz("C:\\Game");
    k["locale"] = sz(locale.c_str());
    k["playerdir"] = sz("My Spore Creations");
    k["reindexyesorno"] = dw(0);
    auto& ep1 = g_keys["hklm\\software\\electronic arts\\spore_ep1"];
    ep1["addonid"] = dw(2);
    ep1["datadir"] = sz("C:\\Game\\DataEP1\\");
    ep1["packid"] = dw(0x07a7f786u);
    auto& bp1 = g_keys["hklm\\software\\electronic arts\\spore creepy and cute parts pack"];
    bp1["addonid"] = dw(1);
    bp1["datadir"] = sz("C:\\Game\\bp1content\\");
    bp1["packid"] = dw(0x06f4b5d1u);
}

uint32_t OpenPath(uint32_t parent, const std::string& sub, bool create) {
    Seed();
    std::string path = RootName(parent);
    if (path.empty()) return 0;
    if (!sub.empty()) path += "\\" + Lower(sub.c_str());
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_keys.count(path)) {
        if (!create) return 0;
        g_keys[path];
    }
    uint32_t h = g_next;
    g_next += 4;
    g_open[h] = path;
    return h;
}
uint32_t Open(uint32_t parent, uint32_t sub, bool create) { return OpenPath(parent, sub ? Str(sub) : "", create); }

#define A(name) void a_##name(Ctx* c)

A(RegOpenKeyExA) {
    uint32_t h = Open(Arg(c, 0), Arg(c, 1), false);
    if (h) W32(Arg(c, 4), h);
    Return(c, h ? 0 : 2, 5);  // ERROR_FILE_NOT_FOUND
}
A(RegOpenKeyA) {
    uint32_t h = Open(Arg(c, 0), Arg(c, 1), false);
    if (h) W32(Arg(c, 2), h);
    Return(c, h ? 0 : 2, 3);
}
A(RegCreateKeyExA) {
    uint32_t h = Open(Arg(c, 0), Arg(c, 1), true);
    W32(Arg(c, 7), h);
    if (Arg(c, 8)) W32(Arg(c, 8), 2);  // REG_OPENED_EXISTING_KEY
    Return(c, h ? 0 : 2, 9);
}
A(RegCloseKey) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_open.erase(Arg(c, 0));
    Return(c, 0, 1);
}
A(RegQueryValueExA) {
    std::string path = RootName(Arg(c, 0));
    std::string name = Arg(c, 1) ? Lower(Str(Arg(c, 1))) : "";
    uint32_t type_out = Arg(c, 3), data = Arg(c, 4), size_out = Arg(c, 5);
    static const bool trace = SettingInt("trace_registry", 0) != 0;
    if (trace) Log("RegQueryValueExA(%s\\%s)", path.c_str(), name.c_str());
    std::lock_guard<std::mutex> lock(g_mutex);
    auto k = g_keys.find(path);
    if (k == g_keys.end() || !k->second.count(name)) {
        Return(c, 2, 6);
        return;
    }
    const Value& v = k->second[name];
    if (type_out) W32(type_out, v.type);
    uint32_t need = uint32_t(v.data.size());
    if (data) {
        if (!size_out || R32(size_out) < need) {
            if (size_out) W32(size_out, need);
            Return(c, 234, 6);  // ERROR_MORE_DATA
            return;
        }
        memcpy(Ptr<void>(data), v.data.data(), need);
    }
    if (size_out) W32(size_out, need);
    Return(c, 0, 6);
}
A(RegSetValueExA) {
    std::string path = RootName(Arg(c, 0));
    std::string name = Arg(c, 1) ? Lower(Str(Arg(c, 1))) : "";
    static const bool trace = SettingInt("trace_registry", 0) != 0;
    if (trace) Log("RegSetValueExA(%s\\%s, type %u, %u bytes, dword %u)", path.c_str(), name.c_str(), Arg(c, 3),
                   Arg(c, 5), Arg(c, 5) >= 4 ? R32(Arg(c, 4)) : 0);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_keys[path][name] = Value{Arg(c, 3), std::string(Str(Arg(c, 4)), Arg(c, 5))};
    Return(c, 0, 6);
}
A(RegEnumKeyA) { Return(c, 259, 4); }  // ERROR_NO_MORE_ITEMS

// CryptoAPI: the engine uses it for online features and random seeds only.
A(CryptAcquireContextA) {
    W32(Arg(c, 0), 0x3000);
    Return(c, 1, 5);
}
A(CryptReleaseContext) { Return(c, 1, 2); }
A(CryptGenRandom) {
    uint32_t buf = Arg(c, 2);
    for (uint32_t i = 0; i < Arg(c, 1); ++i) W8(buf + i, uint8_t(rand()));
    Return(c, 1, 3);
}
A(CryptCreateHash) { Return(c, 0, 5); }
A(CryptHashData) { Return(c, 0, 4); }
A(CryptGetHashParam) { Return(c, 0, 5); }
A(CryptDestroyHash) { Return(c, 1, 1); }
A(CryptImportKey) { Return(c, 0, 6); }
A(CryptEncrypt) { Return(c, 0, 7); }
A(CryptDestroyKey) { Return(c, 1, 1); }

// Wide variants: names in UTF-16; string values are stored as UTF-8 and handed back as UTF-16.
A(RegOpenKeyExW) {
    uint32_t h = OpenPath(Arg(c, 0), WideToUtf8(Arg(c, 1)), false);
    static const bool trace = SettingInt("trace_registry", 0) != 0;
    if (trace) Log("RegOpenKeyExW(%s) -> %s", WideToUtf8(Arg(c, 1)).c_str(), h ? "ok" : "not found");
    if (h) W32(Arg(c, 4), h);
    Return(c, h ? 0 : 2, 5);
}
A(RegQueryValueExW) {
    std::string path = RootName(Arg(c, 0));
    std::string name = Lower(WideToUtf8(Arg(c, 1)).c_str());
    uint32_t type_out = Arg(c, 3), data = Arg(c, 4), size_out = Arg(c, 5);
    static const bool trace = SettingInt("trace_registry", 0) != 0;
    if (trace) Log("RegQueryValueExW(%s\\%s)", path.c_str(), name.c_str());
    std::lock_guard<std::mutex> lock(g_mutex);
    auto k = g_keys.find(path);
    if (k == g_keys.end() || !k->second.count(name)) {
        Return(c, 2, 6);
        return;
    }
    const Value& v = k->second[name];
    std::string bytes = v.data;
    if (v.type == 1 || v.type == 2) {  // REG_SZ / REG_EXPAND_SZ
        std::string text = v.data.substr(0, v.data.find('\0'));
        bytes.clear();
        uint32_t tmp = HeapAlloc(uint32_t(text.size() + 1) * 4 + 4);
        uint32_t n = PutWideString(tmp, uint32_t(text.size()) * 2 + 2, text);
        bytes.assign(Ptr<char>(tmp), 2 * (n + 1));
        HeapFree(tmp);
    }
    if (type_out) W32(type_out, v.type);
    uint32_t need = uint32_t(bytes.size());
    if (data) {
        if (!size_out || R32(size_out) < need) {
            if (size_out) W32(size_out, need);
            Return(c, 234, 6);  // ERROR_MORE_DATA
            return;
        }
        memcpy(Ptr<void>(data), bytes.data(), need);
    }
    if (size_out) W32(size_out, need);
    Return(c, 0, 6);
}
// (hKey, index, name, *cchName, reserved, class, *cchClass, lastWrite): the direct subkeys, in the case the GOG
// installer writes them.
A(RegEnumKeyExW) {
    std::string path = RootName(Arg(c, 0)) + "\\";
    uint32_t index = Arg(c, 1), name = Arg(c, 2), cch = Arg(c, 3);
    std::vector<std::string> subs;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& kv : g_keys) {
            if (kv.first.size() > path.size() && kv.first.compare(0, path.size(), path) == 0 &&
                kv.first.find('\\', path.size()) == std::string::npos)
                subs.push_back(kv.first.substr(path.size()));
        }
    }
    if (index >= subs.size() || path.size() == 1) {
        Return(c, 259, 8);  // ERROR_NO_MORE_ITEMS
        return;
    }
    std::string sub = subs[index];
    if (sub == "spore_ep1") sub = "spore_EP1";
    else if (sub == "spore creepy and cute parts pack") sub = "spore Creepy and Cute Parts Pack";
    uint32_t room = cch ? R32(cch) : 0;
    if (!name || room < sub.size() + 1) {
        if (cch) W32(cch, uint32_t(sub.size()) + 1);
        Return(c, 234, 8);  // ERROR_MORE_DATA
        return;
    }
    uint32_t n = PutWideString(name, room, sub);
    W32(cch, n);
    if (Arg(c, 6)) W32(Arg(c, 6), 0);
    if (Arg(c, 7)) W32(Arg(c, 7), 0), W32(Arg(c, 7) + 4, 0);
    Return(c, 0, 8);
}
void PutUserName(uint32_t buf, uint32_t size_ptr, bool wide) {
    const char* name = "Player";
    uint32_t cap = R32(size_ptr), need = uint32_t(strlen(name)) + 1;
    if (cap >= need) {
        if (wide) PutWideString(buf, cap, name);
        else strcpy(Str(buf), name);
    }
    W32(size_ptr, need);
}
A(GetUserNameA) {
    PutUserName(Arg(c, 0), Arg(c, 1), false);
    Return(c, 1, 2);
}
A(GetUserNameW) {
    PutUserName(Arg(c, 0), Arg(c, 1), true);
    Return(c, 1, 2);
}
// Event log: accepted and dropped (the game reports crashes there).
A(RegisterEventSourceA) { Return(c, 0x4000u, 2); }
A(ReportEventA) { Return(c, 1, 9); }
A(DeregisterEventSource) { Return(c, 1, 1); }

#undef A

#define E(name) {"advapi32.dll", #name, a_##name}
const ImportDef kAdvapi32[] = {
    E(RegOpenKeyExA), E(RegOpenKeyA), E(RegCreateKeyExA), E(RegCloseKey), E(RegQueryValueExA), E(RegSetValueExA),
    E(RegEnumKeyA), E(CryptAcquireContextA), E(CryptReleaseContext), E(CryptGenRandom), E(CryptCreateHash),
    E(CryptHashData), E(CryptGetHashParam), E(CryptDestroyHash), E(CryptImportKey), E(CryptEncrypt),
    E(CryptDestroyKey), E(RegOpenKeyExW), E(RegQueryValueExW), E(RegEnumKeyExW), E(GetUserNameA), E(GetUserNameW),
    E(RegisterEventSourceA), E(ReportEventA), E(DeregisterEventSource),
};
#undef E
ImportRegistrar g_advapi32(kAdvapi32);

}  // namespace

}  // namespace native
