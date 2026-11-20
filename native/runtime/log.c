/* The game's log (2026-11-19): one file for each play session, user/logs/msr-native_<date>_<time>.log, the newest 10
 * kept, so a later start never overwrites the one a player reports from (until then each start wrote user/msr-native.log
 * over the last). Everything the program writes to stderr (and to stdout, when nothing else takes it) goes through a pipe
 * to a writer thread, which
 *   - starts each line with the clock time and the seconds since the start: "[19:30:05.123 +12.345] native: ...",
 *   - writes the player's home folder as %USERPROFILE% (Windows) or ~, so a player can attach the log as it is,
 *   - writes to the file at once: the game's threads only fill the pipe, never wait for the disk (main.c log_buffered:
 *     a file written line by line cost the main thread ~19 ms every 2 s at 144 FPS, 2026-11-02),
 *   - stops at 10 MB, after which only errors, crash reports and the last line are written.
 * stderr itself stays fully buffered (main.c log_buffered): the watchdog hands its lines over every 100 ms (log_tick), a
 * crash handler at once (log_crash_sync: the rest of its report then goes straight to the file), and the end of the
 * program last (log_finish: the "exit:" line; a log without one ended in a crash or a killed process). Whatever stderr
 * was given before (a console, a test's redirection such as tools/dev/shot.ps1's, RecompLauncher's capture) gets the
 * same lines, unchanged and without the times. The launcher no longer gives the game a log file (launcher.cpp StartGame).
 * The start's lines (log_start_lines) name the build and its commit, the package, how the game was started (the
 * launcher, the direct start, Steam, RecompLauncher), the folder and the system; the settings file is logged whole at
 * the start and each change after (msr/settings.cpp). */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "log.h"
#include "build_info.h"
#include <SDL_filesystem.h>
#include <SDL_stdinc.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#define LOG_KEEP 10                 /* sessions kept, this one included */
#define LOG_CAP (10u << 20)         /* a session's lines; then only errors, crash reports and the end */
#define LOG_HARD_CAP (12u << 20)    /* not even those past this */
#define LOG_NAME "msr-native_"      /* (video.c video_game_ended_opening: the game's logs begin so) */

static char g_path[1024];
static double g_t0;
static volatile int g_on, g_direct, g_finished, g_stdout_too;
static volatile unsigned g_sync_asked, g_sync_seen;   /* (sync_writer) */
static unsigned long long g_written;
static int g_capped;
static char g_home[2][512]; static size_t g_home_n[2]; static const char *g_home_as = "~";
#ifdef _WIN32
static HANDLE g_file = INVALID_HANDLE_VALUE, g_read, g_tee;
static int g_errfd = 2, g_outfd = 1;
#else
static int g_file = -1, g_read = -1, g_tee = -1;
#endif

const char *log_path(void) { return g_path; }

static double mono_s(void) {
#ifdef _WIN32
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}
static void clock_text(char *out, size_t n) {   /* 19:30:05.123, the computer's own time */
#ifdef _WIN32
    SYSTEMTIME st; GetLocalTime(&st);
    snprintf(out, n, "%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; const time_t s = ts.tv_sec; localtime_r(&s, &tm);
    snprintf(out, n, "%02d:%02d:%02d.%03ld", tm.tm_hour, tm.tm_min, tm.tm_sec, (long)(ts.tv_nsec / 1000000));
#endif
}
static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL);
#endif
}

