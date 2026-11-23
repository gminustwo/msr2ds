// Moving the user folder between two places (user_move.h): #39, 2026-11-20 ("I would love to have a custom user folder
// to have the rom and the textures in the SD card instead of the internal storage"). The launcher's User folder row on
// Android moves it between the phone's storage and the SD card, each the app's own folder there (no permission needed).
// A move copies everything first, into user.moving at the new place, checks the copy file by file, and only then puts it
// in place there; the old place is removed by the caller once the new one is recorded. A move that fails at any step
// (a full card, a card taken out) removes its copy and leaves the old place as it was.
#include "user_move.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace MSR::UserMove {

namespace {

constexpr std::uint64_t Margin = 64ull << 20;   // free space left over on the new place

bool IsCache(const fs::path& p) {
    const std::string name = p.filename().string();
    return name.size() > 8 && name.compare(0, 4, "pwr_") == 0 && name.compare(name.size() - 4, 4, ".bin") == 0;
}

std::vector<fs::path> Caches(const fs::path& root) {
    std::vector<fs::path> found;
    std::error_code e;
    for (fs::directory_iterator it(root, e), end; !e && it != end; it.increment(e))
        if (it->is_regular_file(e) && IsCache(it->path())) found.push_back(it->path());
    return found;
}

std::string Gb(std::uint64_t bytes) {
    char text[32];
    if (bytes >= (1ull << 30)) std::snprintf(text, sizeof text, "%.1f GB", bytes / 1073741824.0);
    else std::snprintf(text, sizeof text, "%.0f MB", bytes / 1048576.0 + 0.5);
    return text;
}

// One file, copied in 4 MB pieces, its time kept (the app's copy of the native code is checked by size and time)
std::string CopyFile(const fs::path& from, const fs::path& to, std::uint64_t& done, std::uint64_t total,
                     const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in) return "could not be read";
    if (!out) return "could not be written";
    static thread_local std::vector<char> buffer(4u << 20);
    std::uint64_t copied = 0;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize n = in.gcount();
        if (n <= 0) break;
        if (!out.write(buffer.data(), n)) return "could not be written (the place is full or was taken out)";
        copied += static_cast<std::uint64_t>(n);
        done += static_cast<std::uint64_t>(n);
        if (progress) progress(done, total);
    }
    if (in.bad()) return "could not be read";
    out.close();
    if (!out) return "could not be written (the place is full or was taken out)";
    std::error_code e, f;
    const std::uint64_t size = fs::file_size(from, e);
    if (e || copied != size || fs::file_size(to, f) != size || f) return "was not copied whole";
    fs::last_write_time(to, fs::last_write_time(from, e), e);
    return {};
}

} // namespace

Size Measure(const fs::path& root) {
    Size size;
    std::error_code e;
    for (fs::recursive_directory_iterator it(root / "user", e), end; !e && it != end; it.increment(e)) {
        if (!it->is_regular_file(e)) continue;
        size.bytes += it->file_size(e);
        ++size.files;
    }
    for (const fs::path& cache : Caches(root)) {
        size.bytes += fs::file_size(cache, e);
        ++size.files;
    }
    return size;
}

