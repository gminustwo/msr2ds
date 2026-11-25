/* The Android app's side of the program (android.c; the app is native/android, its Java side MsrActivity). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The folder holding user/: the app's folder on the shared storage (Android/data/<app>/files), which a PC reaches over
   USB, so the game file and the native code built on the PC can be copied in; the app's private folder without it. */
const char *msr_android_root(void);
/* The user folder's place (#39): the phone's storage (the app's folder there) or the SD card (the app's folder there).
   msr_android_root is the chosen one, read when the process starts; msr_android_phone_root the phone's; the SD card's
   from msr_android_sd_root (1, its path in out; 0 when there is no SD card). msr_android_root_missing: the folder chosen
   before when it is not there now (the card taken out: the phone's storage is in use), else "". msr_android_set_root
   records the place for the next start ("" or the phone's: the phone's storage); 0 when done. */
const char *msr_android_phone_root(void);
int msr_android_sd_root(char *out, size_t n);
const char *msr_android_root_missing(void);
int msr_android_set_root(const char *path);
/* The native code, copied from where the player put it (user/msr/recomp-android/libmsr_aot.so) to the app's private
   folder: Android loads code only from there. The copy's path, or the original's when the copy failed. */
const char *msr_android_module(const char *path);
/* The game's process is started again by the app: with these arguments (argc 0: the launcher). Does not return. */
void msr_android_relaunch(int argc, const char *const *argv);
/* GPU drivers the player installed (the launcher's GPU driver, vk_load.c): the app's private folder holding them, one
   folder each (files/gpu_drivers/<name>/: meta.json and the driver's libraries, as MsrActivity unpacked them). */
const char *msr_android_gpu_drivers(void);
/* The app's native library folder (ApplicationInfo.nativeLibraryDir), where libadrenotools' hooks are; "" if unknown */
const char *msr_android_native_libs(void);
/* The system's file picker for a GPU driver package (.zip); MsrActivity unpacks the one chosen and tells the launcher
   (msr_gpu_driver_installed, launcher.cpp) */
void msr_android_pick_gpu_driver(void);
/* A page in the system's browser (the launcher's Discord button, the bug reports' sign-in page and issue) */
void msr_android_open_url(const char *url);
/* An HTTPS request for the launcher's bug reports (MsrActivity.httpRequest, on the calling thread): `headers` one a
   line, the body read from `body_path` ("" none), the answer's body written to `out_path`. Its status, -1 for none. */
int msr_android_http(const char *method, const char *url, const char *headers, const char *body_path, const char *out_path);

/* The 3DS's bottom screen on a second display (a dual-screen handheld's lower screen: MsrActivity's BottomDisplay, a
   Presentation there). msr_android_bottom_display asks the app for it or lets it go. video.c then takes the surface
   with msr_android_bottom_poll each frame: the window (acquired for the caller, NULL: none now) and its size, when it
   changed since `*serial` (1, *serial updated; 0: no change). msr_android_bottom_held says whether video.c still draws
   into a surface it took (the app waits for it to let go before the surface goes). Touches on that display come as SDL
   events of type msr_android_bottom_event() (0 before the first request): user.code 1 down, 0 up, data1 and data2 the
   place in the surface's pixels (intptr_t). */
void msr_android_bottom_display(int on);
int msr_android_bottom_poll(unsigned *serial, void **window, int *w, int *h);
void msr_android_bottom_held(int held);
unsigned msr_android_bottom_event(void);

#ifdef __cplusplus
}
#endif