/* ---- writing ---- */
static void file_write(const char *s, size_t n) {
#ifdef _WIN32
    DWORD w;
    while (n) { if (!WriteFile(g_file, s, (DWORD)n, &w, NULL) || !w) return; s += w; n -= w; }
#else
    while (n) { const ssize_t w = write(g_file, s, n); if (w < 0 && errno == EINTR) continue; if (w <= 0) return; s += w; n -= (size_t)w; }
#endif
}
static void tee_write(const char *s, size_t n) {
#ifdef _WIN32
    DWORD w;
    if (!g_tee) return;
    while (n) { if (!WriteFile(g_tee, s, (DWORD)n, &w, NULL) || !w) { g_tee = NULL; return; } s += w; n -= w; }
#else
    if (g_tee < 0) return;
    while (n) { const ssize_t w = write(g_tee, s, n); if (w < 0 && errno == EINTR) continue; if (w <= 0) { g_tee = -1; return; } s += w; n -= (size_t)w; }
#endif
}
static char tee_buf[1 << 16]; static size_t tee_n;   /* the lines as given, for whatever stderr was before */
static void tee_flush(void) { if (tee_n) { tee_write(tee_buf, tee_n); tee_n = 0; } }
static void tee_put(const char *s, size_t n) {
#ifdef _WIN32
    if (!g_tee) return;
#else
    if (g_tee < 0) return;
#endif
    if (tee_n + n > sizeof tee_buf) tee_flush();
    if (n > sizeof tee_buf) { tee_write(s, n); return; }
    memcpy(tee_buf + tee_n, s, n); tee_n += n;
}

static char out_buf[1 << 18]; static size_t out_n;
static void out_flush(void) { if (out_n) { file_write(out_buf, out_n); g_written += out_n; out_n = 0; } }
static void out_put(const char *s, size_t n) {
    if (out_n + n > sizeof out_buf) out_flush();
    if (n > sizeof out_buf) { file_write(s, n); g_written += n; return; }
    memcpy(out_buf + out_n, s, n); out_n += n;
}

/* the player's home folder, as Windows (both slashes) or the system writes it, unless followed by more of a name */
static void home_init(void) {
#ifdef _WIN32
    const char *h = getenv("USERPROFILE"); g_home_as = "%USERPROFILE%";
#else
    const char *h = getenv("HOME"); g_home_as = "~";
#endif
    if (!h || strlen(h) < 4 || strlen(h) >= sizeof g_home[0]) return;   /* ("/", "/" + a short name: nothing to hide) */
    snprintf(g_home[0], sizeof g_home[0], "%s", h);
    size_t n = strlen(g_home[0]);
    while (n > 1 && (g_home[0][n - 1] == '/' || g_home[0][n - 1] == '\\')) g_home[0][--n] = 0;
    g_home_n[0] = n;
    memcpy(g_home[1], g_home[0], n + 1);
    for (size_t i = 0; i < n; i++) if (g_home[1][i] == '\\') g_home[1][i] = '/';
    g_home_n[1] = strcmp(g_home[0], g_home[1]) ? n : 0;
}
static size_t home_at(const char *s, size_t left) {
    for (int k = 0; k < 2; k++) {
        const size_t n = g_home_n[k];
        if (!n || n > left) continue;
#ifdef _WIN32
        if (_strnicmp(s, g_home[k], n)) continue;
#else
        if (strncmp(s, g_home[k], n)) continue;
#endif
        const unsigned char c = n < left ? (unsigned char)s[n] : 0;
        if (isalnum(c) || c == '.' || c == '_' || c == '-') continue;   /* C:\Users\Lou is not C:\Users\Louise */
        return n;
    }
    return 0;
}

/* after the cap: the lines that still count */
static int important(const char *s, size_t n) {
    static const char *const words[] = { "crash", "error", "fail", "lost", "exception", "abort", "exit:", "hang", "watchdog" };
    char low[512]; const size_t m = n < sizeof low - 1 ? n : sizeof low - 1;
    for (size_t i = 0; i < m; i++) low[i] = (char)tolower((unsigned char)s[i]);
    low[m] = 0;
    for (size_t w = 0; w < sizeof words / sizeof *words; w++) if (strstr(low, words[w])) return 1;
    return 0;
}

static void emit_line(const char *s, size_t n) {
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
    if (g_capped) {
        if (g_written >= LOG_HARD_CAP || !important(s, n)) return;
    } else if (g_written + out_n + n + 64 > LOG_CAP) {
        g_capped = 1;
        static const char note[] = "log: this session's log reached 10 MB: from here on only errors, crash reports and its last line";
        emit_line(note, sizeof note - 1);
        if (!important(s, n)) return;
    }
    char head[48], ts[16]; clock_text(ts, sizeof ts);
    const int hn = snprintf(head, sizeof head, "[%s +%.3f] ", ts, mono_s() - g_t0);
    out_put(head, hn > 0 ? (size_t)hn : 0);
    size_t from = 0;
    for (size_t i = 0; i < n; ) {
        const size_t h = home_at(s + i, n - i);
        if (h) { out_put(s + from, i - from); out_put(g_home_as, strlen(g_home_as)); i += h; from = i; }
        else i++;
    }
    out_put(s + from, n - from);
#ifdef _WIN32
    out_put("\r\n", 2);
#else
    out_put("\n", 1);
#endif
}

