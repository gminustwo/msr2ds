// Discord Rich Presence: what the player is doing, on their Discord profile ("Playing msr2ds: Area 4, 23 Metroids
// left"), through the Discord app's local connection (its IPC: \\.\pipe\discord-ipc-N on Windows, the discord-ipc-N
// socket in the runtime or temporary folder elsewhere, Flatpak's and Snap's included). No library: the protocol is a
// frame of an opcode, a length and JSON (0 handshake, 1 a command, 2 close, 3 ping, 4 pong). Nothing goes anywhere but
// the Discord app on this computer, and only while the player's "Discord Rich Presence" setting is on.
//   - the launcher: "In the launcher"; it lets go while the game runs (msr_discord_hold), whose own connection shows it;
//   - the game: the area, from the map package being loaded (layered_fs.cpp's "where:" lines: packs/maps/<scenario>/),
//     named as the game names it on its pause map (its text table's GUI_AREA_* line), and the Metroids left (the
//     items the touch screen's bar shows), with the time since the game started.
// A connection lost or not there (Discord closed, not installed) is looked for again every 20 s. The log says when it
// connects, what it shows and why it stopped; never the Discord account's name. The Android app has none of it.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#elif !defined(__ANDROID__)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif
#include "msr.h"
#include "msr/overlay.h"

#ifndef MSR_NATIVE_VERSION
#define MSR_NATIVE_VERSION "dev"
#endif
#ifndef MSR_UPDATE_REPO
#define MSR_UPDATE_REPO "gminustwo/msr2ds"
#endif
#ifndef MSR_DISCORD_APP_ID
#define MSR_DISCORD_APP_ID ""
#endif
#ifndef MSR_DISCORD_INVITE
#define MSR_DISCORD_INVITE ""
#endif

namespace {

// The map folders' areas, as the game's world map groups them (its code's "s040_area4|s050_area5" and
// "s060_area6|s065_area6b|s067_area6c"; Area04-05 ... Area10 in gui/scripts/ingamemenucomposition.lc) and as its pause
// map names them, in the same order: Surface, Area 1 ... Area 8 (the text table's GUI_AREA_SURFACE, GUI_AREA_AREA_1 ...).
// The folders' own numbers are the game's working ones (s090_area9 is the player's Area 7).
struct Scenario {
    const char* folder;
    int area;   // 0 the Surface, 1..8
};
constexpr Scenario Scenarios[] = {
    {"s000_surface", 0}, {"s110_surfaceb", 0}, {"s010_area1", 1},  {"s020_area2", 2},  {"s025_area2b", 2},
    {"s028_area2c", 2},  {"s030_area3", 3},    {"s033_area3b", 3}, {"s036_area3c", 3}, {"s040_area4", 4},
    {"s050_area5", 4},   {"s060_area6", 5},    {"s065_area6b", 5}, {"s067_area6c", 5}, {"s070_area7", 6},
    {"s090_area9", 7},   {"s100_area10", 8},
};
constexpr const char* AreaNames[] = {"Surface", "Area 1", "Area 2", "Area 3", "Area 4", "Area 5", "Area 6", "Area 7", "Area 8"};

int AreaOf(const std::string& folder) {
    for (const auto& s : Scenarios)
        if (folder == s.folder) return s.area;
    return -1;
}

// "packs/maps/<scenario>/<scenario>.pkg" -> "<scenario>" (a sub-area's package, or anything else: "")
std::string ScenarioOf(const char* path) {
    static constexpr char Maps[] = "packs/maps/";
    if (!path || std::strncmp(path, Maps, sizeof Maps - 1)) return {};
    const char* folder = path + sizeof Maps - 1;
    const char* slash = std::strchr(folder, '/');
    if (!slash) return {};
    const std::string name(folder, slash);
    return std::string(slash + 1) == name + ".pkg" ? name : std::string{};
}

#if !defined(__ANDROID__)

// The game's own names for the areas: its English text table (system/localization/us_english.txt, "BTXT": a key, then
// its UTF-16 text, each ended by a zero), read once in the game; the launcher and a table without them keep AreaNames.
std::string GameAreaName(int area) {
    static std::once_flag once;
    static std::string names[9];
    static bool in_game = false;
    std::call_once(once, [] {
        size_t size = 0;
        uint8_t* text = msr_romfs_file("system/localization/us_english.txt", &size);
        if (!text) return;
        in_game = true;
        std::size_t i = 8;
        while (i < size) {
            const std::size_t key_end = std::find(text + i, text + size, 0) - text;
            const std::string key(reinterpret_cast<const char*>(text + i), key_end - i);
            std::size_t k = key_end + 1;
            std::string value;   // UTF-16 to UTF-8 (the names are ASCII in English; the rest kept as it comes)
            for (; k + 1 < size && (text[k] || text[k + 1]); k += 2) {
                const unsigned c = text[k] | unsigned{text[k + 1]} << 8;
                if (c < 0x80) value += static_cast<char>(c);
                else if (c < 0x800) { value += static_cast<char>(0xC0 | c >> 6); value += static_cast<char>(0x80 | (c & 0x3F)); }
                else { value += static_cast<char>(0xE0 | c >> 12); value += static_cast<char>(0x80 | (c >> 6 & 0x3F)); value += static_cast<char>(0x80 | (c & 0x3F)); }
            }
            i = k + 2;
            if (key == "GUI_AREA_SURFACE") names[0] = value;
            else if (key.size() == 15 && key.compare(0, 14, "GUI_AREA_AREA_") == 0 && key[14] >= '1' && key[14] <= '8')
                names[key[14] - '0'] = value;
        }
        std::free(text);
    });
    if (area < 0 || area > 8) return {};
    return in_game && !names[area].empty() ? names[area] : AreaNames[area];
}

std::string Json(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += static_cast<char>(c);
    }
    return out + "\"";
}

