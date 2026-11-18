libadrenotools, commit 8fae8ce254dfc1344527e05301e43f37dea2df80
https://github.com/bylaws/libadrenotools
liblinkernsbypass (lib/linkernsbypass), commit aa3975893d83ef1bc84c321ec60c65fbf1287887
https://github.com/bylaws/liblinkernsbypass
Unmodified subset: the custom driver loader (src/driver.cpp) and the two hooks it needs (libhook_impl.so,
libmain_hook.so), without BCn enabling, file redirection, GPU memory import or the tools. Built by native/CMakeLists.txt
for the Android app, which loads it only when the player chose a GPU driver (native/runtime/vk_load.c). BSD 2-Clause
license: LICENSE and lib/linkernsbypass/LICENSE.