std::string Move(const fs::path& from, const fs::path& to, const std::function<void(std::uint64_t, std::uint64_t)>& progress,
                 fs::path* aside) {
    if (aside) aside->clear();
    std::error_code e;
    const fs::path source = from / "user";
    if (!fs::is_directory(source, e)) return "There is no user folder at " + source.string() + ".";
    if (fs::equivalent(from, to, e)) return "The user folder is there already.";
    fs::create_directories(to, e);
    const Size size = Measure(from);
    const fs::space_info space = fs::space(to, e);
    if (!e && space.available < size.bytes + Margin)
        return "Not enough room: the user folder takes " + Gb(size.bytes) + ", and " + Gb(space.available) + " is free there.";

    // the copy, beside the place it goes
    const fs::path staging = to / "user.moving";
    fs::remove_all(staging, e);   // (a move stopped before: the app was closed)
    std::uint64_t done = 0, files = 0;
    const auto fail = [&](const std::string& why) {
        std::error_code r;
        fs::remove_all(staging, r);
        for (const fs::path& cache : Caches(from)) fs::remove(to / (cache.filename().string() + ".moving"), r);
        return why;
    };
    if (!fs::create_directories(staging, e) && !fs::is_directory(staging, e))
        return fail("Could not make a folder at " + to.string() + " (" + e.message() + ").");
    for (fs::recursive_directory_iterator it(source, e), end; it != end; it.increment(e)) {
        if (e) return fail("Could not read the user folder (" + e.message() + ").");
        const fs::path rel = fs::relative(it->path(), source, e);
        if (it->is_directory(e)) {
            fs::create_directories(staging / rel, e);
            if (e) return fail("Could not make " + rel.generic_string() + " there (" + e.message() + ").");
        } else if (it->is_regular_file(e)) {
            const std::string why = CopyFile(it->path(), staging / rel, done, size.bytes, progress);
            if (!why.empty()) return fail(rel.generic_string() + " " + why + ".");
            ++files;
        }
    }
    for (const fs::path& cache : Caches(from)) {   // the shader caches: copied as .moving, renamed with the rest
        const std::string why = CopyFile(cache, to / (cache.filename().string() + ".moving"), done, size.bytes, progress);
        if (!why.empty()) return fail(cache.filename().string() + " " + why + ".");
        ++files;
    }
    // checked: every file there, each its size
    Size staged;
    for (fs::recursive_directory_iterator it(staging, e), end; !e && it != end; it.increment(e)) {
        if (!it->is_regular_file(e)) continue;
        const fs::path rel = fs::relative(it->path(), staging, e);
        if (fs::file_size(it->path(), e) != fs::file_size(source / rel, e) || e)
            return fail(rel.generic_string() + " is not the same size as the original.");
        staged.bytes += it->file_size(e);
        ++staged.files;
    }
    if (files != size.files || staged.files + Caches(from).size() != size.files)
        return fail("The copy has " + std::to_string(staged.files) + " files of " + std::to_string(size.files) + ".");

    // in place: a user folder already there set aside, the copy renamed
    const fs::path target = to / "user";
    if (fs::exists(target, e)) {
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", std::localtime(&now));
        const fs::path old = to / ("user-before-" + std::string(stamp));
        fs::rename(target, old, e);
        if (e) return fail("A user folder is at " + target.string() + " already, and it could not be set aside (" + e.message() + ").");
        if (aside) *aside = old;
    }
    fs::rename(staging, target, e);
    if (e) {
        const std::string why = "The copy could not be put in place (" + e.message() + ").";
        std::error_code r;
        if (aside && !aside->empty()) fs::rename(*aside, target, r);
        return fail(why);
    }
    for (const fs::path& cache : Caches(from)) {
        const fs::path name = to / cache.filename();
        fs::remove(name, e);
        fs::rename(to / (cache.filename().string() + ".moving"), name, e);
    }
    // the game file's path, when it was in the user folder
    const fs::path path_file = target / "msr" / "game-path.txt";
    std::string game;
    {
        std::ifstream f(path_file);
        std::getline(f, game);
    }
    while (!game.empty() && (game.back() == '\r' || game.back() == '\n' || game.back() == ' ')) game.pop_back();
    const std::string old_prefix = source.generic_string() + "/";
    if (game.compare(0, old_prefix.size(), old_prefix) == 0) {
        std::ofstream f(path_file, std::ios::binary | std::ios::trunc);
        f << (target / fs::path(game.substr(old_prefix.size()))).generic_string() << "\n";
    }
    return {};
}

void Undo(const fs::path& to, const fs::path& aside) {
    std::error_code e;
    fs::remove_all(to / "user", e);
    if (!aside.empty()) fs::rename(aside, to / "user", e);
}

std::string Remove(const fs::path& from) {
    std::error_code e;
    fs::remove_all(from / "user", e);
    for (const fs::path& cache : Caches(from)) fs::remove(cache, e);
    if (fs::exists(from / "user")) return "Some of the old user folder at " + (from / "user").string() + " could not be removed.";
    return {};
}

} // namespace MSR::UserMove
