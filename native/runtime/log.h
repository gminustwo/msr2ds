/* The game's log, one file for each play session (log.c) */
#ifndef MSR_LOG_H
#define MSR_LOG_H
#ifdef __cplusplus
extern "C" {
#endif

/* The session's own log in dir (user/logs): the newest 10 kept; stderr (and stdout, when nothing else takes it) goes
   through the writer from here on. 0 when on; else stderr stays as it was given. */
int log_start(const char *dir);
const char *log_path(void);             /* this session's file, or "" */
void log_start_lines(void);             /* the log's first lines: the build, how it was started, the folder, the system */
void log_tick(void);                    /* the watchdog, every 25 ms: the lines so far to the writer, every 100 ms */
void log_crash_sync(void);              /* a crash: the lines so far reach the file; later ones go straight to it */
void log_finish(const char *how);       /* the last line ("exit: ..."), then straight to the file */
/* The settings file, all of it, at the start; and what a save changed (before and after: the file's text, malloc'd) */
void log_settings_file(const char *path);
char *log_read_text(const char *path);
void log_settings_changes(const char *before, const char *after);

#ifdef __cplusplus
}
#endif
#endif