#define SYNC_MARK "\001log-sync "   /* (sync_writer: a line of its own, neither written nor passed on) */
static void line_done(const char *s, size_t n) {
    if (n > sizeof SYNC_MARK - 1 && !memcmp(s, SYNC_MARK, sizeof SYNC_MARK - 1)) { g_sync_seen = (unsigned)strtoul(s + sizeof SYNC_MARK - 1, NULL, 10); return; }
    tee_put(s, n); tee_put("\n", 1);
    emit_line(s, n);
}
static void *writer(void *arg) {
    (void)arg;
    static char in[65536]; size_t have = 0;
    for (;;) {
#ifdef _WIN32
        DWORD got = 0;
        if (!ReadFile(g_read, in + have, (DWORD)(sizeof in - have), &got, NULL) || !got) break;
        const size_t n = got;
#else
        const ssize_t r = read(g_read, in + have, sizeof in - have);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        const size_t n = (size_t)r;
#endif
        size_t start = 0;
        for (size_t i = have; i < have + n; i++) if (in[i] == '\n') { line_done(in + start, i - start); start = i + 1; }
        have += n;
        if (!start && have == sizeof in) { line_done(in, have); have = 0; }   /* a line longer than the buffer */
        else if (start) { memmove(in, in + start, have - start); have -= start; }
        out_flush(); tee_flush();
    }
    if (have) line_done(in, have);
    out_flush(); tee_flush();
    return NULL;
}

/* Everything written so far is in the file once this returns (or `ms` have passed: a writer that cannot go on, the
   crashing thread itself). A numbered mark goes down the pipe after it, and the writer says when it reaches it. (Asking
   the pipe whether it is empty does not work on Windows: PeekNamedPipe waits behind the writer's pending ReadFile on the
   same handle, which waits for more lines: the program hung at its end, 2026-11-19.) Safe in a signal handler on Linux
   (write, nanosleep); on Windows, as safe as the crash report's own fflush. */
static void sync_writer(int ms) {
    const unsigned n = ++g_sync_asked;
    char mark[32]; const int k = snprintf(mark, sizeof mark, SYNC_MARK "%u\n", n);
    fflush(stderr);
    if (g_stdout_too) fflush(stdout);
#ifdef _WIN32
    _write(g_errfd, mark, (unsigned)k);
#else
    { const char *p = mark; size_t left = (size_t)k; while (left) { const ssize_t w = write(2, p, left); if (w < 0 && errno == EINTR) continue; if (w <= 0) break; p += w; left -= (size_t)w; } }
#endif
    for (int waited = 0; waited < ms && g_sync_seen < n; waited += 2) sleep_ms(2);
}

/* stderr (and stdout, if it went to the pipe) straight to the file, for what the crash report or the runtime's end write */
static void go_direct(void) {
    if (g_direct) return;
#ifdef _WIN32
    HANDLE d;
    if (!DuplicateHandle(GetCurrentProcess(), g_file, GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS)) return;
    const int fd = _open_osfhandle((intptr_t)d, _O_WRONLY | _O_BINARY);
    if (fd < 0) { CloseHandle(d); return; }
    _dup2(fd, g_errfd); SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(g_errfd));
    if (g_stdout_too) { _dup2(fd, g_outfd); SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(g_outfd)); }
    _close(fd);
#else
    dup2(g_file, 2);
    if (g_stdout_too) dup2(g_file, 1);
#endif
    g_direct = 1;
}

void log_tick(void) {
    static unsigned n;
    if (!g_on || g_direct || ++n % 4) return;
    fflush(stderr);
    if (g_stdout_too) fflush(stdout);
}

