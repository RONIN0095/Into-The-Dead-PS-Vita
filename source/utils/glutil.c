/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 * Copyright (C) 2026      Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"

#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"

void na_gles3_init(void);
extern int vgl_store_depth_stencil; // vitaGL (extras/patches/vitagl-zzzzzz-depth-store-switch.patch)

void gl_preload() {
    if (!file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }
    // Unity binds attributes by name (in_POSITION0, ...) before linking
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
}

void gl_init() {
    // No MSAA: Unity renders into its own FBOs and blits to the screen.
    // vitaGL's circular pool: 16 MB instead of its default 32 MB (taken from
    // the GPU's memory). Unity draws from its own buffers; the pool is for
    // the copies texture uploads make, at most 4 MB at a time.
    vglSetCircularPoolSize(16 * 1024 * 1024);
    // Depth/stencil stay on chip: loading and storing them in every scene
    // (taken over from the Ninja Arashi port) cost GPU bandwidth in every
    // pass. ux0:data/intothedead/depth_store.txt turns it back on.
    vgl_store_depth_stencil = file_exists("ux0:data/intothedead/depth_store.txt");
    l_info("Depth/stencil %s memory in every scene", vgl_store_depth_stencil ? "kept in" : "not kept in");
    vglInitExtended(0, 960, 544, 8 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    na_gles3_init();
}

void gl_swap() {
    vglSwapBuffers(GL_FALSE);
}
