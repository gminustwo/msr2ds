/* The Android app's side of the program (android.h). The app (native/android) is SDL's Android activity around this
 * program (libmain.so): MsrActivity passes the arguments of the process it runs, and starts the process again with
 * other ones (relaunch), so the launcher and the game each run in a process of their own, as on a PC. */
#include "android.h"
#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>
#include <android/native_window_jni.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The user folder's place (#39, 2026-11-20: "a custom user folder to have the rom and the textures in the SD card"): the
   phone's storage or the SD card, each the app's own folder there (no permission needed). The choice is kept in the
   app's private folder, user-place.txt: the chosen folder's path, no file for the phone's storage. A chosen folder that
   is not there (the card taken out) leaves the phone's storage in use, and the launcher says so. */
static int jni_failed(JNIEnv *env);
static char place_missing[4096];
static const char *place_file(void) {
    static char path[4096];
    if (!path[0]) {
        const char *internal = SDL_AndroidGetInternalStoragePath();
        snprintf(path, sizeof path, "%s/user-place.txt", internal ? internal : ".");
    }
    return path;
}

const char *msr_android_phone_root(void) {
    static char root[4096];
    if (!root[0]) {
        const char *dir = (SDL_AndroidGetExternalStorageState() & SDL_ANDROID_EXTERNAL_STORAGE_WRITE)
                              ? SDL_AndroidGetExternalStoragePath() : NULL;
        if (!dir) dir = SDL_AndroidGetInternalStoragePath();
        snprintf(root, sizeof root, "%s", dir ? dir : ".");
    }
    return root;
}

const char *msr_android_root(void) {
    static char root[4096];
    if (!root[0]) {
        char chosen[4096] = "";
        FILE *f = fopen(place_file(), "r");
        if (f) {
            if (!fgets(chosen, sizeof chosen, f)) chosen[0] = 0;
            fclose(f);
            chosen[strcspn(chosen, "\r\n")] = 0;
        }
        struct stat st;
        if (chosen[0] && !stat(chosen, &st) && S_ISDIR(st.st_mode) && !access(chosen, W_OK)) {
            snprintf(root, sizeof root, "%s", chosen);
        } else {
            if (chosen[0]) snprintf(place_missing, sizeof place_missing, "%s", chosen);
            snprintf(root, sizeof root, "%s", msr_android_phone_root());
        }
    }
    return root;
}

const char *msr_android_root_missing(void) {
    msr_android_root();
    return place_missing;
}

int msr_android_set_root(const char *path) {
    if (!path || !path[0] || !strcmp(path, msr_android_phone_root())) return unlink(place_file()) && errno != ENOENT ? -1 : 0;
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.new", place_file());
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    const int ok = fprintf(f, "%s\n", path) > 0;
    if (fclose(f) || !ok || rename(tmp, place_file())) { unlink(tmp); return -1; }
    return 0;
}

/* the SD card's folder for the app: getExternalFilesDirs' entries after the first (the phone's storage), the first on
   removable storage that is mounted */