void log_crash_sync(void) {
    if (!g_on || g_direct) return;
    sync_writer(400);
    go_direct();
    char line[96], ts[16]; clock_text(ts, sizeof ts);
    const int n = snprintf(line, sizeof line, "[%s +%.3f] log: (from here on written straight to the file)\n", ts, mono_s() - g_t0);
    if (n > 0) file_write(line, (size_t)n);
}

void log_finish(const char *how) {
    if (!g_on || g_finished || g_direct) return;
    g_finished = 1;
    const unsigned s = (unsigned)(mono_s() - g_t0);
    fprintf(stderr, "exit: the session ended (%s) after %u:%02u:%02u\n", how ? how : "?", s / 3600, s / 60 % 60, s % 60);
    sync_writer(1000);
    go_direct();   /* (anything the runtime's exit writes) */
}
static void log_at_exit(void) { log_finish("the program ended"); }

/* ---- the session's file ---- */
static void make_dirs(const char *dir) {
    char p[1024]; snprintf(p, sizeof p, "%s", dir);
    for (char *c = p + 1; ; c++) {
        const char k = *c;
        if (k == '/' || k == '\\' || !k) {
            *c = 0;
#ifdef _WIN32
            _mkdir(p);
#else
            mkdir(p, 0777);
#endif
            if (!k) break;
            *c = k;
        }
    }
}
static void stamp_name(char *out, size_t n, time_t when, const char *dir, const char *suffix) {
    struct tm tm; memset(&tm, 0, sizeof tm);
#ifdef _WIN32
    const struct tm *p = localtime(&when);   /* (the start's thread only) */
    if (p) tm = *p;
#else
    localtime_r(&when, &tm);
#endif
    snprintf(out, n, "%s/" LOG_NAME "%04d-%02d-%02d_%02d-%02d-%02d%s.log", dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, suffix);
}
/* an earlier version's single log (user/msr-native.log, -crashed.log) joins the others, named by when it was written */
static void adopt_old(const char *old, const char *dir, const char *suffix) {
    struct stat st; char to[1100];
    if (stat(old, &st)) return;
    stamp_name(to, sizeof to, st.st_mtime, dir, suffix);
    if (!stat(to, &st)) return;
    rename(old, to);
}
static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)b, *(char *const *)a); }   /* newest first */
static void prune(const char *dir, int keep) {   /* the newest `keep` stay */
    char *names[512]; int n = 0;
#ifdef _WIN32
    char pat[1100]; snprintf(pat, sizeof pat, "%s/" LOG_NAME "*.log", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < 512) names[n++] = strdup(fd.cFileName); } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir); struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) && n < 512) {
        const size_t l = strlen(e->d_name);
        if (!strncmp(e->d_name, LOG_NAME, sizeof LOG_NAME - 1) && l > 4 && !strcmp(e->d_name + l - 4, ".log")) names[n++] = strdup(e->d_name);
    }
    closedir(d);
#endif
    qsort(names, (size_t)n, sizeof *names, name_cmp);
    for (int i = 0; i < n; i++) {
        if (i >= keep && names[i]) { char p[1100]; snprintf(p, sizeof p, "%s/%s", dir, names[i]); remove(p); }
        free(names[i]);
    }
}

#ifdef _WIN32
static void quiet_iph(const wchar_t *e, const wchar_t *f, const wchar_t *file, unsigned line, uintptr_t r) {
    (void)e; (void)f; (void)file; (void)line; (void)r;
}
static int win_useful(HANDLE h) {   /* a file, a pipe or a console: worth the same lines; not NUL, not none */
    if (!h || h == INVALID_HANDLE_VALUE || h == (HANDLE)(intptr_t)-2) return 0;
    const DWORD t = GetFileType(h); DWORD mode;
    return t == FILE_TYPE_DISK || t == FILE_TYPE_PIPE || (t == FILE_TYPE_CHAR && GetConsoleMode(h, &mode));
}
static HANDLE stream_handle(FILE *f) {
    const int fd = _fileno(f);
    return fd >= 0 ? (HANDLE)_get_osfhandle(fd) : INVALID_HANDLE_VALUE;
}
#else
static int fd_useful(int fd) {
    struct stat st, nul;
    if (fstat(fd, &st)) return 0;
    return !(S_ISCHR(st.st_mode) && !stat("/dev/null", &nul) && st.st_rdev == nul.st_rdev);
}
#endif

