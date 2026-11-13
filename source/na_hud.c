/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/*
 * Things set in the game through the IL2CPP API, on the main thread between
 * frames. For now only the frame rate: Unity holds a phone game at 30 fps
 * unless the game asks for more; 60 is asked for, and what the game had set
 * is logged.
 *
 * (The Ninja Arashi port, which this one started from, also hid that game's
 * touch controls here and told the input code when a level was running.
 * na_hud_in_level() stays for the input code and says "no": the cursor and
 * the touch screen are used everywhere until this game's controls are
 * mapped.)
 */

#include <stdint.h>

#include <psp2/kernel/processmgr.h>

#include "na_hud.h"
#include "na_modules.h"
#include "utils/logger.h"

typedef void *(*domain_get_t)(void);
typedef void *(*assembly_open_t)(void *domain, const char *name);
typedef void *(*assembly_image_t)(void *assembly);
typedef void *(*class_from_name_t)(void *image, const char *ns, const char *name);
typedef void *(*method_from_name_t)(void *klass, const char *name, int argc);
typedef void *(*runtime_invoke_t)(void *method, void *obj, void **params, void **exc);

static runtime_invoke_t invoke;
static void *m_get_fps, *m_set_fps, *m_set_tex_limit, *m_get_tex_limit;
static int state = 0; // 0 not set up, 1 ready, -1 failed

int na_hud_in_level(void) {
    return 0;
}

int na_hud_gdpr_up(void) {
    return 0;
}

static void *sym(const char *name) {
    return (void *)na_module_symbol(NA_LIB_IL2CPP, name);
}

static int setup(void) {
    domain_get_t domain_get = sym("il2cpp_domain_get");
    assembly_open_t assembly_open = sym("il2cpp_domain_assembly_open");
    assembly_image_t assembly_image = sym("il2cpp_assembly_get_image");
    class_from_name_t class_from_name = sym("il2cpp_class_from_name");
    method_from_name_t method_from_name = sym("il2cpp_class_get_method_from_name");
    invoke = sym("il2cpp_runtime_invoke");
    if (!domain_get || !assembly_open || !assembly_image || !class_from_name || !method_from_name || !invoke) {
        l_warn("[HUD] IL2CPP API missing");
        return -1;
    }
    void *core = assembly_open(domain_get(), "UnityEngine.CoreModule");
    void *app = core ? class_from_name(assembly_image(core), "UnityEngine", "Application") : NULL;
    if (app) {
        m_get_fps = method_from_name(app, "get_targetFrameRate", 0);
        m_set_fps = method_from_name(app, "set_targetFrameRate", 1);
    }
    void *qs = core ? class_from_name(assembly_image(core), "UnityEngine", "QualitySettings") : NULL;
    if (qs) {
        m_set_tex_limit = method_from_name(qs, "set_masterTextureLimit", 1);
        m_get_tex_limit = method_from_name(qs, "get_masterTextureLimit", 0);
    }
    l_info("[HUD] Application %p: targetFrameRate %p/%p; QualitySettings %p: masterTextureLimit %p/%p", app, m_get_fps,
           m_set_fps, qs, m_get_tex_limit, m_set_tex_limit);
    return (m_get_fps && m_set_fps) || m_set_tex_limit ? 1 : -1;
}

static void *call(void *method, void *obj, void **params) {
    void *exc = NULL;
    void *r = invoke(method, obj, params, &exc);
    return exc ? NULL : r;
}

volatile uint32_t na_hud_us = 0; // time spent here, for the frame log (main.c)

/*
 * Textures at half size (QualitySettings.masterTextureLimit = 1: Unity
 * uploads each mipmapped texture from its second level). The game's 655
 * textures filled the Vita's 85 MB of video memory and then the heap while
 * the menu loaded; at half size they take a quarter. On the 960x544 screen
 * the difference is small. Set as early as possible: before the first scene
 * with the game's textures loads.
 */
#define TEXTURE_LIMIT 1

void na_hud_tick(int frame) {
    if (state < 0 || (frame != 0 && (frame < 30 || frame % 300 != 30))) return;
    uint64_t t_start = sceKernelGetProcessTimeWide();
    if (state == 0) {
        state = setup();
        if (state < 0) return;
    }
    if (m_set_tex_limit) {
        void *boxed = m_get_tex_limit ? call(m_get_tex_limit, NULL, NULL) : NULL;
        int limit = boxed ? *(int *)((char *)boxed + 8) : -1;
        if (limit != TEXTURE_LIMIT) {
            int want = TEXTURE_LIMIT;
            void *a[1] = { &want };
            call(m_set_tex_limit, NULL, a);
            l_info("[HUD] QualitySettings.masterTextureLimit %d -> %d", limit, want);
        }
    }
    if (!m_get_fps || !m_set_fps) return;
    void *boxed = call(m_get_fps, NULL, NULL);
    int fps = boxed ? *(int *)((char *)boxed + 8) : 0;
    static int last_fps = -12345;
    if (fps != last_fps) {
        last_fps = fps;
        l_info("[HUD] Application.targetFrameRate is %d", fps);
    }
    if (fps != 60) {
        int want = 60;
        void *a[1] = { &want };
        call(m_set_fps, NULL, a);
    }
    na_hud_us += (uint32_t)(sceKernelGetProcessTimeWide() - t_start);
}