// ---- the connection to the Discord app ----
enum Op : uint32_t { Handshake = 0, Frame = 1, Close = 2, Ping = 3, Pong = 4 };

struct Connection {
#ifdef _WIN32
    HANDLE pipe = INVALID_HANDLE_VALUE;
    bool Open() const { return pipe != INVALID_HANDLE_VALUE; }
#else
    int fd = -1;
    bool Open() const { return fd >= 0; }
#endif
    std::string in;   // bytes read, not yet a whole frame

    void Shut() {
#ifdef _WIN32
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
#else
        if (fd >= 0) close(fd);
        fd = -1;
#endif
        in.clear();
    }

    bool Connect() {
#ifdef _WIN32
        for (int i = 0; i < 10; ++i) {
            wchar_t name[64];
            std::swprintf(name, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
            pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) return true;
        }
        return false;
#else
        // the runtime folder, else the temporary ones; the app there, or as Flatpak (Discord, Vesktop) or Snap runs it
        std::vector<std::string> bases;
        for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
            if (const char* v = std::getenv(var); v && *v) bases.emplace_back(v);
        bases.emplace_back("/tmp");
        static constexpr const char* Subfolders[] = {"", "app/com.discordapp.Discord/", "app/dev.vencord.Vesktop/",
                                                     "snap.discord/", ".flatpak/com.discordapp.Discord/xdg-run/",
                                                     "app/com.discordapp.DiscordCanary/", "snap.discord-canary/"};
        for (const auto& base : bases)
            for (const char* sub : Subfolders)
                for (int i = 0; i < 10; ++i) {
                    std::string path = base;
                    if (path.back() != '/') path += '/';
                    path += std::string(sub) + "discord-ipc-" + std::to_string(i);
                    sockaddr_un address{};
                    address.sun_family = AF_UNIX;
                    if (path.size() >= sizeof address.sun_path) continue;
                    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
                    const int s = socket(AF_UNIX, SOCK_STREAM, 0);
                    if (s < 0) return false;
#ifdef __APPLE__
                    const int one = 1;
                    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
                    if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0) {
                        fd = s;
                        return true;
                    }
                    close(s);
                }
        return false;
#endif
    }

    bool Send(uint32_t op, const std::string& json) {
        std::string frame(8, '\0');
        const uint32_t length = static_cast<uint32_t>(json.size());
        std::memcpy(&frame[0], &op, 4);   // (little-endian: every platform here)
        std::memcpy(&frame[4], &length, 4);
        frame += json;
        std::size_t done = 0;
        while (done < frame.size()) {
#ifdef _WIN32
            DWORD n = 0;
            if (!WriteFile(pipe, frame.data() + done, static_cast<DWORD>(frame.size() - done), &n, nullptr) || !n) {
                Shut();
                return false;
            }
#else
#ifdef MSG_NOSIGNAL
            const ssize_t n = send(fd, frame.data() + done, frame.size() - done, MSG_NOSIGNAL);
#else
            const ssize_t n = send(fd, frame.data() + done, frame.size() - done, 0);
#endif
            if (n <= 0) {
                Shut();
                return false;
            }
#endif
            done += static_cast<std::size_t>(n);
        }
        return true;
    }

    // What has come in, without waiting (up to `wait_ms` for the first byte); false once the connection is gone
    bool Read(int wait_ms) {
        char buf[4096];
        for (int waited = 0;;) {
#ifdef _WIN32
            DWORD available = 0;   // (only this thread uses the pipe: no read of its own is pending behind this)
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) { Shut(); return false; }
            if (available) {
                DWORD n = 0;
                if (!ReadFile(pipe, buf, std::min<DWORD>(available, sizeof buf), &n, nullptr) || !n) { Shut(); return false; }
                in.append(buf, n);
                continue;
            }
#else
            pollfd p{fd, POLLIN, 0};
            const int ready = poll(&p, 1, 0);
            if (ready > 0) {
                const ssize_t n = recv(fd, buf, sizeof buf, 0);
                if (n <= 0) { Shut(); return false; }
                in.append(buf, static_cast<std::size_t>(n));
                continue;
            }
            if (ready < 0) { Shut(); return false; }
#endif
            if (waited >= wait_ms) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            waited += 20;
        }
    }

    // The next whole frame read, if one is there
    bool Next(uint32_t& op, std::string& json) {
        if (in.size() < 8) return false;
        uint32_t length;
        std::memcpy(&op, in.data(), 4);
        std::memcpy(&length, in.data() + 4, 4);
        if (length > (1u << 20)) { Shut(); return false; }   // not Discord's
        if (in.size() < 8 + length) return false;
        json = in.substr(8, length);
        in.erase(0, 8 + length);
        return true;
    }
};