int log_start(const char *dir) {
    if (g_on) return 0;
    g_t0 = mono_s();
    home_init();
    make_dirs(dir);
    {   /* the folder above: an earlier version's single log */
        char parent[1024]; snprintf(parent, sizeof parent, "%s", dir);
        char *slash = strrchr(parent, '/'); if (slash) *slash = 0; else snprintf(parent, sizeof parent, ".");
        char old[1100];
        snprintf(old, sizeof old, "%s/msr-native.log", parent); adopt_old(old, dir, "");
        snprintf(old, sizeof old, "%s/msr-native-crashed.log", parent); adopt_old(old, dir, "-crashed");
    }
    prune(dir, LOG_KEEP - 1);
    const time_t now = time(NULL);
    for (int i = 1; i < 10; i++) {   /* two starts in the same second: _2, _3... */
        char suffix[8] = "";
        if (i > 1) snprintf(suffix, sizeof suffix, "_%d", i);
        stamp_name(g_path, sizeof g_path, now, dir, suffix);
#ifdef _WIN32
        g_file = CreateFileA(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
#else
        g_file = open(g_path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0644);
        if (g_file >= 0 || errno != EEXIST) break;
#endif
    }
#ifdef _WIN32
    if (g_file == INVALID_HANDLE_VALUE) { g_path[0] = 0; return -1; }
    _invalid_parameter_handler iph = _set_invalid_parameter_handler(quiet_iph);
    HANDLE r = NULL, w = NULL;
    if (!CreatePipe(&r, &w, NULL, 1 << 20)) { _set_invalid_parameter_handler(iph); CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; g_path[0] = 0; return -1; }
    fflush(stdout); fflush(stderr);
    HANDLE cur = stream_handle(stderr);
    if (win_useful(cur)) DuplicateHandle(GetCurrentProcess(), cur, GetCurrentProcess(), &g_tee, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (cur == INVALID_HANDLE_VALUE || cur == (HANDLE)(intptr_t)-2) {   /* a window program started without one: a stream to replace */
        if (freopen("NUL", "w", stderr) && !getenv("PWR_LOG_UNBUFFERED")) setvbuf(stderr, NULL, _IOFBF, 1 << 16);
    }
    HANDLE out = stream_handle(stdout);
    g_stdout_too = !win_useful(out);
    if (g_stdout_too && (out == INVALID_HANDLE_VALUE || out == (HANDLE)(intptr_t)-2)) freopen("NUL", "w", stdout);
    const int wfd = _open_osfhandle((intptr_t)w, _O_WRONLY | _O_BINARY);
    g_errfd = _fileno(stderr); g_outfd = _fileno(stdout);
    if (wfd < 0 || g_errfd < 0 || _dup2(wfd, g_errfd)) {
        if (wfd >= 0) _close(wfd); else CloseHandle(w);
        CloseHandle(r); if (g_tee) CloseHandle(g_tee); g_tee = NULL;
        _set_invalid_parameter_handler(iph); CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; g_path[0] = 0; return -1;
    }
    if (g_stdout_too && (g_outfd < 0 || _dup2(wfd, g_outfd))) g_stdout_too = 0;
    _close(wfd);
    _setmode(g_errfd, _O_BINARY); SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(g_errfd));
    if (g_stdout_too) { _setmode(g_outfd, _O_BINARY); SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(g_outfd)); }
    _set_invalid_parameter_handler(iph);
    g_read = r;
#else
    if (g_file < 0) { g_path[0] = 0; return -1; }
    int fds[2];
    if (pipe(fds)) { close(g_file); g_file = -1; g_path[0] = 0; return -1; }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC); fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#ifdef F_SETPIPE_SZ
    fcntl(fds[1], F_SETPIPE_SZ, 1 << 20);   /* (Linux: room for bursts while the writer waits for the disk) */
#endif
    fflush(stdout); fflush(stderr);
    if (fd_useful(2)) { g_tee = dup(2); if (g_tee >= 0) fcntl(g_tee, F_SETFD, FD_CLOEXEC); }
    g_stdout_too = !fd_useful(1);
    dup2(fds[1], 2);
    if (g_stdout_too) dup2(fds[1], 1);
    close(fds[1]);
    g_read = fds[0];
