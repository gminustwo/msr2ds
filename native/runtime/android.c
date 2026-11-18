/* The Android app's side of the program (android.h). The app (native/android) is SDL's Android activity around this
 * program (libmain.so): MsrActivity passes the arguments of the process it runs, and starts the process again with
 * other ones (relaunch), so the launcher and the game each run in a process of their own, as on a PC. */
#include "android.h"
#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *msr_android_root(void) {
    static char root[4096];
    if (!root[0]) {
        const char *dir = (SDL_AndroidGetExternalStorageState() & SDL_ANDROID_EXTERNAL_STORAGE_WRITE)
                              ? SDL_AndroidGetExternalStoragePath() : NULL;
        if (!dir) dir = SDL_AndroidGetInternalStoragePath();
        snprintf(root, sizeof root, "%s", dir ? dir : ".");
    }
    return root;
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