// "key":"value" in a frame's JSON (the first one), for the log; "" when not there
std::string Field(const std::string& json, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    const auto at = json.find(k);
    if (at == std::string::npos) return {};
    auto v = json.find_first_not_of(" :", at + k.size());
    if (v == std::string::npos) return {};
    if (json[v] == '"') {
        const auto end = json.find('"', v + 1);
        return end == std::string::npos ? std::string{} : json.substr(v + 1, end - v - 1);
    }
    const auto end = json.find_first_of(",}", v);
    return json.substr(v, end == std::string::npos ? std::string::npos : end - v);
}

// ---- what is shown ----
struct Wanted {
    bool launcher = false;
    bool held = false;
    std::string scenario;      // the map folder last loaded ("" none yet)
    int metroids_left = -1;    // -1: not known
};

std::mutex lock;
std::condition_variable wake;
Wanted wanted;
bool started = false, stopping = false;
std::thread worker;
std::int64_t start_time = 0;   // the program's start, for "elapsed"

bool Enabled() {
    return MSR::GetOverlay().GetPlayerSettings().discord_presence;
}

// The activity's JSON and its words for the log, from what is wanted
std::string Activity(const Wanted& w, std::string& said) {
    std::string details, state;
    if (w.launcher) {
        details = "In the launcher";
    } else {
        const int area = AreaOf(w.scenario);
        if (area >= 0) {
            details = GameAreaName(area);
            if (w.metroids_left > 0) state = std::to_string(w.metroids_left) + (w.metroids_left == 1 ? " Metroid left" : " Metroids left");
            else if (w.metroids_left == 0) state = "No Metroids left";
        } else if (w.scenario == "s020_credits") {
            details = "Watching the credits";
        } else {
            details = "In the main menu";   // (s000_mainmenu, s010_cockpit: the title menus' scene; before any of them)
        }
    }
    said = details + (state.empty() ? "" : ", " + state);
    std::string a = "{\"details\":" + Json(details);
    if (!state.empty()) a += ",\"state\":" + Json(state);
    if (!w.launcher && start_time) a += ",\"timestamps\":{\"start\":" + std::to_string(start_time) + "}";
    a += ",\"assets\":{\"large_image\":\"icon\",\"large_text\":" + Json("msr2ds " MSR_NATIVE_VERSION) + "}";
    a += ",\"buttons\":[{\"label\":\"Get msr2ds\",\"url\":" + Json("https://github.com/" MSR_UPDATE_REPO) + "}";
    if (*MSR_DISCORD_INVITE) a += ",{\"label\":\"Join the Discord\",\"url\":" + Json(MSR_DISCORD_INVITE) + "}";
    a += "]}";
    return a;
}