#endif
    pthread_t th;
    if (pthread_create(&th, NULL, writer, NULL)) { go_direct(); g_on = 1; return 0; }   /* (straight to the file, without the times) */
    pthread_detach(th);
    g_on = 1;
    atexit(log_at_exit);
    return 0;
}

/* ---- the start's lines ---- */
static void os_text(char *out, size_t n) {
#ifdef _WIN32
    OSVERSIONINFOW v; memset(&v, 0, sizeof v); v.dwOSVersionInfoSize = sizeof v;
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    typedef LONG (WINAPI *GetVersionFn)(OSVERSIONINFOW *);
    GetVersionFn get = nt ? (GetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    if (get) get(&v);
    char shown[32] = ""; DWORD size = sizeof shown;   /* "25H2" */
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion", RRF_RT_REG_SZ, NULL, shown, &size) != ERROR_SUCCESS)
        shown[0] = 0;
    const char *name = v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000 ? "Windows 11" : v.dwMajorVersion == 10 ? "Windows 10" : "Windows";
    typedef const char *(*WineFn)(void);
    WineFn wine = nt ? (WineFn)(void *)GetProcAddress(nt, "wine_get_version") : NULL;
    snprintf(out, n, "%s%s%s (version %lu.%lu, build %lu)%s%s", name, shown[0] ? " " : "", shown, (unsigned long)v.dwMajorVersion,
             (unsigned long)v.dwMinorVersion, (unsigned long)v.dwBuildNumber, wine ? ", under Wine " : "", wine ? wine() : "");
#elif defined(__ANDROID__)
    char rel[PROP_VALUE_MAX] = "", sdk[PROP_VALUE_MAX] = "", maker[PROP_VALUE_MAX] = "", model[PROP_VALUE_MAX] = "", soc[PROP_VALUE_MAX] = "";
    __system_property_get("ro.build.version.release", rel); __system_property_get("ro.build.version.sdk", sdk);
    __system_property_get("ro.product.manufacturer", maker); __system_property_get("ro.product.model", model);
    if (__system_property_get("ro.soc.model", soc) <= 0) __system_property_get("ro.board.platform", soc);
    snprintf(out, n, "Android %s (API %s) on a %s %s, chip %s", rel, sdk, maker, model, soc[0] ? soc : "?");
#elif defined(__APPLE__)
    char ver[64] = "", model[64] = ""; size_t a = sizeof ver, b = sizeof model;
    if (sysctlbyname("kern.osproductversion", ver, &a, NULL, 0)) snprintf(ver, sizeof ver, "?");
    if (sysctlbyname("hw.model", model, &b, NULL, 0)) snprintf(model, sizeof model, "?");
    snprintf(out, n, "macOS %s on a %s", ver, model);
#else
    char pretty[256] = "Linux", line[256], board[64] = "", vendor[64] = "";
    FILE *f = fopen("/etc/os-release", "r");
    while (f && fgets(line, sizeof line, f)) {
        if (strncmp(line, "PRETTY_NAME=", 12)) continue;
        char *v = line + 12; if (*v == '"') v++;
        v[strcspn(v, "\"\r\n")] = 0; snprintf(pretty, sizeof pretty, "%s", v); break;
    }
    if (f) fclose(f);
    struct utsname u; const int un = uname(&u);
    if ((f = fopen("/sys/devices/virtual/dmi/id/product_name", "r"))) { if (fgets(board, sizeof board, f)) board[strcspn(board, "\r\n")] = 0; fclose(f); }
    if ((f = fopen("/sys/devices/virtual/dmi/id/sys_vendor", "r"))) { if (fgets(vendor, sizeof vendor, f)) vendor[strcspn(vendor, "\r\n")] = 0; fclose(f); }
    const int deck = !strcmp(vendor, "Valve") && (!strcmp(board, "Jupiter") || !strcmp(board, "Galileo"));
    snprintf(out, n, "%s, kernel %s%s%s", pretty, un ? "?" : u.release,
             deck ? (!strcmp(board, "Galileo") ? ", a Steam Deck OLED" : ", a Steam Deck LCD") : "",
             !deck && getenv("SteamDeck") && !strcmp(getenv("SteamDeck"), "1") ? ", Steam says a Steam Deck" : "");
#endif
}

static int file_exists(const char *p) { struct stat st; return !stat(p, &st); }

void log_start_lines(void) {
    char *base = SDL_GetBasePath();   /* the program's folder, with a separator at the end */
    char package[600] = "", probe[1100];
    snprintf(probe, sizeof probe, "%sCMakeCache.txt", base ? base : "");
    const int source = base && file_exists(probe);
#if defined(__ANDROID__)
    snprintf(package, sizeof package, "the APK");
#elif defined(__APPLE__)
    snprintf(package, sizeof package, "%s", source ? "built from source" : base && strstr(base, ".app/") ? "the macOS app" : "a macOS build");
#elif defined(_WIN32)
    snprintf(package, sizeof package, "%s", source ? "built from source" : "the Windows release");
#else
    const char *appimage = getenv("APPIMAGE");
    if (appimage) { const char *b = strrchr(appimage, '/'); snprintf(package, sizeof package, "the AppImage %s", b ? b + 1 : appimage); }
    else snprintf(package, sizeof package, "%s", source ? "built from source" : "a Linux build");
#endif
    fprintf(stderr, "native: msr-native %s, %s, build %s (%s), %s\n", MSR_NATIVE_VERSION,
#if defined(__ANDROID__)
            "Android"
#elif defined(_WIN32)
            "Windows"
#elif defined(__APPLE__)
            "macOS"
#else
            "Linux"
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
            " arm64"
#elif defined(__x86_64__) || defined(_M_X64)
            " x64"
#else
            ""
#endif
            , MSR_BUILD_COMMIT, MSR_BUILD_DATE, package);
    {   /* how: the launcher's Play, the direct start (MSR_STARTED_BY, launcher.cpp), else directly; through Steam (its own
           variables in the game's environment), RecompLauncher (RECOMPLAUNCHER) */
        const char *by = getenv("MSR_STARTED_BY");
        const char *who =
#ifdef __ANDROID__
            "the app";
#else
            by && !strcmp(by, "launcher") ? "the launcher's Play" : by && !strcmp(by, "direct") ? "the direct start (msr-native-direct)"
            : "itself (msr-native --play, no launcher)";
#endif
        static const char *const steam_vars[] = { "SteamGameId", "SteamOverlayGameId", "SteamClientLaunch", "SteamAppId", "SteamEnv" };
        int steam = 0;
        for (size_t i = 0; i < sizeof steam_vars / sizeof *steam_vars; i++) if (getenv(steam_vars[i])) steam = 1;
        const char *rl = getenv("RECOMPLAUNCHER"), *rlv = getenv("RECOMPLAUNCHER_VERSION");
        fprintf(stderr, "start: started by %s%s%s%s%s\n", who, steam ? ", through Steam" : ", not through Steam",
                rl ? ", through RecompLauncher" : "", rl && rlv ? " " : "", rl && rlv ? rlv : "");
    }
    {
        char cwd[1024] = "?";
#ifdef _WIN32
        if (!_getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "?");
        const int same = base && !_strnicmp(cwd, base, strlen(cwd)) && strlen(base) == strlen(cwd) + 1;
#else
        if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "?");
        const int same = base && !strncmp(cwd, base, strlen(cwd)) && strlen(base) == strlen(cwd) + 1;
#endif
        fprintf(stderr, "start: the folder %s (user/ there%s)\n", cwd, same ? ", beside the program: portable" : "");
    }
    if (g_on) fprintf(stderr, "start: this session's log: %s (the newest %d are kept; one without an \"exit:\" line at its end "
                      "ended in a crash, or its process was ended)\n", g_path, LOG_KEEP);
    else fprintf(stderr, "start: no log file of its own (user/logs could not be written): these lines go only where stderr goes\n");
    char os[400]; os_text(os, sizeof os);
    fprintf(stderr, "pc: %s\n", os);
    if (base) SDL_free(base);
}

