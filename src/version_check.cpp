// Version check: both ends tell each other which swml mods they run, and the log says what differs.
//
// A client replays the host's world with its own copy of the simulation, so a mod that changes the simulation on one
// end only makes the two drift apart (2026-10-24: the friend ran realistic_aero 0.5.6 with the VLM off against the
// host's 0.5.7 lattice, and nothing said so). The manifest is a line of "id=version" for every enabled mod plus the
// few settings that change the simulation itself (physics_fixes' wheel spin, realistic_aero's fixes and VLM mode,
// network_fixes' remote push), read from the mods folder and swml.ini at start-up. It travels once per peer and
// session on our channel 11, reliably, after the redundancy hello (redundancy.cpp); peers without this mod never see it.
//
// Logs "versions: peerN ..." once per peer and manifest: "same mods" or every difference, marked
//   MUST MATCH  mods every player needs (multiplayer=everyone, e.g. scope_look's game data)
//   differs     anything else: informational (a client replays the host's physics, it doesn't recompute it).

#include "log.h"
#include "net_shared.h"

#include <windows.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace {

struct ModInfo {
    std::string version, multiplayer;
};
struct SimSetting {
    const char* mod;
    const wchar_t* section;
    const wchar_t* key;
    const wchar_t* fallback;
};
// Settings that change what the simulation computes (not just how fast or what is logged).
constexpr SimSetting kSimSettings[] = {
    {"physics_fixes", L"wheels", L"spin_fix", L"1"},
    {"physics_fixes", L"wheels", L"keep_per_tick", L"0.9999"},
    {"realistic_aero", L"fix", L"enabled", L"1"},
    {"realistic_aero", L"vlm", L"mode", L"off"},
    {"network_fixes", L"push", L"remote_push", L"1"},
};

std::string g_manifest;
std::mutex g_mu;
std::map<int, std::string> g_logged; // peer -> manifest already compared

std::string narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s += (c > 31 && c < 127 && c != ';' && c != '=') ? char(c) : '_';
    return s;
}

std::wstring parent(const std::wstring& path)
{
    size_t i = path.find_last_of(L'\\');
    return i == std::wstring::npos ? path : path.substr(0, i);
}

std::wstring ini_read(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* def)
{
    wchar_t buf[128];
    GetPrivateProfileStringW(section, key, def, buf, 128, file.c_str());
    return buf;
}

// "id=version;..." for enabled mods, then "id.section.key=value;..." for the simulation settings.
std::string build_manifest()
{
    std::wstring mods = parent(mod_dir()), root = parent(mods), swml_ini = root + L"\\swml.ini";
    std::map<std::string, ModInfo> found;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((mods + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            std::wstring dir = mods + L"\\" + fd.cFileName, man = dir + L"\\mod.ini";
            if (GetFileAttributesW(man.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            std::wstring id = ini_read(man, L"mod", L"id", fd.cFileName);
            int def = GetPrivateProfileIntW(L"mod", L"default_enabled", 1, man.c_str());
            if (!GetPrivateProfileIntW(L"mods", id.c_str(), def, swml_ini.c_str())) continue;
            found[narrow(id)] = {narrow(ini_read(man, L"mod", L"version", L"?")), narrow(ini_read(man, L"mod", L"multiplayer", L"local"))};
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::string m;
    for (auto& [id, info] : found) m += id + "=" + info.version + "/" + info.multiplayer + ";";
    for (const SimSetting& s : kSimSettings) {
        if (!found.count(s.mod)) continue;
        std::wstring dir = mods + L"\\" + std::wstring(s.mod, s.mod + strlen(s.mod));
        std::wstring ini = dir + L"\\settings.ini";
        if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) ini = dir + L"\\settings.default.ini";
        m += std::string(s.mod) + "." + narrow(s.section) + "." + narrow(s.key) + "=" + narrow(ini_read(ini, s.section, s.key, s.fallback)) + ";";
    }
    return m;
}

std::map<std::string, std::string> parse(const std::string& m)
{
    std::map<std::string, std::string> out;
    size_t at = 0;
    while (at < m.size()) {
        size_t end = m.find(';', at);
        if (end == std::string::npos) end = m.size();
        std::string item = m.substr(at, end - at);
        size_t eq = item.find('=');
        if (eq != std::string::npos) out[item.substr(0, eq)] = item.substr(eq + 1);
        at = end + 1;
    }
    return out;
}

} // namespace

const std::string& version_manifest()
{
    if (g_manifest.empty()) g_manifest = build_manifest();
    return g_manifest;
}

void version_on_manifest(int peer, const char* data, size_t size)
{
    std::string theirs(data, std::min<size_t>(size, 4096));
    {
        std::lock_guard lock(g_mu);
        if (g_logged[peer] == theirs) return;
        g_logged[peer] = theirs;
    }
    auto mine = parse(version_manifest()), other = parse(theirs);
    std::set<std::string> keys;
    for (auto& [k, v] : mine) keys.insert(k);
    for (auto& [k, v] : other) keys.insert(k);
    std::string diff;
    int must = 0;
    for (const std::string& k : keys) {
        auto a = mine.find(k), b = other.find(k);
        std::string va = a == mine.end() ? "(none)" : a->second, vb = b == other.end() ? "(none)" : b->second;
        if (va == vb) continue;
        // Critical: a mod every player needs (game data / definitions on both ends). Everything else is informational:
        // a client replays vehicles and loose objects kinematically from the host's states and puppets every other
        // character (MP_STABILITY_REVIEW.md section 6), so host-side physics changes (wheel spin, aero, push) can't
        // make the two copies drift apart, and physics_fixes' client parts are local speed-ups.
        bool everyone = va.find("/everyone") != std::string::npos || vb.find("/everyone") != std::string::npos;
        bool critical = everyone;
        must += critical;
        diff += "  " + std::string(critical ? "MUST MATCH " : "differs ") + k + ": here " + va + ", peer " + vb + ";";
    }
    if (diff.empty()) swlog("versions: peer%d runs the same mods and simulation settings", peer);
    else swlog("versions: peer%d differs (%d that change the shared simulation):%s", peer, must, diff.c_str());
}