std::string Command(const std::string& activity) {   // SET_ACTIVITY; no activity clears it
    static unsigned nonce = 0;
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    std::string args = "{\"pid\":" + std::to_string(pid);
    if (!activity.empty()) args += ",\"activity\":" + activity;
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":" + args + "},\"nonce\":\"" + std::to_string(++nonce) + "\"}";
}

void Work() {
    using Clock = std::chrono::steady_clock;
    Connection c;
    Clock::time_point next_try{}, last_sent{};
    std::string shown;          // the activity sent last ("" none)
    bool told_missing = false, told_off = false, told_taken = false;
    int failures = 0;           // handshakes refused in a row (a wrong application ID: looked for less often)
    for (;;) {
        Wanted w;
        {
            std::unique_lock hold{lock};
            wake.wait_for(hold, std::chrono::seconds(1));
            if (stopping) break;
            w = wanted;
        }
        const auto now = Clock::now();
        const bool enabled = Enabled() && !w.held;
        if (!enabled) {
            if (c.Open()) {
                c.Send(Frame, Command(""));
                c.Shut();
                std::fprintf(stderr, "discord: %s\n", w.held ? "the game shows its own Rich Presence: the launcher's cleared"
                                                             : "Rich Presence turned off: cleared");
            } else if (!told_off && !w.held) {
                std::fprintf(stderr, "discord: Rich Presence is off (the Game tab's Discord Rich Presence)\n");
            }
            told_off = !w.held;
            shown.clear();
            continue;
        }
        told_off = false;
        if (!c.Open()) {
            if (now < next_try) continue;
            next_try = now + std::chrono::seconds(failures >= 3 ? 120 : 20);
            if (!c.Connect()) {
                if (!told_missing) std::fprintf(stderr, "discord: no Discord app running on this computer (looked for every 20 s)\n");
                told_missing = true;
                continue;
            }
            // the handshake, then READY (or a close that says why not)
            bool ready = false;
            std::string why;
            if (c.Send(Handshake, "{\"v\":1,\"client_id\":" + Json(MSR_DISCORD_APP_ID) + "}")) {
                for (int waited = 0; waited < 5000 && c.Open() && !ready && why.empty(); waited += 100) {
                    c.Read(100);
                    uint32_t op;
                    std::string json;
                    while (c.Next(op, json)) {
                        if (op == Frame && Field(json, "evt") == "READY") ready = true;
                        else if (op == Close || Field(json, "evt") == "ERROR") why = Field(json, "message") + " (" + Field(json, "code") + ")";
                    }
                }
            }
            if (!ready) {
                c.Shut();
                failures++;
                std::fprintf(stderr, "discord: the Discord app did not take the connection%s%s\n", why.empty() ? "" : ": ", why.c_str());
                continue;
            }
            failures = 0;
            told_missing = false;
            told_taken = false;
            shown.clear();
            std::fprintf(stderr, "discord: connected to the Discord app: Rich Presence on\n");
        }
        // what came in: pings answered, a close or an error said once
        if (!c.Read(0)) {
            std::fprintf(stderr, "discord: the Discord app closed the connection (looked for again in 20 s)\n");
            next_try = now + std::chrono::seconds(20);
            continue;
        }
        uint32_t op;
        std::string json;
        while (c.Next(op, json)) {
            if (op == Ping) c.Send(Pong, json);
            else if (op == Close) {
                std::fprintf(stderr, "discord: the Discord app closed the connection: %s\n", Field(json, "message").c_str());
                c.Shut();
            } else if (Field(json, "evt") == "ERROR") {
                static int told = 0;
                if (told++ < 5) std::fprintf(stderr, "discord: the Discord app refused the presence: %s (%s)\n",
                                             Field(json, "message").c_str(), Field(json, "code").c_str());
            } else if (Field(json, "cmd") == "SET_ACTIVITY" && !told_taken && !shown.empty()) {
                told_taken = true;   // (once a connection)
                std::fprintf(stderr, "discord: the Discord app shows it\n");
            }
        }
        if (!c.Open()) { next_try = now + std::chrono::seconds(20); continue; }
        std::string said;
        const std::string activity = Activity(w, said);
        // Discord takes 5 updates in 20 s: one every 4 s at most, the latest
        if (activity != shown && now - last_sent >= std::chrono::seconds(4)) {
            if (c.Send(Frame, Command(activity))) {
                shown = activity;
                last_sent = now;
                std::fprintf(stderr, "discord: showing \"%s\"\n", said.c_str());
            }
        }
    }
    if (c.Open()) {   // the end: the presence cleared (Discord also clears it when the connection closes)
        c.Send(Frame, Command(""));
        c.Read(200);
        c.Shut();
    }
}