/* ---- the settings ---- */
char *log_read_text(const char *path) {
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (!f) return NULL;
    char *t = malloc(1 << 20); size_t n = t ? fread(t, 1, (1 << 20) - 1, f) : 0;
    fclose(f);
    if (t) t[n] = 0;
    return t;
}
typedef struct { char name[128]; char value[160]; } IniEntry;
/* "[section] key" = value, each line of an .ini; malloc'd, *count set */
static IniEntry *ini_entries(const char *text, int *count) {
    *count = 0;
    if (!text) return NULL;
    int cap = 64; IniEntry *e = malloc(sizeof *e * (size_t)cap);
    char section[64] = "";
    for (const char *l = text; e && *l; ) {
        const char *end = l + strcspn(l, "\r\n");
        const char *a = l; while (a < end && (*a == ' ' || *a == '\t')) a++;
        if (a < end && *a == '[') {
            const char *b = memchr(a, ']', (size_t)(end - a));
            if (b) snprintf(section, sizeof section, "%.*s", (int)(b - a - 1), a + 1);
        } else if (a < end && *a != ';' && *a != '#') {
            const char *eq = memchr(a, '=', (size_t)(end - a));
            if (eq) {
                if (*count == cap) { cap *= 2; IniEntry *g = realloc(e, sizeof *e * (size_t)cap); if (!g) break; e = g; }
                const char *ke = eq; while (ke > a && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                const char *v = eq + 1; while (v < end && (*v == ' ' || *v == '\t')) v++;
                snprintf(e[*count].name, sizeof e[*count].name, "[%s] %.*s", section, (int)(ke - a), a);
                snprintf(e[*count].value, sizeof e[*count].value, "%.*s", (int)(end - v), v);
                (*count)++;
            }
        }
        l = end; while (*l == '\r' || *l == '\n') l++;
    }
    return e;
}
void log_settings_file(const char *path) {
    char *t = log_read_text(path); int n = 0;
    IniEntry *e = ini_entries(t, &n);
    char line[400]; size_t at = 0; char section[64] = "";
    for (int i = 0; i < n; i++) {
        char item[300];
        const char *sp = strchr(e[i].name, ' ');
        const size_t sl = sp ? (size_t)(sp - e[i].name) : 0;
        const int new_section = sl && (strlen(section) != sl || strncmp(section, e[i].name, sl));
        if (new_section) snprintf(section, sizeof section, "%.*s", (int)sl, e[i].name);
        snprintf(item, sizeof item, "%s%s=%s", new_section ? e[i].name : "", new_section ? "" : (sp ? sp + 1 : e[i].name), e[i].value);
        if (at && at + strlen(item) + 2 > 200) { fprintf(stderr, "settings: %s\n", line); at = 0; }
        at += (size_t)snprintf(line + at, sizeof line - at, "%s%s", at ? ", " : "", item);
    }
    if (at) fprintf(stderr, "settings: %s\n", line);
    if (!n) fprintf(stderr, "settings: none in %s (the defaults)\n", path ? path : "(no file)");
    free(e); free(t);
}
void log_settings_changes(const char *before, const char *after) {
    int nb = 0, na = 0;
    IniEntry *b = ini_entries(before, &nb), *a = ini_entries(after, &na);
    char line[1200]; size_t at = 0;
#define CHANGE_PUT(...) do { if (at < sizeof line) { const int w_ = snprintf(line + at, sizeof line - at, __VA_ARGS__); if (w_ > 0) at += (size_t)w_; } } while (0)
    for (int i = 0; i < na; i++) {
        int j = 0; while (j < nb && strcmp(b[j].name, a[i].name)) j++;
        if (j < nb && !strcmp(b[j].value, a[i].value)) continue;
        CHANGE_PUT("%s%s %s -> %s", at ? "; " : "", a[i].name, j < nb ? b[j].value : "(new)", a[i].value);
    }
    for (int j = 0; j < nb; j++) {
        int i = 0; while (i < na && strcmp(b[j].name, a[i].name)) i++;
        if (i == na) CHANGE_PUT("%s%s %s -> (removed)", at ? "; " : "", b[j].name, b[j].value);
    }
#undef CHANGE_PUT
    if (at) fprintf(stderr, "settings: changed: %s\n", line);
    free(a); free(b);
}