int msr_android_sd_root(char *out, size_t n) {
    out[0] = 0;
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return 0;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID dirs_m = (*env)->GetMethodID(env, cls, "getExternalFilesDirs", "(Ljava/lang/String;)[Ljava/io/File;");
    jobjectArray dirs = dirs_m ? (jobjectArray)(*env)->CallObjectMethod(env, activity, dirs_m, NULL) : NULL;
    if (jni_failed(env)) dirs = NULL;
    jclass environment = (*env)->FindClass(env, "android/os/Environment");
    jclass file = (*env)->FindClass(env, "java/io/File");
    if (jni_failed(env)) environment = file = NULL;
    jmethodID removable = environment ? (*env)->GetStaticMethodID(env, environment, "isExternalStorageRemovable", "(Ljava/io/File;)Z") : NULL;
    jmethodID state = environment ? (*env)->GetStaticMethodID(env, environment, "getExternalStorageState", "(Ljava/io/File;)Ljava/lang/String;") : NULL;
    jmethodID absolute = file ? (*env)->GetMethodID(env, file, "getAbsolutePath", "()Ljava/lang/String;") : NULL;
    if (jni_failed(env)) removable = state = absolute = NULL;
    const jsize count = dirs ? (*env)->GetArrayLength(env, dirs) : 0;
    for (jsize i = 1; i < count && removable && state && absolute && !out[0]; i++) {
        jobject dir = (*env)->GetObjectArrayElement(env, dirs, i);
        if (!dir) continue;   /* a volume not mounted now */
        const jboolean is_removable = (*env)->CallStaticBooleanMethod(env, environment, removable, dir);
        jstring s = jni_failed(env) || !is_removable ? NULL : (jstring)(*env)->CallStaticObjectMethod(env, environment, state, dir);
        if (jni_failed(env)) s = NULL;
        int mounted = 0;
        if (s) {
            const char *c = (*env)->GetStringUTFChars(env, s, NULL);
            mounted = c && !strcmp(c, "mounted");
            if (c) (*env)->ReleaseStringUTFChars(env, s, c);
            (*env)->DeleteLocalRef(env, s);
        }
        jstring path = mounted ? (jstring)(*env)->CallObjectMethod(env, dir, absolute) : NULL;
        if (jni_failed(env)) path = NULL;
        if (path) {
            const char *c = (*env)->GetStringUTFChars(env, path, NULL);
            if (c) { snprintf(out, n, "%s", c); (*env)->ReleaseStringUTFChars(env, path, c); }
            (*env)->DeleteLocalRef(env, path);
        }
        (*env)->DeleteLocalRef(env, dir);
    }
    if (dirs) (*env)->DeleteLocalRef(env, dirs);
    if (environment) (*env)->DeleteLocalRef(env, environment);
    if (file) (*env)->DeleteLocalRef(env, file);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    return out[0] != 0;
}

const char *msr_android_module(const char *path) {
    static char copy[4096];
    const char *internal = SDL_AndroidGetInternalStoragePath();
    struct stat src, dst;
    if (!internal || stat(path, &src)) return path;
    char dir[4096], tmp[4200];
    snprintf(dir, sizeof dir, "%s/recomp", internal);
    mkdir(dir, 0700);
    snprintf(copy, sizeof copy, "%s/libmsr_aot.so", dir);
    if (!stat(copy, &dst) && dst.st_size == src.st_size && dst.st_mtime == src.st_mtime) return copy;   /* copied before */
    snprintf(tmp, sizeof tmp, "%s.new", copy);
    FILE *in = fopen(path, "rb"), *out = in ? fopen(tmp, "wb") : NULL;
    int ok = in && out;
    static char buf[1 << 20];
    for (size_t n; ok && (n = fread(buf, 1, sizeof buf, in)) > 0;) ok = fwrite(buf, 1, n, out) == n;
    if (in) { ok = ok && !ferror(in); fclose(in); }
    if (out && fclose(out)) ok = 0;
    if (ok) {
        const struct timespec times[2] = {src.st_atim, src.st_mtim};
        utimensat(AT_FDCWD, tmp, times, 0);
        chmod(tmp, 0400);   /* code loaded at run time is kept read-only (Android 14's rule for loaded code) */
        ok = !rename(tmp, copy);
    }
    if (!ok) {
        unlink(tmp);
        fprintf(stderr, "android: the native code could not be copied to %s\n", copy);
        return path;
    }
    fprintf(stderr, "android: the native code copied to %s\n", copy);
    return copy;
}

const char *msr_android_gpu_drivers(void) {
    static char dir[4096];
    if (!dir[0]) {
        const char *internal = SDL_AndroidGetInternalStoragePath();
        snprintf(dir, sizeof dir, "%s/gpu_drivers", internal ? internal : ".");
    }
    return dir;
}