void Change(const std::function<void(Wanted&)>& edit) {
    std::scoped_lock hold{lock};
    edit(wanted);
    wake.notify_one();
}

#endif // !__ANDROID__

} // namespace

extern "C" const char* msr_scenario_area(const char* scenario) {
    const int area = AreaOf(scenario ? scenario : "");
    return area < 0 ? nullptr : AreaNames[area];
}

#if defined(__ANDROID__)
extern "C" void msr_discord_start(int) {}
extern "C" void msr_discord_hold(int) {}
extern "C" void msr_discord_scenario(const char*) {}
extern "C" void msr_discord_tick(void) {}
extern "C" void msr_discord_stop(void) {}
#else

extern "C" void msr_discord_start(int launcher) {
    if (!*MSR_DISCORD_APP_ID || std::getenv("MSR_NO_DISCORD")) return;   // (a build without an application; tests)
    std::scoped_lock hold{lock};
    if (started) return;
    started = true;
    wanted.launcher = launcher != 0;
    start_time = static_cast<std::int64_t>(std::time(nullptr));
    worker = std::thread(Work);
}

extern "C" void msr_discord_hold(int held) {
    Change([held](Wanted& w) { w.held = held != 0; });
}

extern "C" void msr_discord_scenario(const char* path) {
    const std::string scenario = ScenarioOf(path);
    if (scenario.empty()) return;
    Change([&scenario](Wanted& w) {
        if (w.scenario != scenario) w.metroids_left = -1;   // (read again in the new area)
        w.scenario = scenario;
    });
}

extern "C" void msr_discord_tick(void) {
    {
        std::scoped_lock hold{lock};
        if (!started || wanted.launcher || AreaOf(wanted.scenario) < 0) return;
    }
    // the Metroids left, as the touch screen's bar counts them (compositor.cpp): the total less those defeated
    const float killed = msr_item(0x7086BC, "ITEM_METROID_COUNT");
    const float total = msr_item(0x7086C0, "ITEM_METROID_TOTAL_COUNT");
    const int left = killed >= 0.f && total > 0.f ? std::max(0, static_cast<int>(total - killed)) : -1;
    std::scoped_lock hold{lock};
    if (left != wanted.metroids_left) {
        wanted.metroids_left = left;
        wake.notify_one();
    }
}

extern "C" void msr_discord_stop(void) {
    {
        std::scoped_lock hold{lock};
        if (!started) return;
        stopping = true;
        wake.notify_one();
    }
    if (worker.joinable()) worker.join();
}
#endif
