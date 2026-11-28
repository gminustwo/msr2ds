The bottom screen's panel lease (drm-lease-v1) and its touchscreen, for runtime/second_display_lease.c.

These files are Azahar's DRM lease code (GPLv2 or later, license.txt) as MHGU Recomp's AArch64 Linux builds carry it,
taken from https://github.com/cerroverb/MHGURecomp-aarch64 (branch aarch64-linux-v2, commit 8c10fd6,
third_party/azahar-drm-lease). Each file's header names its Azahar source
(https://github.com/virtudude/azahar, branch drm-leasing, commit d16299140, and https://github.com/cerroverb/azahar,
branch drm-leasing-link-crtc, commit 71a132b3b) and the edits made for MHGU Recomp.

The glue that MHGU Recomp's builds added around them (drm_lease.h, drm_lease_dl.cpp, drm_lease_dl.h, drm_lease_glue.h)
and runtime/second_display_lease.c are MHGU Recomp's, under its MIT license (Copyright (c) 2026 Rokk):
LICENSE-MHGU-Recomp.txt.

Edits for msr2ds: the environment variables are named MSR_SECOND_DISPLAY_ instead of MHGU_SECOND_DISPLAY_, and the
comments name runtime/second_display_lease.c instead of MHGU Recomp's runtime/nvn/vulkan/second_display_drmlease.c.
Nothing else is changed.