/* (a Java exception from a call: cleared, so that JNI can go on) */
static int jni_failed(JNIEnv *env) {
    if (!(*env)->ExceptionCheck(env)) return 0;
    (*env)->ExceptionClear(env);
    return 1;
}

const char *msr_android_native_libs(void) {
    static char dir[4096];
    if (dir[0]) return dir;
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return dir;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID get = (*env)->GetMethodID(env, cls, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    jobject info = get ? (*env)->CallObjectMethod(env, activity, get) : NULL;
    if (jni_failed(env)) info = NULL;
    if (info) {
        jclass icls = (*env)->GetObjectClass(env, info);
        jfieldID field = (*env)->GetFieldID(env, icls, "nativeLibraryDir", "Ljava/lang/String;");
        jstring s = field ? (jstring)(*env)->GetObjectField(env, info, field) : NULL;
        if (jni_failed(env)) s = NULL;
        if (s) {
            const char *c = (*env)->GetStringUTFChars(env, s, NULL);
            if (c) { snprintf(dir, sizeof dir, "%s", c); (*env)->ReleaseStringUTFChars(env, s, c); }
            (*env)->DeleteLocalRef(env, s);
        }
        (*env)->DeleteLocalRef(env, icls);
        (*env)->DeleteLocalRef(env, info);
    }
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    return dir;
}

void msr_android_pick_gpu_driver(void) {
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID pick = (*env)->GetMethodID(env, cls, "pickGpuDriver", "()V");
    if (pick) (*env)->CallVoidMethod(env, activity, pick);
    else fprintf(stderr, "android: MsrActivity.pickGpuDriver is missing\n");
    jni_failed(env);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

void msr_android_open_url(const char *url) {
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID open = (*env)->GetMethodID(env, cls, "openUrl", "(Ljava/lang/String;)V");
    jstring u = (*env)->NewStringUTF(env, url);
    if (open && u) (*env)->CallVoidMethod(env, activity, open, u);
    else fprintf(stderr, "android: MsrActivity.openUrl is missing\n");
    jni_failed(env);
    if (u) (*env)->DeleteLocalRef(env, u);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

int msr_android_http(const char *method, const char *url, const char *headers, const char *body_path, const char *out_path) {
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();   /* (SDL attaches the calling thread) */
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return -1;
    int status = -1;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID http = (*env)->GetMethodID(env, cls, "httpRequest",
                                         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I");
    jstring m = (*env)->NewStringUTF(env, method), u = (*env)->NewStringUTF(env, url), h = (*env)->NewStringUTF(env, headers),
            b = (*env)->NewStringUTF(env, body_path), o = (*env)->NewStringUTF(env, out_path);
    if (http && m && u && h && b && o) status = (*env)->CallIntMethod(env, activity, http, m, u, h, b, o);
    else fprintf(stderr, "android: MsrActivity.httpRequest is missing\n");
    if (jni_failed(env)) status = -1;
    jstring all[] = {m, u, h, b, o};
    for (int i = 0; i < 5; i++) if (all[i]) (*env)->DeleteLocalRef(env, all[i]);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    return status;
}

void msr_android_relaunch(int argc, const char *const *argv) {
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    fflush(NULL);
    if (env && activity) {
        jclass cls = (*env)->GetObjectClass(env, activity);
        jmethodID relaunch = (*env)->GetMethodID(env, cls, "relaunch", "([Ljava/lang/String;)V");
        jclass string = (*env)->FindClass(env, "java/lang/String");
        jobjectArray args = (*env)->NewObjectArray(env, argc, string, NULL);
        for (int i = 0; i < argc; i++) {
            jstring s = (*env)->NewStringUTF(env, argv[i]);
            (*env)->SetObjectArrayElement(env, args, i, s);
            (*env)->DeleteLocalRef(env, s);
        }
        if (relaunch) (*env)->CallVoidMethod(env, activity, relaunch, args);
        else fprintf(stderr, "android: MsrActivity.relaunch is missing\n");
    }
    fflush(NULL);
    _exit(0);
}

/* ---- the bottom screen on a second display (android.h; MsrActivity's BottomDisplay) ---- */
static pthread_mutex_t bottom_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bottom_cond = PTHREAD_COND_INITIALIZER;
static ANativeWindow *bottom_window;   /* the app's newest surface (a reference of ours), NULL: none */
static int bottom_w, bottom_h;
static unsigned bottom_serial = 1;   /* each change of the surface */
static int bottom_holding;   /* video.c draws into a surface it took */
static Uint32 bottom_event_type;

void msr_android_bottom_display(int on) {
    if (on && !bottom_event_type) {
        const Uint32 t = SDL_RegisterEvents(1);
        if (t != (Uint32)-1) bottom_event_type = t;
    }
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID m = (*env)->GetMethodID(env, cls, "bottomDisplay", "(Z)V");
    if (m) (*env)->CallVoidMethod(env, activity, m, (jboolean)(on != 0));
    else fprintf(stderr, "android: MsrActivity.bottomDisplay is missing\n");
    jni_failed(env);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

unsigned msr_android_bottom_event(void) { return bottom_event_type; }

int msr_android_bottom_available(void) {
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return 0;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID m = (*env)->GetMethodID(env, cls, "hasBottomDisplay", "()Z");
    const jboolean has = m ? (*env)->CallBooleanMethod(env, activity, m) : JNI_FALSE;
    const int ok = !jni_failed(env) && has;
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    return ok;
}

int msr_android_bottom_poll(unsigned *serial, void **window, int *w, int *h) {
    pthread_mutex_lock(&bottom_lock);
    const int changed = *serial != bottom_serial;
    if (changed) {
        *serial = bottom_serial;
        *window = bottom_window; *w = bottom_w; *h = bottom_h;
        if (bottom_window) ANativeWindow_acquire(bottom_window);
    }
    pthread_mutex_unlock(&bottom_lock);
    return changed;
}

void msr_android_bottom_held(int held) {
    pthread_mutex_lock(&bottom_lock);
    bottom_holding = held;
    pthread_cond_broadcast(&bottom_cond);
    pthread_mutex_unlock(&bottom_lock);
}

/* BottomDisplay's SurfaceView (the app's UI thread): a surface, or none (surfaceDestroyed). Without one it returns once
   video.c has let go of the old surface (a second at most: the game may be held, or not drawing) */
JNIEXPORT void JNICALL Java_io_github_gminustwo_msrnative_BottomDisplay_bottomSurface(JNIEnv *env, jclass cls, jobject surface,
                                                                                      jint w, jint h) {
    (void)cls;
    ANativeWindow *window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    pthread_mutex_lock(&bottom_lock);
    if (bottom_window) ANativeWindow_release(bottom_window);
    bottom_window = window; bottom_w = w; bottom_h = h;
    ++bottom_serial;
    if (!window) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 1;
        while (bottom_holding)
            if (pthread_cond_timedwait(&bottom_cond, &bottom_lock, &until)) {
                fprintf(stderr, "android: the bottom screen's surface went while still in use\n");
                break;
            }
    }
    pthread_mutex_unlock(&bottom_lock);
}

/* ...and a touch there (down 1, up 0), at that place in the surface's pixels: an event for video_frame */
JNIEXPORT void JNICALL Java_io_github_gminustwo_msrnative_BottomDisplay_bottomTouch(JNIEnv *env, jclass cls, jint down, jfloat x, jfloat y) {
    (void)env; (void)cls;
    if (!bottom_event_type) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = bottom_event_type;
    e.user.code = down;
    e.user.data1 = (void *)(intptr_t)x;
    e.user.data2 = (void *)(intptr_t)y;
    SDL_PushEvent(&e);
}
