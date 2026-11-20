# libshaderc_shared.so (Android.mk): for the app's one ABI and its lowest Android, with libc++ inside as libmain.so has it
APP_ABI := arm64-v8a
APP_PLATFORM := android-29
APP_STL := c++_static
APP_OPTIM := release
APP_MODULES := shaderc_shared
APP_SUPPORT_FLEXIBLE_PAGE_SIZES := true
# the NDK's folder (shaderc's sources are in it, and its asserts name them) stays out of the library: it is usually in
# the builder's home folder (C:/Users/<name>/AppData/Local/Android/Sdk/ndk/...)
APP_CFLAGS += -ffile-prefix-map=$(NDK_ROOT)=ndk
