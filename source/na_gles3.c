/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_gles3.c
 * @brief OpenGL ES 3.0 as Unity 6 sees it, on top of vitaGL (ES 2.0).
 *
 * Unity fetches every GL function with eglGetProcAddress()/dlsym(). The
 * lookup goes:
 *   1. the table below (ES 3.0 additions, version/limits queries, shader
 *      translation, functions that must look absent),
 *   2. vitaGL (vglGetProcAddress),
 *   3. NULL, with a log line.
 *
 * Functions vitaGL does not have are either implemented on top of ES 2.0
 * (glTexStorage2D, glClearBuffer*, glDrawBuffers, fences, ...) or are stubs
 * that log their first call and return 0. Every name Unity asks for is
 * logged once, so the log shows exactly what the game uses.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vitaGL.h>
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>

#include "na_shaders.h"

static void glBindFramebuffer_na(GLenum target, GLuint fb); // with the screenshot code at the end
static void check_upload(const char *what, GLenum ifmt, GLsizei w, GLsizei h);
static int blend_on = 0; // GL_BLEND, for the draw trace
static GLenum blend_f[4] = { GL_ONE, GL_ZERO, GL_ONE, GL_ZERO }; // current blend factors
static GLuint bound_draw_fb, bound_read_fb; // (screenshot code)
#include "utils/logger.h"
#include "na_stats.h"
// Bound 2D textures per unit and texture sizes, for the draw trace
static GLuint unit_tex[16];
static int active_unit = 0;
#define MAX_TEX_INFO 4096
static struct { uint16_t w, h; uint32_t fmt; uint8_t filled, reported; } tex_info[MAX_TEX_INFO];
static void note_texture(GLenum target, GLsizei w, GLsizei h, GLenum fmt) {
    if (target != 0x0DE1) return; // GL_TEXTURE_2D
    GLuint t = unit_tex[active_unit];
    if (t < MAX_TEX_INFO) { tex_info[t].w = w; tex_info[t].h = h; tex_info[t].fmt = fmt; tex_info[t].filled = 0; }
}
// The texture got pixels (an upload, or it is a render target)
static void note_filled_id(GLuint t) {
    if (t < MAX_TEX_INFO) tex_info[t].filled = 1;
}
static void note_filled(GLenum target) {
    if (target == 0x0DE1) note_filled_id(unit_tex[active_unit]);
}
// Draws with a texture that never got pixels show black or nothing: log them
static void check_empty_textures(void) {
#ifdef NA_RELEASE
    return;
#endif
    static int logged = 0;
    if (logged >= 48) return;
    for (int u = 0; u < 4; u++) {
        GLuint t = unit_tex[u];
        if (!t || t >= MAX_TEX_INFO || !tex_info[t].w || tex_info[t].filled || tex_info[t].reported) continue;
        tex_info[t].reported = 1;
        logged++;
        l_warn("[GL] draw with texture %u (unit %d, %dx%d, fmt 0x%x) that never got pixels", t, u, tex_info[t].w,
               tex_info[t].h, tex_info[t].fmt);
    }
}
static void *convert_etc2(GLenum *fmt, GLsizei *size, const void **data, GLsizei w, GLsizei h);
static void note_big_upload(GLenum target, GLsizei w, GLsizei h);
static int etc2_cached_upload(GLenum target, GLenum fmt, GLsizei w, GLsizei h, const void *data, GLsizei size);
#include "utils/logger.h"

#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#define GL_NUM_EXTENSIONS 0x821D
#endif
#define GL_MAX_3D_TEXTURE_SIZE_ 0x8073
#define GL_MAX_ARRAY_TEXTURE_LAYERS_ 0x88FF
#define GL_MAX_DRAW_BUFFERS_ 0x8824
#define GL_MAX_COLOR_ATTACHMENTS_ 0x8CDF
#define GL_MAX_SAMPLES_ 0x8D57
#define GL_MAX_UNIFORM_BUFFER_BINDINGS_ 0x8A2F
#define GL_MAX_UNIFORM_BLOCK_SIZE_ 0x8A30
#define GL_MAX_VERTEX_UNIFORM_BLOCKS_ 0x8A2B
#define GL_MAX_FRAGMENT_UNIFORM_BLOCKS_ 0x8A2D
#define GL_MAX_COMBINED_UNIFORM_BLOCKS_ 0x8A2E
#define GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT_ 0x8A34
#define GL_MAX_VERTEX_OUTPUT_COMPONENTS_ 0x9122
#define GL_MAX_FRAGMENT_INPUT_COMPONENTS_ 0x9125
#define GL_MAX_VARYING_COMPONENTS_ 0x8B4B
#define GL_MAX_ELEMENT_INDEX_ 0x8D6B
#define GL_MAX_ELEMENTS_VERTICES_ 0x80E8
#define GL_MAX_ELEMENTS_INDICES_ 0x80E9
#define GL_MAX_PROGRAM_TEXEL_OFFSET_ 0x8905
#define GL_MIN_PROGRAM_TEXEL_OFFSET_ 0x8904
#define GL_NUM_PROGRAM_BINARY_FORMATS_ 0x87FE
#define GL_NUM_SHADER_BINARY_FORMATS_ 0x8DF9
#define GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS_ 0x8C8A
#define GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS_ 0x8C8B
#define GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS_ 0x8C80
#define GL_MAX_VERTEX_UNIFORM_COMPONENTS_ 0x8B4A
#define GL_MAX_FRAGMENT_UNIFORM_COMPONENTS_ 0x8B49
#define GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS_ 0x8A31
#define GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS_ 0x8A33
#define GL_MAX_SERVER_WAIT_TIMEOUT_ 0x9111
#define GL_MAX_TEXTURE_LOD_BIAS_ 0x84FD
#define GL_READ_BUFFER_ 0x0C02
#define GL_DRAW_BUFFER0_ 0x8825
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT_ 0x84FF
#define GL_ACTIVE_UNIFORM_BLOCKS_ 0x8A36
#define GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH_ 0x8A35
#define GL_PROGRAM_BINARY_LENGTH_ 0x8741
#define GL_UNIFORM_BLOCK_INDEX_ 0x8A3A
#define GL_NUM_SAMPLE_COUNTS_ 0x9380
#define GL_ALREADY_SIGNALED_ 0x911A
#define GL_SHADER_TYPE_ 0x8B4F
#define GL_COLOR_ 0x1800
#define GL_DEPTH_ 0x1801
#define GL_STENCIL_ 0x1802
#define GL_DEPTH_STENCIL_ 0x84F9
#define GL_TEXTURE_BINDING_2D_ 0x8069
#define GL_TEXTURE_BINDING_CUBE_MAP_ 0x8514
#ifndef GL_LOW_FLOAT
#define GL_LOW_FLOAT 0x8DF0
#define GL_MEDIUM_FLOAT 0x8DF1
#define GL_HIGH_FLOAT 0x8DF2
#endif
#ifndef GL_DEPTH_STENCIL_OES
#define GL_DEPTH_STENCIL_OES 0x84F9
#endif
#ifndef GL_UNSIGNED_INT_24_8_OES
#define GL_UNSIGNED_INT_24_8_OES 0x84FA
#endif

/*
 * Version and extensions
 */
static const char *extensions[] = {
    "GL_EXT_texture_compression_s3tc",
    "GL_EXT_texture_compression_dxt1",
    "GL_IMG_texture_compression_pvrtc",
    "GL_OES_compressed_ETC1_RGB8_texture",
    "GL_OES_depth24",
    "GL_OES_packed_depth_stencil",
    "GL_OES_rgb8_rgba8",
    "GL_OES_texture_npot",
    "GL_OES_element_index_uint",
    "GL_OES_vertex_array_object",
    "GL_EXT_texture_format_BGRA8888",
};
#define N_EXTENSIONS ((int)(sizeof(extensions) / sizeof(extensions[0])))
static char extension_string[1024];

static const GLubyte *glGetString_na(GLenum name) {
    switch (name) {
        case GL_VENDOR: return (const GLubyte *)"vitaGL";
        case GL_RENDERER: return (const GLubyte *)"vitaGL (PS Vita SGX543MP4+)";
        case GL_VERSION: return (const GLubyte *)"OpenGL ES 3.0 vitaGL";
        case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"OpenGL ES GLSL ES 3.00";
        case GL_EXTENSIONS:
            if (!extension_string[0]) {
                for (int i = 0; i < N_EXTENSIONS; i++) {
                    strcat(extension_string, extensions[i]);
                    strcat(extension_string, " ");
                }
            }
            return (const GLubyte *)extension_string;
        default:
            return glGetString(name);
    }
}

static const GLubyte *glGetStringi_na(GLenum name, GLuint index) {
    if (name == GL_EXTENSIONS && index < (GLuint)N_EXTENSIONS)
        return (const GLubyte *)extensions[index];
    return NULL;
}

static int es3_integer(GLenum pname, GLint *v) {
    switch (pname) {
        case GL_MAJOR_VERSION: *v = 3; return 1;
        case GL_MINOR_VERSION: *v = 0; return 1;
        case GL_NUM_EXTENSIONS: *v = N_EXTENSIONS; return 1;
        case GL_MAX_3D_TEXTURE_SIZE_: *v = 256; return 1;
        case GL_MAX_ARRAY_TEXTURE_LAYERS_: *v = 256; return 1;
        case GL_MAX_DRAW_BUFFERS_: *v = 1; return 1;
        case GL_MAX_COLOR_ATTACHMENTS_: *v = 1; return 1;
        case GL_MAX_SAMPLES_: *v = 1; return 1;
        case GL_MAX_UNIFORM_BUFFER_BINDINGS_: *v = 14; return 1; // vitaGL's UBOS_NUM
        case GL_MAX_UNIFORM_BLOCK_SIZE_: *v = 16384; return 1;
        case GL_MAX_VERTEX_UNIFORM_BLOCKS_: *v = 7; return 1;
        case GL_MAX_FRAGMENT_UNIFORM_BLOCKS_: *v = 7; return 1;
        case GL_MAX_COMBINED_UNIFORM_BLOCKS_: *v = 14; return 1;
        case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT_: *v = 256; return 1;
        case GL_MAX_VERTEX_OUTPUT_COMPONENTS_: *v = 64; return 1;
        case GL_MAX_FRAGMENT_INPUT_COMPONENTS_: *v = 60; return 1;
        case GL_MAX_VARYING_COMPONENTS_: *v = 60; return 1;
        case GL_MAX_ELEMENT_INDEX_: *v = 0xFFFFFF; return 1;
        case GL_MAX_ELEMENTS_VERTICES_: *v = 65536; return 1;
        case GL_MAX_ELEMENTS_INDICES_: *v = 65536; return 1;
        case GL_MAX_PROGRAM_TEXEL_OFFSET_: *v = 7; return 1;
        case GL_MIN_PROGRAM_TEXEL_OFFSET_: *v = -8; return 1;
        case GL_NUM_PROGRAM_BINARY_FORMATS_: *v = 0; return 1;
        case GL_NUM_SHADER_BINARY_FORMATS_: *v = 0; return 1;
        case GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS_: *v = 64; return 1;
        case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS_: *v = 4; return 1;
        case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS_: *v = 4; return 1;
        case GL_MAX_VERTEX_UNIFORM_COMPONENTS_: *v = 1024; return 1;
        case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS_: *v = 896; return 1;
        case GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS_: *v = 1024; return 1;
        case GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS_: *v = 896; return 1;
        case GL_MAX_SERVER_WAIT_TIMEOUT_: *v = 0; return 1;
        case GL_READ_BUFFER_: *v = GL_BACK; return 1;
        case GL_DRAW_BUFFER0_: *v = GL_BACK; return 1;
        default: return 0;
    }
}

static void glGetIntegerv_na(GLenum pname, GLint *data) {
    if (!es3_integer(pname, data))
        glGetIntegerv(pname, data);
}

static void glGetInteger64v_na(GLenum pname, int64_t *data) {
    GLint v = 0;
    if (pname == GL_MAX_SERVER_WAIT_TIMEOUT_) {
        *data = 0;
        return;
    }
    glGetIntegerv_na(pname, &v);
    *data = v;
}

static void glGetFloatv_na(GLenum pname, GLfloat *data) {
    GLint v;
    if (pname == GL_MAX_TEXTURE_LOD_BIAS_) {
        *data = 15.0f;
        return;
    }
    if (es3_integer(pname, &v)) {
        *data = (GLfloat)v;
        return;
    }
    glGetFloatv(pname, data);
}

static void glGetBooleanv_na(GLenum pname, GLboolean *data) {
    GLint v;
    if (es3_integer(pname, &v)) {
        *data = v ? GL_TRUE : GL_FALSE;
        return;
    }
    glGetBooleanv(pname, data);
}

/*
 * Shaders: GLSL ES 3.00 -> 1.00 (na_shaders.c)
 */
#define MAX_SHADER_IDS 8192
static uint8_t shader_is_fragment[MAX_SHADER_IDS];

static GLuint glCreateShader_na(GLenum type) {
    GLuint s = glCreateShader(type);
    if (s < MAX_SHADER_IDS) shader_is_fragment[s] = (type == GL_FRAGMENT_SHADER);
    return s;
}

static int shaders_seen = 0;

static void glShaderSource_na(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length) {
    // Join the parts
    size_t total = 0;
    for (int i = 0; i < count; i++)
        total += length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
    char *src = malloc(total + 1);
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        size_t n = length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
        memcpy(src + pos, string[i], n);
        pos += n;
    }
    src[pos] = 0;

    int frag = shader < MAX_SHADER_IDS ? shader_is_fragment[shader] : 0;
    int changed = 0;
    char *t = na_shaders_translate(src, frag, &changed);
#ifdef NA_RELEASE
    if (0) {
#else
    if (shaders_seen < 4) {
#endif
        l_info("glShaderSource(%u, %s): %u bytes%s", shader, frag ? "fragment" : "vertex",
               (unsigned)pos, t ? ", translated from GLSL ES 3.00" : "");
        // The source Unity gave and what vitaGL gets, for checking the translation
        char path[128];
        snprintf(path, sizeof(path), DATA_PATH "shader%d_%s.txt", shaders_seen, frag ? "frag" : "vert");
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "// ---- from Unity ----\n%s\n// ---- to vitaGL ----\n%s\n", src, t ? t : "(unchanged)");
            fclose(f);
        }
    }
    if (shaders_seen++ == 4) l_info("glShaderSource: more shaders follow (not logged)");
    if ((shaders_seen % 50) == 0) l_info("glShaderSource: %d shaders so far", shaders_seen);
    const GLchar *s = t ? t : src;
    GLint len = (GLint)strlen(s);
    glShaderSource(shader, 1, &s, &len);
    free(t);
    free(src);
}

static void glGetProgramiv_na(GLuint program, GLenum pname, GLint *params) {
    switch (pname) {
        case GL_ACTIVE_UNIFORM_BLOCKS_:
        case GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH_:
        case GL_PROGRAM_BINARY_LENGTH_:
            *params = 0;
            return;
        default:
            glGetProgramiv(program, pname, params);
    }
}

static void glGetActiveUniformBlockiv_na(GLuint p, GLuint i, GLenum pname, GLint *params) { *params = 0; }

static void glGetActiveUniformBlockName_na(GLuint p, GLuint i, GLsizei bufsize, GLsizei *length, GLchar *name) {
    if (length) *length = 0;
    if (name && bufsize > 0) name[0] = 0;
}

static void glGetUniformIndices_na(GLuint p, GLsizei count, const GLchar *const *names, GLuint *indices) {
    for (int i = 0; i < count; i++) indices[i] = 0xFFFFFFFF; // GL_INVALID_INDEX
}

static void glGetActiveUniformsiv_na(GLuint p, GLsizei count, const GLuint *indices, GLenum pname, GLint *params) {
    for (int i = 0; i < count; i++)
        params[i] = (pname == GL_UNIFORM_BLOCK_INDEX_) ? -1 : 0;
}

static void glGetShaderPrecisionFormat_na(GLenum shadertype, GLenum precisiontype, GLint *range, GLint *precision) {
    switch (precisiontype) {
        case GL_LOW_FLOAT: case GL_MEDIUM_FLOAT: case GL_HIGH_FLOAT:
            range[0] = 127; range[1] = 127; *precision = 23;
            break;
        default:
            range[0] = 31; range[1] = 30; *precision = 0;
            break;
    }
}

static void glDetachShader_na(GLuint p, GLuint s) {}
static void glValidateProgram_na(GLuint p) {}
static GLboolean glIsVertexArray_na(GLuint a) { return a != 0; }
static void glProgramParameteri_na(GLuint p, GLenum pname, GLint v) {}

/*
 * Buffer targets vitaGL doesn't have
 *
 * vitaGL knows ARRAY, ELEMENT_ARRAY and UNIFORM buffers. Unity creates and
 * fills its vertex and index buffers through GL_COPY_WRITE_BUFFER (so the
 * bound VAO is left alone): vitaGL rejected those calls, the buffers never
 * got memory, and draws read their indices from address 0 (GPU crash).
 * Bindings of the other targets are kept here; operations on them run with
 * the buffer briefly bound as GL_ARRAY_BUFFER (which no VAO captures until
 * glVertexAttribPointer).
 */
extern uint32_t vertex_array_unit; // vitaGL buffers.c

typedef struct { // vitaGL's vbo (shared.h; built without scratch memory)
    void *ptr;
    int32_t size;
    void *(*alloc_func)(size_t);
    uint32_t last_frame;
    uint8_t mapped;
} vgl_vbo;
extern uint32_t vgl_framecount; // vitaGL gxm.c
static int same_frame_maps = 0; // maps of buffers drawn from in the same frame

#define N_EXTRA_TARGETS 10
static const GLenum extra_targets[N_EXTRA_TARGETS] = {
    0x8F36, 0x8F37,         // COPY_READ, COPY_WRITE
    0x88EB, 0x88EC,         // PIXEL_PACK, PIXEL_UNPACK
    0x8C8E, 0x8F3F, 0x90EE, // TRANSFORM_FEEDBACK, DRAW_INDIRECT, DISPATCH_INDIRECT
    0x90D2, 0x92C0, 0x8C2A, // SHADER_STORAGE, ATOMIC_COUNTER, TEXTURE_BUFFER
};
static GLuint extra_bound[N_EXTRA_TARGETS];

static int extra_index(GLenum target) {
    for (int i = 0; i < N_EXTRA_TARGETS; i++)
        if (extra_targets[i] == target) return i;
    return -1;
}

#define PIXEL_UNPACK_IDX 3
extern volatile int na_egl_swap_count;
extern volatile int na_trace_swap;
#define TRACING_FRAME() (na_egl_swap_count == na_trace_swap)
static int upload_logs = 0; // see log_upload

static void glBindBuffer_na(GLenum target, GLuint buffer) {
    int i = extra_index(target);
    if (i >= 0) extra_bound[i] = buffer;
    else glBindBuffer(target, buffer);
}

/*
 * vitaGL keeps 14 uniform buffer bindings (UBOS_NUM) and doesn't check the
 * index: Unity, told there were 24, bound slots 14-23 and those writes
 * landed in the variables after vitaGL's table - is_shark_online, link
 * counters and the first entry of the program table, whose shaders and
 * status became 0 (program 1 "unlinked itself"; the cutscene drew nothing).
 * Bindings past 14 are dropped (the translated shaders use no uniform blocks).
 */
static void glBindBufferBase_na(GLenum target, GLuint index, GLuint buffer) {
    if (target == 0x8A11 /* UNIFORM_BUFFER */ && index >= 14) return;
    glBindBufferBase(target, index, buffer);
}

static void glBindBufferRange_na(GLenum target, GLuint index, GLuint buffer, GLintptr off, GLsizeiptr size) {
    if (target == 0x8A11 && index >= 14) return;
    glBindBufferRange(target, index, buffer, off, size);
}

static void glDeleteBuffers_na(GLsizei n, const GLuint *ids) {
    for (int k = 0; k < n; k++)
        for (int i = 0; i < N_EXTRA_TARGETS; i++)
            if (extra_bound[i] == ids[k]) extra_bound[i] = 0;
    glDeleteBuffers(n, ids);
}

// Runs op with target's buffer bound as GL_ARRAY_BUFFER if target is one of ours.
#define WITH_TARGET(target, op) do { \
        int i_ = extra_index(target); \
        if (i_ < 0) { op(target); break; } \
        uint32_t saved_ = vertex_array_unit; \
        vertex_array_unit = extra_bound[i_]; \
        op(GL_ARRAY_BUFFER); \
        vertex_array_unit = saved_; \
    } while (0)


/*
 * Buffers Unity maps (its dynamic vertex/index rings for particles, trails,
 * text popups) are written by worker threads while mapped. vitaGL's
 * glBufferSubData copies a buffer that was drawn from recently into new
 * storage; on a mapped buffer the copy is taken while those writes are still
 * going on and the draws read half-written vertices: huge triangles and
 * streaks across the level. Sub-data on a buffer that has been mapped is
 * written in place instead (the fences keep the GPU's reads safe).
 */
#define MAX_MAPPED_BUFS 64
static void *mapped_bufs[MAX_MAPPED_BUFS];
static int n_mapped_bufs = 0;

static void note_mapped(void *b) {
    if (!b) return;
    for (int i = 0; i < n_mapped_bufs; i++)
        if (mapped_bufs[i] == b) return;
    if (n_mapped_bufs < MAX_MAPPED_BUFS) mapped_bufs[n_mapped_bufs++] = b;
}

static int was_mapped(void *b) {
    for (int i = 0; i < n_mapped_bufs; i++)
        if (mapped_bufs[i] == b) return 1;
    return 0;
}

static void *element_buffer(void);
static void *target_buffer(GLenum target) {
    if (target == GL_ARRAY_BUFFER) return (void *)vertex_array_unit;
    if (target == GL_ELEMENT_ARRAY_BUFFER) return element_buffer();
    int i = extra_index(target);
    return i >= 0 ? (void *)extra_bound[i] : NULL;
}

static void glBufferData_na(GLenum target, GLsizei size, const void *data, GLenum usage) {
    if (TRACING_FRAME()) l_info("[GL] BufferData(0x%x, %d, %p, 0x%x)", target, size, data, usage);
    if (target == 0x88EC && upload_logs < 200) { upload_logs++; l_info("[UP] PBO %u: " "BufferData size %d data %p usage 0x%x", extra_bound[PIXEL_UNPACK_IDX], size, data, usage); }
#define OP(t) glBufferData(t, size, data, usage)
    WITH_TARGET(target, OP);
#undef OP
}

static void glBufferSubData_na(GLenum target, GLintptr off, GLsizeiptr size, const void *data) {
    if (TRACING_FRAME()) l_info("[GL] BufferSubData(0x%x, +%d, %d)", target, (int)off, (int)size);
    if (target == 0x88EC && upload_logs < 200) { upload_logs++; l_info("[UP] PBO %u: " "BufferSubData off %d size %d data %p", extra_bound[PIXEL_UNPACK_IDX], (int)off, (int)size, data); }
    vgl_vbo *b = (vgl_vbo *)target_buffer(target);
    // in place only while it is mapped (vitaGL copies a buffer drawn from in
    // the last frames, which would lose the writes going on through the map)
    if (b && b->ptr && b->mapped && was_mapped(b) && off >= 0 && off + size <= b->size && data) {
        memcpy((uint8_t *)b->ptr + off, data, size);
        return;
    }
#define OP(t) glBufferSubData(t, off, size, data)
    WITH_TARGET(target, OP);
#undef OP
}

static void *element_buffer(void); // the bound index buffer (needs vitaGL's VAO, below)

static void *glMapBufferRange_na(GLenum target, GLintptr off, GLsizeiptr len, GLbitfield access) {
    if (TRACING_FRAME()) l_info("[GL] MapBufferRange(0x%x, +%d, %d, access 0x%x)", target, (int)off, (int)len, access);
    note_mapped(target_buffer(target));
    {
        vgl_vbo *m = (vgl_vbo *)target_buffer(target);
        if (m && m->last_frame == vgl_framecount && !m->mapped && same_frame_maps++ < 20)
            l_info("[GL] buffer %p (%d bytes) mapped again in the frame it was drawn from: new memory", m, m->size);
    }
    void *r = NULL;
    // MAP_INVALIDATE_BUFFER: the old contents may go; vitaGL would hand out
    // the memory the GPU is still drawing from, so give the buffer new
    // storage first (glBufferData frees the old one once the GPU is done)
    if (access & 0x0008) {
        vgl_vbo *b = NULL;
        if (target == GL_ARRAY_BUFFER) b = (vgl_vbo *)vertex_array_unit;
        else if (target == GL_ELEMENT_ARRAY_BUFFER) b = (vgl_vbo *)element_buffer();
        else if (extra_index(target) >= 0) b = (vgl_vbo *)extra_bound[extra_index(target)];
        if (b && b->size > 0) glBufferData_na(target, b->size, NULL, GL_DYNAMIC_DRAW);
    }
#define OP(t) r = glMapBufferRange(t, off, len, access)
    WITH_TARGET(target, OP);
#undef OP
    if (target == 0x88EC && upload_logs < 200) {
        upload_logs++;
        l_info("[UP] PBO %u: MapBufferRange off %d len %d access 0x%x -> %p", extra_bound[PIXEL_UNPACK_IDX], (int)off,
               (int)len, access, r);
    }
    return r;
}

static GLboolean glUnmapBuffer_na(GLenum target) {
    if (TRACING_FRAME()) l_info("[GL] UnmapBuffer(0x%x)", target);
    GLboolean r = GL_FALSE;
    if (target == 0x88EC && upload_logs < 200) { upload_logs++; l_info("[UP] PBO %u: UnmapBuffer", extra_bound[PIXEL_UNPACK_IDX]); }
#define OP(t) r = glUnmapBuffer(t)
    WITH_TARGET(target, OP);
#undef OP
    return r;
}

static void glFlushMappedBufferRange_na(GLenum target, GLintptr off, GLsizeiptr len) {
    if (target == 0x88EC && upload_logs < 200) { upload_logs++; l_info("[UP] PBO %u: " "FlushMappedBufferRange off %d len %d", extra_bound[PIXEL_UNPACK_IDX], (int)off, (int)len); }
#define OP(t) glFlushMappedBufferRange(t, off, len)
    WITH_TARGET(target, OP);
#undef OP
}

static void glGetBufferParameteriv_na(GLenum target, GLenum pname, GLint *params) {
#define OP(t) glGetBufferParameteriv(t, pname, params)
    WITH_TARGET(target, OP);
#undef OP
}

static GLuint buffer_bound_to(GLenum target) {
    int i = extra_index(target);
    if (i >= 0) return extra_bound[i];
    if (target == GL_ARRAY_BUFFER) return vertex_array_unit;
    return 0;
}

static void glCopyBufferSubData_na(GLenum rt, GLenum wt, GLintptr roff, GLintptr woff, GLsizeiptr size) {
    vgl_vbo *src = (vgl_vbo *)buffer_bound_to(rt);
    GLuint dst = buffer_bound_to(wt);
    if (!src || !src->ptr || !dst || roff + size > src->size) {
        static int warned = 0;
        if (warned++ < 8) l_warn("glCopyBufferSubData(0x%x -> 0x%x, %d bytes): no source/destination", rt, wt, (int)size);
        return;
    }
    uint32_t saved = vertex_array_unit;
    vertex_array_unit = dst;
    glBufferSubData(GL_ARRAY_BUFFER, woff, size, (const uint8_t *)src->ptr + roff);
    vertex_array_unit = saved;
}

// Texture uploads read from the bound PIXEL_UNPACK buffer when there is one
static const void *unpack_ptr(const void *data) {
    vgl_vbo *b = (vgl_vbo *)extra_bound[PIXEL_UNPACK_IDX];
    if (!b) return data;
    return b->ptr ? (const uint8_t *)b->ptr + (uintptr_t)data : NULL;
}


// Upload tracing for big textures: where the data comes from and how much of it is zero
static void log_upload(const char *what, GLint x, GLint y, GLsizei w, GLsizei h, GLsizei size, const void *raw,
                       const void *data) {
    static int logged = 0;
    if (w < 512 || logged >= 60) return;
    logged++;
    vgl_vbo *b = (vgl_vbo *)extra_bound[PIXEL_UNPACK_IDX];
    size_t zero = 0, n = 0, first_nz = (size_t)-1, last_nz = 0;
    if (data)
        for (size_t i = 0; i + 4 <= (size_t)size; i += 64, n++) {
            if (*(const uint32_t *)((const uint8_t *)data + i) == 0) zero++;
            else {
                if (first_nz == (size_t)-1) first_nz = i;
                last_nz = i;
            }
        }
    l_info("[UP] %s tex %u %d,%d %dx%d size %d: %s %p (pbo %p size %d) -> %p, %u%% zero words, nonzero %d..%d", what,
           unit_tex[active_unit], x, y, w, h, size, b ? "pbo offset" : "pointer", raw, b, b ? b->size : 0, data,
           n ? (unsigned)(zero * 100 / n) : 0, (int)first_nz, (int)last_nz);
}
/*
 * Textures: immutable storage on top of glTexImage2D
 */
typedef struct {
    GLuint tex;
    GLenum ifmt;
    GLsizei levels, w, h;
} tex_storage;

#define MAX_STORAGE 4096
static tex_storage storage[MAX_STORAGE];

static tex_storage *storage_for(GLuint tex, int create) {
    uint32_t h = (tex * 2654435761u) % MAX_STORAGE;
    for (int n = 0; n < MAX_STORAGE; n++) {
        tex_storage *s = &storage[(h + n) % MAX_STORAGE];
        if (s->tex == tex) return s;
        if (s->tex == 0) {
            if (!create) return NULL;
            s->tex = tex;
            return s;
        }
    }
    return NULL;
}

static GLuint bound_texture(GLenum target) {
    GLint t = 0;
    glGetIntegerv(target == GL_TEXTURE_CUBE_MAP || (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
                  ? GL_TEXTURE_BINDING_CUBE_MAP_ : GL_TEXTURE_BINDING_2D_, &t);
    return (GLuint)t;
}

// Sized internal format -> format/type for glTexImage2D. Returns 0 for compressed formats.
static int sized_to_unsized(GLenum ifmt, GLenum *format, GLenum *type) {
    *type = GL_UNSIGNED_BYTE;
    switch (ifmt) {
        case 0x8058: /* RGBA8 */ case 0x8C43: /* SRGB8_ALPHA8 */ case GL_RGBA:
        case 0x881A: /* RGBA16F */ case 0x8059: /* RGB10_A2 */
            *format = GL_RGBA; return 1;
        case 0x8C3A: /* R11F_G11F_B10F: HDR render targets; GXM has no 24-bit color surface */
            *format = GL_RGBA; return 1;
        case 0x8051: /* RGB8 */ case 0x8C41: /* SRGB8 */ case GL_RGB:
            *format = GL_RGB; return 1;
        case 0x8D62: /* RGB565 */
            *format = GL_RGB; *type = GL_UNSIGNED_SHORT_5_6_5; return 1;
        case 0x8056: /* RGBA4 */
            *format = GL_RGBA; *type = GL_UNSIGNED_SHORT_4_4_4_4; return 1;
        case 0x8057: /* RGB5_A1 */
            *format = GL_RGBA; *type = GL_UNSIGNED_SHORT_5_5_5_1; return 1;
        case 0x8229: /* R8 */ case 0x822D: /* R16F */
            *format = GL_LUMINANCE; return 1;
        case 0x822B: /* RG8 */
            *format = GL_LUMINANCE_ALPHA; return 1;
        case GL_ALPHA: *format = GL_ALPHA; return 1;
        case GL_LUMINANCE: *format = GL_LUMINANCE; return 1;
        case GL_LUMINANCE_ALPHA: *format = GL_LUMINANCE_ALPHA; return 1;
        case 0x81A5: /* DEPTH_COMPONENT16 */ case 0x81A6: /* DEPTH_COMPONENT24 */ case 0x8CAC: /* DEPTH_COMPONENT32F */
            *format = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_INT; return 1;
        case 0x88F0: /* DEPTH24_STENCIL8 */
            *format = GL_DEPTH_STENCIL_OES; *type = GL_UNSIGNED_INT_24_8_OES; return 1;
        default:
            return 0;
    }
}

/*
 * Unity's Alpha8 textures (fonts, masks) are GL_R8 read through a texture
 * swizzle (alpha = red), which vitaGL doesn't have: as LUMINANCE their alpha
 * was 1 and every glyph got a black box. R8 textures are stored as
 * INTENSITY (U8_RRRR) instead, so red and alpha both hold the value.
 */
static GLint single_channel_ifmt(GLenum ifmt, GLenum unsized) {
    return ifmt == 0x8229 /* R8 */ ? GL_INTENSITY : (GLint)unsized;
}

static int tex_storage_logged = 0;
static void glTexStorage2D_na(GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h) {
    GLuint tex = bound_texture(target);
    note_texture(target, w, h, ifmt);
    if (tex_storage_logged++ < 400)
        l_info("[GL] glTexStorage2D(tex %u, target 0x%x, %d levels, 0x%x, %dx%d)", tex, target, levels, ifmt, w, h);
    tex_storage *s = storage_for(tex, 1);
    if (s) {
        s->ifmt = ifmt;
        s->levels = levels;
        s->w = w;
        s->h = h;
    }
    GLenum format, type;
    if (!sized_to_unsized(ifmt, &format, &type)) {
        // Compressed: allocated on the first full-level glCompressedTexSubImage2D
        return;
    }
    int faces = target == GL_TEXTURE_CUBE_MAP ? 6 : 1;
    for (int f = 0; f < faces; f++) {
        GLenum t = faces == 6 ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + f : target;
        for (int l = 0; l < levels; l++) {
            GLsizei lw = w >> l, lh = h >> l;
            if (lw < 1) lw = 1;
            if (lh < 1) lh = 1;
            glTexImage2D(t, l, single_channel_ifmt(ifmt, format), lw, lh, 0, ifmt == 0x8229 ? GL_RED : format, type, NULL);
            break; // mipmaps are off (see "Mipmaps" below)
        }
    }
}

static void glCompressedTexSubImage2D_na_inner(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                         GLenum format, GLsizei size, const void *data) {
    if (level > 0) return; // mipmaps are off
    const void *raw = data;
    data = unpack_ptr(data);
    log_upload("CompressedTexSubImage2D", x, y, w, h, size, raw, data);
    if (!data) return;
    tex_storage *s = storage_for(bound_texture(target), 0);
    GLsizei lw = s ? s->w >> level : w, lh = s ? s->h >> level : h;
    if (lw < 1) lw = 1;
    if (lh < 1) lh = 1;
    if (x == 0 && y == 0 && w >= lw && h >= lh) {
        void *converted = convert_etc2(&format, &size, &data, w, h);
        glGetError();
        if (!etc2_cached_upload(target, format, w, h, data, size))
            glCompressedTexImage2D(target, level, format, w, h, 0, size, data);
        check_upload("glCompressedTexSubImage2D", format, w, h);
        free(converted);
        note_filled(target);
        note_big_upload(target, w, h);
    } else {
        static int warned = 0;
        if (warned++ < 8)
            l_warn("glCompressedTexSubImage2D: partial update (%d,%d %dx%d of %dx%d) ignored", x, y, w, h, lw, lh);
    }
}
static void glCompressedTexSubImage2D_na(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                         GLenum format, GLsizei size, const void *data) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    glCompressedTexSubImage2D_na_inner(target, level, x, y, w, h, format, size, data);
    na_stats.tex_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    na_stats.tex_n++;
}



/*
 * ETC2 texture cache. vitaGL turns ETC2 into DXT5 on the CPU (decode, then
 * stb_dxt), which made loading slow: a 2048x2048 texture took about 1.8 s.
 * The DXT5 result of each ETC2 texture is kept in ux0:data/intothedead/
 * texcache, named after the CRC32 and size of the ETC2 data; later loads
 * upload it directly (vitaGL only swizzles DXT5 on the GPU).
 */
#include <zlib.h>
#include <sys/stat.h>
void eac_decode(uint8_t *in, uint8_t *out, const int w, const int h, int flag); // vitaGL
#define EAC_ETC2 0
void stb_compress_dxt_block(unsigned char *dest, const unsigned char *src, int alpha, int mode);


/*
 * ETC2 -> RGBA -> DXT5 for a band of block rows. Converting a texture the
 * first time is split over three threads (the Vita has three cores for
 * games, and the others mostly wait while a scene loads).
 */
#include <pthread.h>
typedef struct {
    const uint8_t *etc;
    uint8_t *dxt;
    int w, row0, rows; // block rows
    int ok;
} conv_job;

static void *conv_band(void *arg) {
    conv_job *j = arg;
    size_t bw = (size_t)j->w / 4;
    uint8_t *rgba = malloc((size_t)j->w * 4 * 4 * j->rows);
    if (!rgba) return NULL;
    eac_decode((uint8_t *)j->etc + (size_t)j->row0 * bw * 16, rgba, j->w, j->rows * 4, EAC_ETC2);
    uint8_t block[64], *out = j->dxt + (size_t)j->row0 * bw * 16;
    for (int by = 0; by < j->rows * 4; by += 4)
        for (int bx = 0; bx < j->w; bx += 4) {
            for (int r = 0; r < 4; r++) memcpy(&block[r * 16], &rgba[((by + r) * j->w + bx) * 4], 16);
            stb_compress_dxt_block(out, block, 1, 0);
            out += 16;
        }
    free(rgba);
    j->ok = 1;
    return NULL;
}

static int etc2_to_dxt5(const void *etc, uint8_t *dxt, int w, int h) {
    int rows = h / 4, n = rows >= 48 ? 3 : 1;
    conv_job jobs[3];
    pthread_t th[3];
    int started[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        jobs[i].etc = etc;
        jobs[i].dxt = dxt;
        jobs[i].w = w;
        jobs[i].row0 = rows * i / n;
        jobs[i].rows = rows * (i + 1) / n - jobs[i].row0;
        jobs[i].ok = 0;
    }
    for (int i = 1; i < n; i++) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 64 * 1024);
        started[i] = pthread_create(&th[i], &a, conv_band, &jobs[i]) == 0;
        pthread_attr_destroy(&a);
        if (!started[i]) conv_band(&jobs[i]);
    }
    conv_band(&jobs[0]);
    for (int i = 1; i < n; i++)
        if (started[i]) pthread_join(th[i], NULL);
    for (int i = 0; i < n; i++)
        if (!jobs[i].ok) return 0;
    return 1;
}

int na_etc2_to_dxt5(const void *etc, uint8_t *dxt, int w, int h) { return etc2_to_dxt5(etc, dxt, w, h); }

static int etc2_cached_upload(GLenum target, GLenum fmt, GLsizei w, GLsizei h, const void *data, GLsizei size) {
    if (target != GL_TEXTURE_2D || fmt != 0x9278 || !data) return 0;
    if (w < 64 || h < 64 || w > 2048 || h > 2048 || (w & (w - 1)) || (h & (h - 1))) return 0;
    if (size < w * h) return 0;
    // Key: CRC32 of all the ETC2 data. (Samples are not enough: some atlases
    // differ in a few blocks only; 1 in 8 blocks still mixed up 5 textures.)
    uint32_t crc = (uint32_t)crc32(crc32(0L, Z_NULL, 0), data, w * h);
    char path[128], zpath[132];
    snprintf(path, sizeof(path), DATA_PATH "texcache/%08x_%dx%d.dxt5", (unsigned)crc, w, h);
    snprintf(zpath, sizeof(zpath), "%sz", path);
    size_t dsize = (size_t)w * h; // DXT5: one byte per pixel
    uint8_t *dxt = malloc(dsize);
    if (!dxt) return 0;
    static int hits = 0, misses = 0;
    /*
     * Cache files are zlib-compressed (.dxt5z): DXT5 data of these textures
     * shrinks to about 13% (large transparent areas), and the memory card
     * reads only about 10 MB/s, so reading 4 MB per 2048x2048 texture was
     * most of a level's loading time. Plain .dxt5 files are still read.
     */
    FILE *f = fopen(zpath, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long zn = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *z = zn > 0 ? malloc(zn) : NULL;
        int ok = 0;
        if (z && fread(z, 1, zn, f) == (size_t)zn) {
            uLongf out = dsize;
            ok = uncompress(dxt, &out, z, zn) == Z_OK && out == dsize;
        }
        free(z);
        fclose(f);
        if (ok) {
            glCompressedTexImage2D(target, 0, 0x83F3 /* DXT5 */, w, h, 0, (GLsizei)dsize, dxt);
            free(dxt);
            if (hits++ < 8) l_info("[texcache] hit %s", zpath);
            return 1;
        }
    }
    f = fopen(path, "rb");
    if (f) {
        size_t got = fread(dxt, 1, dsize, f);
        fclose(f);
        if (got == dsize) {
            glCompressedTexImage2D(target, 0, 0x83F3 /* DXT5 */, w, h, 0, (GLsizei)dsize, dxt);
            free(dxt);
            if (hits++ < 8) l_info("[texcache] hit %s", path);
            return 1;
        }
    }
    if (!etc2_to_dxt5(data, dxt, w, h)) {
        free(dxt);
        return 0;
    }
    glCompressedTexImage2D(target, 0, 0x83F3, w, h, 0, (GLsizei)dsize, dxt);
    mkdir(DATA_PATH "texcache", 0777);
    uLongf zn = compressBound(dsize);
    uint8_t *z = malloc(zn);
    if (z && compress2(z, &zn, dxt, dsize, 1) == Z_OK && (f = fopen(zpath, "wb"))) {
        fwrite(z, 1, zn, f);
        fclose(f);
    }
    free(z);
    if (misses++ < 8) l_info("[texcache] stored %s", zpath);
    free(dxt);
    return 1;
}

/*
 * Texture memory dumps: the big ETC2 pictures (title, cutscenes) draw black
 * in large parts although vitaGL's decode + DXT5 path gives the right
 * picture on a PC. The first big textures' GPU memory is written to files
 * right after the upload and again with each screenshot, to compare.
 */
#define MAX_DUMP_TEX 0 // dumps off (3 to compare texture memory again)
static GLuint dump_tex[MAX_DUMP_TEX];
static int n_dump_tex = 0;

static void dump_tex_data(GLuint t, const char *when) {
    if (t >= MAX_TEX_INFO || !tex_info[t].w) return;
    GLint old = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D_, &old);
    glBindTexture(GL_TEXTURE_2D, t);
    void *p = vglGetTexDataPointer(GL_TEXTURE_2D);
    SceGxmTexture *gt = vglGetGxmTexture(GL_TEXTURE_2D);
    unsigned fmt = gt ? (unsigned)sceGxmTextureGetFormat(gt) : 0;
    unsigned tw = gt ? sceGxmTextureGetWidth(gt) : 0, th = gt ? sceGxmTextureGetHeight(gt) : 0;
    size_t size = (size_t)tw * th; // DXT5: one byte per pixel
    char path[96];
    snprintf(path, sizeof(path), DATA_PATH "tex%u_%s.bin", t, when);
    int ok = 0;
    if (p && size) {
        FILE *f = fopen(path, "wb");
        if (f) {
            ok = fwrite(p, 1, size, f) == size;
            fclose(f);
        }
    }
    l_info("[GL] texture %u (%s): data %p, gxm format 0x%x, %ux%u -> %s%s", t, when, p, fmt, tw, th, path,
           ok ? "" : " (not written)");
    glBindTexture(GL_TEXTURE_2D, old);
}

static void note_big_upload(GLenum target, GLsizei w, GLsizei h) {
    if (target != 0x0DE1 || w < 1024 || h < 1024 || n_dump_tex >= MAX_DUMP_TEX) return;
    GLuint t = unit_tex[active_unit];
    dump_tex[n_dump_tex++] = t;
    glFinish();
    dump_tex_data(t, "upload");
}

static void glGetTexLevelParameteriv_na(GLenum target, GLint level, GLenum pname, GLint *params) { *params = 0; }
static void glGetTexLevelParameterfv_na(GLenum target, GLint level, GLenum pname, GLfloat *params) { *params = 0; }
static void glGetTexParameteriv_na(GLenum target, GLenum pname, GLint *params) { *params = 0; }

static void glGetInternalformativ_na(GLenum target, GLenum ifmt, GLenum pname, GLsizei bufsize, GLint *params) {
    if (pname == GL_NUM_SAMPLE_COUNTS_ && bufsize > 0) params[0] = 0;
}

/*
 * Framebuffers
 */
static GLfloat clear_color[4] = { 0, 0, 0, 0 };
static GLfloat clear_depth = 1.0f;
static GLint clear_stencil = 0;

static void glClearColor_na(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    clear_color[0] = r; clear_color[1] = g; clear_color[2] = b; clear_color[3] = a;
    glClearColor(r, g, b, a);
}

static void glClearDepthf_na(GLfloat d) {
    clear_depth = d;
    glClearDepthf(d);
}

static void glClearStencil_na(GLint s) {
    clear_stencil = s;
    glClearStencil(s);
}

static void glClearBufferfv_na(GLenum buffer, GLint drawbuffer, const GLfloat *v) {
    if (buffer == GL_COLOR_) {
        glClearColor(v[0], v[1], v[2], v[3]);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
    } else if (buffer == GL_DEPTH_) {
        glClearDepthf(v[0]);
        glClear(GL_DEPTH_BUFFER_BIT);
        glClearDepthf(clear_depth);
    }
}

static void glClearBufferiv_na(GLenum buffer, GLint drawbuffer, const GLint *v) {
    if (buffer == GL_STENCIL_) {
        glClearStencil(v[0]);
        glClear(GL_STENCIL_BUFFER_BIT);
        glClearStencil(clear_stencil);
    }
}

static void glClearBufferuiv_na(GLenum buffer, GLint drawbuffer, const GLuint *v) {
    if (buffer == GL_COLOR_) {
        GLfloat f[4] = { v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3] / 255.0f };
        glClearBufferfv_na(buffer, drawbuffer, f);
    }
}

static void glClearBufferfi_na(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
    glClearDepthf(depth);
    glClearStencil(stencil);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glClearDepthf(clear_depth);
    glClearStencil(clear_stencil);
}

static void glDrawBuffers_na(GLsizei n, const GLenum *bufs) {}
static void glInvalidateFramebuffer_na(GLenum target, GLsizei n, const GLenum *attachments) {}

/*
 * Mipmaps
 *
 * vitaGL's mip chains for runtime-recompressed (ETC2 -> DXT5, padded to a
 * power of two) and immutable-storage textures don't match what it tells
 * the GPU when a sampler asks for mipmaps, and the GPU then reads past the
 * texture: a GPU crash on the first frame of the game scene. A 2D game
 * loses next to nothing without them, so only level 0 is kept and the
 * mipmapped minification filters become their base filter.
 */
static GLint base_filter(GLint f) {
    switch (f) {
        case 0x2700: case 0x2702: return GL_NEAREST; // NEAREST_MIPMAP_NEAREST/LINEAR
        case 0x2701: case 0x2703: return GL_LINEAR;  // LINEAR_MIPMAP_NEAREST/LINEAR
        default: return f;
    }
}

static void glTexParameteri_na(GLenum target, GLenum pname, GLint v) {
    if (pname == GL_TEXTURE_MIN_FILTER) v = base_filter(v);
    if (pname == 0x813C || pname == 0x813D || pname == 0x813A || pname == 0x813B) return; // BASE/MAX_LEVEL, MIN/MAX_LOD
    glTexParameteri(target, pname, v);
}

static void glTexParameterf_na(GLenum target, GLenum pname, GLfloat v) {
    if (pname == GL_TEXTURE_MIN_FILTER) {
        glTexParameteri_na(target, pname, (GLint)v);
        return;
    }
    if (pname == 0x813C || pname == 0x813D || pname == 0x813A || pname == 0x813B) return;
    glTexParameterf(target, pname, v);
}

static void glTexParameteriv_na(GLenum target, GLenum pname, const GLint *v) {
    glTexParameteri_na(target, pname, v[0]);
}

static void glSamplerParameteri_na(GLuint smp, GLenum pname, GLint v) {
    if (pname == GL_TEXTURE_MIN_FILTER) v = base_filter(v);
    if (pname == 0x813A || pname == 0x813B) return; // MIN/MAX_LOD
    glSamplerParameteri(smp, pname, v);
}

static void glSamplerParameterf_na(GLuint smp, GLenum pname, GLfloat v) {
    if (pname == GL_TEXTURE_MIN_FILTER) {
        glSamplerParameteri_na(smp, pname, (GLint)v);
        return;
    }
    if (pname == 0x813A || pname == 0x813B) return;
    glSamplerParameterf(smp, pname, v);
}

static void glTexImage2D_na_inner(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum format,
                            GLenum type, const void *data) {
    if (level > 0) return;
    note_texture(target, w, h, ifmt);
    if (ifmt == 0x8229 /* R8 */ || (ifmt == GL_RED && format == GL_RED)) ifmt = GL_INTENSITY; // see single_channel_ifmt
    glGetError();
    glTexImage2D(target, level, ifmt, w, h, border, format, type, unpack_ptr(data));
    check_upload("glTexImage2D", ifmt, w, h);
    if (unpack_ptr(data)) note_filled(target);
}
static void glTexImage2D_na(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum format,
                            GLenum type, const void *data) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    glTexImage2D_na_inner(target, level, ifmt, w, h, border, format, type, data);
    na_stats.tex_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    na_stats.tex_n++;
}

static void glTexSubImage2D_na_inner(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                               GLenum type, const void *data) {
    if (level > 0) return;
    data = unpack_ptr(data);
    if (!data) return;
    glTexSubImage2D(target, level, x, y, w, h, format, type, data);
    note_filled(target);
}
static void glTexSubImage2D_na(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                               GLenum type, const void *data) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    glTexSubImage2D_na_inner(target, level, x, y, w, h, format, type, data);
    na_stats.tex_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    na_stats.tex_n++;
}

/*
 * ETC2 without alpha (GL_COMPRESSED_RGB8_ETC2, Unity's format for opaque
 * textures: backgrounds, scenery) is rejected by vitaGL, which only decodes
 * ETC2 with EAC alpha: those textures stayed black. Each 8-byte RGB block
 * gets an 8-byte EAC alpha block in front (base 255, multiplier 0: fully
 * opaque), and the result goes in as GL_COMPRESSED_RGBA8_ETC2_EAC. sRGB
 * variants are passed on as their linear formats.
 * Returns a buffer to free (or NULL if the data is used as is).
 */
static void *convert_etc2(GLenum *fmt, GLsizei *size, const void **data, GLsizei w, GLsizei h) {
    if (*fmt == 0x9279) *fmt = 0x9278; // SRGB8_ALPHA8_ETC2_EAC -> RGBA8_ETC2_EAC
    if (*fmt == 0x9275) *fmt = 0x9274; // SRGB8_ETC2 -> RGB8_ETC2
    if (*fmt != 0x9274) return NULL;
    size_t blocks = (size_t)((w + 3) / 4) * ((h + 3) / 4);
    if ((size_t)*size < blocks * 8) blocks = *size / 8;
    uint8_t *out = malloc(blocks * 16);
    if (!out) return NULL;
    const uint8_t *in = *data;
    for (size_t i = 0; i < blocks; i++) {
        uint8_t *b = &out[i * 16];
        memset(b, 0, 8);
        b[0] = 255; // base alpha, multiplier 0 and table 0 in b[1]
        memcpy(b + 8, &in[i * 8], 8);
    }
    *fmt = 0x9278; // GL_COMPRESSED_RGBA8_ETC2_EAC
    *size = (GLsizei)(blocks * 16);
    *data = out;
    return out;
}

// Texture uploads that fail (out of GPU memory, unsupported format) leave a
// texture that draws black: report them with the memory left.
static void check_upload(const char *what, GLenum ifmt, GLsizei w, GLsizei h) {
    GLenum e = glGetError();
    if (e == GL_NO_ERROR) return;
    static int reported = 0;
    if (reported++ < 40)
        l_warn("[GL] %s(0x%x, %dx%d) failed: 0x%x (free: vram %u KB, ram %u KB, phycont %u KB, newlib %u KB)", what, ifmt,
               w, h, e, (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024), (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024),
               (unsigned)(vglMemFree(VGL_MEM_SLOW) / 1024), (unsigned)(vglMemFree(VGL_MEM_EXTERNAL) / 1024));
}

#include <malloc.h>
extern int _newlib_heap_size_user; // main.c

void na_gl_log_memory(void) {
    // newlib heap: what is handed out (uordblks) of the heap grown so far (arena)
    struct mallinfo mi = mallinfo();
    {
        void na_mmap_report(size_t *committed, size_t *reserved); // reimpl/mem.c
        size_t c = 0, r = 0;
        na_mmap_report(&c, &r);
        int na_mmap_fault_count(void);
        l_info("[MEM] reservations: %u KB with memory of %u KB reserved (%d first touches)", (unsigned)(c / 1024),
               (unsigned)(r / 1024), na_mmap_fault_count());
    }
    l_info("[MEM] heap: %u KB used, %u KB free in arena, %u KB never touched; vram %u KB free, vgl ram %u KB, phycont %u KB",
           (unsigned)(mi.uordblks / 1024), (unsigned)(mi.fordblks / 1024),
           (unsigned)((_newlib_heap_size_user - (int)mi.arena) / 1024),
           (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024), (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024),
           (unsigned)(vglMemFree(VGL_MEM_SLOW) / 1024));
}

static void glCompressedTexImage2D_na_inner(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border,
                                      GLsizei size, const void *data) {
    if (level > 0) return;
    note_texture(target, w, h, ifmt);
    const void *raw = data;
    data = unpack_ptr(data);
    log_upload("CompressedTexImage2D", 0, 0, w, h, size, raw, data);
    if (!data) return;
    void *converted = convert_etc2(&ifmt, &size, &data, w, h);
    glGetError();
    if (!etc2_cached_upload(target, ifmt, w, h, data, size))
        glCompressedTexImage2D(target, level, ifmt, w, h, border, size, data);
    check_upload("glCompressedTexImage2D", ifmt, w, h);
    free(converted);
    note_filled(target);
    note_big_upload(target, w, h);
}
static void glCompressedTexImage2D_na(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border,
                                      GLsizei size, const void *data) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    glCompressedTexImage2D_na_inner(target, level, ifmt, w, h, border, size, data);
    na_stats.tex_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    na_stats.tex_n++;
}

static void glGenerateMipmap_na(GLenum target) {}

/*
 * Render targets
 *
 * vitaGL only knows depth/stencil renderbuffers. Unity renders into color
 * renderbuffers (MSAA or not) and resolves them with glBlitFramebuffer, so
 * a color renderbuffer is backed by a texture and attached as one;
 * otherwise the framebuffer has no color surface and the GPU writes to
 * address 0.
 */
typedef struct {
    GLuint rb;
    GLuint tex;
    GLenum ifmt;
    GLsizei w, h;
} rb_info;

#define MAX_RBS 256
static rb_info rbs[MAX_RBS];
static GLuint bound_rb = 0;
static int gl_trace_left = 400; // FBO setup lines logged

#define TRACE(...) do { if (gl_trace_left > 0) { gl_trace_left--; l_info("[GL] " __VA_ARGS__); } } while (0)

static rb_info *rb_for(GLuint rb, int create) {
    if (!rb) return NULL;
    rb_info *free_slot = NULL;
    for (int i = 0; i < MAX_RBS; i++) {
        if (rbs[i].rb == rb) return &rbs[i];
        if (!rbs[i].rb && !free_slot) free_slot = &rbs[i];
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->rb = rb;
    return free_slot;
}

static int is_color_format(GLenum ifmt) {
    switch (ifmt) {
        case 0x81A5: case 0x81A6: case 0x81A7: case 0x8CAC: // DEPTH_COMPONENT16/24/32/32F
        case 0x88F0: case 0x8CAD: case 0x8D48: case GL_DEPTH_COMPONENT: // DEPTH24_STENCIL8, DEPTH32F_STENCIL8, STENCIL_INDEX8
        case 0x84F9: // DEPTH_STENCIL
            return 0;
        default:
            return 1;
    }
}

static void glBindRenderbuffer_na(GLenum target, GLuint rb) {
    bound_rb = rb;
    glBindRenderbuffer(target, rb);
}

static void glRenderbufferStorage_na(GLenum target, GLenum ifmt, GLsizei w, GLsizei h) {
    TRACE("glRenderbufferStorage(rb %u, 0x%x, %dx%d)", bound_rb, ifmt, w, h);
    rb_info *r = rb_for(bound_rb, 1);
    if (r) {
        r->ifmt = ifmt;
        r->w = w;
        r->h = h;
    }
    if (!is_color_format(ifmt) || !r) {
        glRenderbufferStorage(target, ifmt, w, h);
        return;
    }
    GLenum format, type;
    if (!sized_to_unsized(ifmt, &format, &type)) {
        format = GL_RGBA;
        type = GL_UNSIGNED_BYTE;
    }
    GLint old = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D_, &old);
    if (!r->tex) glGenTextures(1, &r->tex);
    glBindTexture(GL_TEXTURE_2D, r->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, format, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, (GLuint)old);
}

static void glRenderbufferStorageMultisample_na(GLenum target, GLsizei samples, GLenum ifmt, GLsizei w, GLsizei h) {
    glRenderbufferStorage_na(target, ifmt, w, h);
}

static void glFramebufferRenderbuffer_na(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb) {
    rb_info *r = rb_for(rb, 0);
    TRACE("glFramebufferRenderbuffer(0x%x, rb %u%s)", attachment, rb, r && r->tex ? " as texture" : "");
    if (r && r->tex) {
        glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, r->tex, 0);
        return;
    }
    if (attachment == GL_COLOR_ATTACHMENT0 && rb == 0) {
        glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, 0, 0);
        return;
    }
    if (attachment >= GL_COLOR_ATTACHMENT0 && attachment < GL_COLOR_ATTACHMENT0 + 16) return; // MRT: unsupported
    glFramebufferRenderbuffer(target, attachment, rbtarget, rb);
}

static void glDeleteRenderbuffers_na(GLsizei n, const GLuint *ids) {
    for (int i = 0; i < n; i++) {
        rb_info *r = rb_for(ids[i], 0);
        if (r) {
            if (r->tex) glDeleteTextures(1, &r->tex);
            memset(r, 0, sizeof(*r));
        }
        if (ids[i] == bound_rb) bound_rb = 0;
    }
    glDeleteRenderbuffers(n, ids);
}

static void glGetRenderbufferParameteriv_na(GLenum target, GLenum pname, GLint *params) {
    rb_info *r = rb_for(bound_rb, 0);
    switch (pname) {
        case 0x8D42: *params = r ? r->w : 0; break;       // WIDTH
        case 0x8D43: *params = r ? r->h : 0; break;       // HEIGHT
        case 0x8D44: *params = r ? r->ifmt : 0; break;    // INTERNAL_FORMAT
        case 0x8CAB: *params = 1; break;                  // SAMPLES
        default: *params = 0; break;
    }
}

static void glFramebufferTexture2D_na(GLenum target, GLenum attachment, GLenum textarget, GLuint tex, GLint level) {
    TRACE("glFramebufferTexture2D(0x%x, 0x%x, tex %u, level %d)", attachment, textarget, tex, level);
    if (attachment > GL_COLOR_ATTACHMENT0 && attachment < GL_COLOR_ATTACHMENT0 + 16) return; // MRT: unsupported
    if (attachment != GL_COLOR_ATTACHMENT0) return; // depth textures: vitaGL keeps its own depth buffer
    extern GLuint na_first_color_target;
    if (!na_first_color_target && tex) na_first_color_target = tex;
    if (textarget != GL_TEXTURE_2D) {
        static int warned = 0;
        if (warned++ < 8) l_warn("glFramebufferTexture2D: target 0x%x not supported", textarget);
        return;
    }
    glFramebufferTexture2D(target, attachment, textarget, tex, level);
    note_filled_id(tex);
}

static void glFramebufferTexture_na(GLenum target, GLenum attachment, GLuint tex, GLint level) {
    glFramebufferTexture2D_na(target, attachment, GL_TEXTURE_2D, tex, level);
}

static GLenum glCheckFramebufferStatus_na(GLenum target) {
    GLenum r = glCheckFramebufferStatus(target);
    if (r != GL_FRAMEBUFFER_COMPLETE) TRACE("glCheckFramebufferStatus(0x%x) = 0x%x", target, r);
    return r;
}

static void glBlitFramebuffer_na(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                                 GLbitfield mask, GLenum filter) {
    static int logged_blits = 0;
    if (logged_blits++ < 8)
        l_info("[GL] glBlitFramebuffer(%d,%d-%d,%d -> %d,%d-%d,%d, mask 0x%x)", sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask);
    if (!(mask & GL_COLOR_BUFFER_BIT)) return; // depth/stencil blits: not supported by vitaGL
    glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, GL_COLOR_BUFFER_BIT, filter);
}

/*
 * Depth and stencil tests are off (experiment, hardware run 35): the menu
 * shows only strips of its big panels, as if most fragments failed a depth
 * or stencil test against stale values. Ninja Arashi is 2D: Unity orders
 * its sprites and UI itself, so neither test is needed to draw it.
 */
static void glActiveTexture_na(GLenum unit) {
    active_unit = (unit - 0x84C0) & 15; // GL_TEXTURE0
    glActiveTexture(unit);
}

static void glBindTexture_na(GLenum target, GLuint tex) {
    if (target == 0x0DE1) unit_tex[active_unit] = tex;
    glBindTexture(target, tex);
}

static void glDisable_na(GLenum cap) {
    if (cap == GL_BLEND) blend_on = 0;
    glDisable(cap);
}

/*
 * Culling of front faces. vitaGL only knows back face culling: with
 * glCullFace(GL_FRONT) it set "no polygons" and dropped every draw. Unity
 * culls front faces for some meshes (mirrored ones, and Cull Front
 * shaders): the black blocks under the ground were not drawn, and what the
 * frame before left there smeared into streaks as the camera moved.
 * Culling the front faces is culling the back faces with the other winding
 * as the front, which vitaGL is given instead.
 */
static GLenum front_face = GL_CCW, cull_face = GL_BACK;
static void apply_cull(void) {
    if (cull_face == GL_FRONT) {
        glFrontFace(front_face == GL_CCW ? GL_CW : GL_CCW);
        glCullFace(GL_BACK);
    } else {
        glFrontFace(front_face);
        glCullFace(cull_face);
    }
}
static void glCullFace_na(GLenum mode) {
    static int logged = 0;
    if (mode != GL_BACK && logged++ < 8) l_info("[GL] glCullFace(0x%x)", mode);
    cull_face = mode;
    apply_cull();
}
static void glFrontFace_na(GLenum mode) {
    front_face = mode;
    apply_cull();
}

static void glEnable_na(GLenum cap) {
    if (cap == GL_BLEND) blend_on = 1;
    glEnable(cap);
}

/*
 * Color writes off (glColorMask(0, 0, 0, 0)): Unity's UI Mask draws its
 * shape that way, only to write the stencil, before and after the masked
 * content. vitaGL didn't keep the color out of those draws, so the closing
 * mask draw covered the whole list (level select: chapter art, levels) with
 * an opaque rectangle. Such draws now also blend with (ZERO, ONE), which
 * leaves the color buffer as it is whatever the mask does.
 */
static int color_mask_off = 0;

static void glColorMask_na(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    color_mask_off = !r && !g && !b && !a;
    glColorMask(r, g, b, a);
}

static void glColorMaski_na(GLuint i, GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    if (i == 0) glColorMask_na(r, g, b, a);
}

static void masked_draw_begin(void) {
    if (!color_mask_off) return;
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ZERO, GL_ONE);
}

static void masked_draw_end(void) {
    if (!color_mask_off) return;
    glBlendFuncSeparate(blend_f[0], blend_f[1], blend_f[2], blend_f[3]);
    if (!blend_on) glDisable(GL_BLEND);
}

static void glBlendEquationi_na(GLuint i, GLenum mode) {
    if (i == 0) glBlendEquation(mode);
}

static void glBlendEquationSeparatei_na(GLuint i, GLenum rgb, GLenum alpha) {
    if (i == 0) glBlendEquationSeparate(rgb, alpha);
}

static void glBlendFuncSeparatei_na(GLuint i, GLenum sr, GLenum dr, GLenum sa, GLenum da) {
    if (i == 0) glBlendFuncSeparate(sr, dr, sa, da);
}

/*
 * Vertex input / uniforms
 */
static void glVertexAttribIPointer_na(GLuint index, GLint size, GLenum type, GLsizei stride, const void *ptr) {
    glVertexAttribPointer(index, size, type, GL_FALSE, stride, ptr);
}

#define UIV(n) \
    static void glUniform##n##uiv_na(GLint loc, GLsizei count, const GLuint *v) { \
        glUniform##n##iv(loc, count, (const GLint *)v); \
    }
UIV(1) UIV(2) UIV(3) UIV(4)

static void glGetUniformiv_na(GLuint p, GLint loc, GLint *params) { *params = 0; }

/*
 * Draw validation
 *
 * A draw that reads past a vertex or index buffer faults on the GPU ("GPU
 * crash", no CPU stack to look at). Every draw is checked against vitaGL's
 * own buffer state first: out-of-range draws are skipped and logged. The
 * first draws of the game scene are also logged one by one and followed by
 * glFinish, so a crash that gets past the checks still names its draw.
 */
typedef struct {
    uint16_t streamIndex, offset;
    uint8_t format, componentCount;
    uint16_t regIndex;
} vgl_attr;

typedef struct {
    uint16_t stride, indexSource;
} vgl_stream;

typedef struct { // vitaGL's vao (shared.h), VERTEX_ATTRIBS_NUM = 16
    uint32_t index_array_unit;
    uint8_t vertex_attrib_size[16];
    uint32_t vertex_attrib_offsets[16];
    uint32_t vertex_attrib_vbo[16];
    uint32_t vertex_attrib_state;
    uint32_t vertex_attrib_divisor;
    float *vertex_attrib_value[16];
    vgl_attr vertex_attrib_config[16];
    vgl_stream vertex_stream_config[16];
} vgl_vao;

extern vgl_vao *cur_vao; // vitaGL buffers.c
static void *element_buffer(void) { return (void *)cur_vao->index_array_unit; }
extern volatile int na_egl_swap_count; // na_egl.c

static GLuint cur_program = 0;
#define MAX_PROGS 4096
static int draw_no = 0, draws_traced = 0, bad_draws = 0;
static uint32_t first_vertex = 0, last_vertex = 0;

static void glBlendFunc_na(GLenum s, GLenum d) {
    blend_f[0] = blend_f[2] = s; blend_f[1] = blend_f[3] = d;
    glBlendFunc(s, d);
}

static void glBlendFuncSeparate_na(GLenum sr, GLenum dr, GLenum sa, GLenum da) {
    blend_f[0] = sr; blend_f[1] = dr; blend_f[2] = sa; blend_f[3] = da;
    glBlendFuncSeparate(sr, dr, sa, da);
}
#define TRACE_FROM_SWAP 0x7fffffff // automatic trace off (650: just after the menu is up)
#define TRACED_DRAWS 120

/*
 * Program lifetime as in GL: a program deleted while it is current stays
 * alive until another program is made current. vitaGL frees it at once and
 * hands its id to the next glCreateProgram; Unity deletes shaders while
 * switching scenes (cutscene: every draw then used a program slot vitaGL
 * had cleared). Program and shader lifetime events are logged.
 */
static GLuint pending_delete = 0;
static int prog_events = 0;
#define PROG_EVENT(...) do { \
        if (prog_events < 600) { prog_events++; l_info("[GL] " __VA_ARGS__); } \
    } while (0)

int vglCurrentProgramDrawable(char *info, int info_size); // extras/patches/vitagl-zz-program-check.patch

static void glUseProgram_na(GLuint p) {
    cur_program = p;
    glUseProgram(p);
    if (p) {
        char info[160];
        if (!vglCurrentProgramDrawable(info, sizeof(info))) PROG_EVENT("glUseProgram(%u): %s", p, info);
    }
    if (pending_delete && pending_delete != p) {
        PROG_EVENT("deferred glDeleteProgram(%u)", pending_delete);
        glDeleteProgram(pending_delete);
        pending_delete = 0;
    }
}

static void glAttachShader_na(GLuint p, GLuint sh) {
    PROG_EVENT("glAttachShader(%u, %u)", p, sh);
    glAttachShader(p, sh);
}

static void glDeleteShader_na(GLuint sh) {
    PROG_EVENT("glDeleteShader(%u)", sh);
    glDeleteShader(sh);
}

static GLuint glCreateProgram_na(void) {
    GLuint p = glCreateProgram();
    PROG_EVENT("glCreateProgram() = %u", p);
    return p;
}

// How Unity binds attributes and fills uniforms (the first calls only)
static int gl_calls_logged = 0;
#define LOG_CALL(...) do { if (gl_calls_logged < 160) { gl_calls_logged++; l_info("[GL] " __VA_ARGS__); } } while (0)

static void glBindAttribLocation_na(GLuint p, GLuint idx, const GLchar *name) {
    LOG_CALL("glBindAttribLocation(%u, %u, %s)", p, idx, name);
    glBindAttribLocation(p, idx, name);
}

// last ObjectToWorld matrix per program, for the frame trace
static GLint prog_o2w_loc[MAX_PROGS];
static float prog_o2w[MAX_PROGS][16];

// and the view-projection matrix, for the near plane check (cut_near_triangles)
static GLint prog_vp_loc[MAX_PROGS];
static float prog_vp[MAX_PROGS][16];

static GLint glGetUniformLocation_na(GLuint p, const GLchar *name) {
    GLint r = glGetUniformLocation(p, name);
    if (p < MAX_PROGS && strcmp(name, "hlslcc_mtx4x4unity_ObjectToWorld") == 0) prog_o2w_loc[p] = r;
    if (p < MAX_PROGS && strcmp(name, "hlslcc_mtx4x4unity_MatrixVP") == 0) prog_vp_loc[p] = r;
    LOG_CALL("glGetUniformLocation(%u, %s) = %d", p, name, r);
    return r;
}

static void glUniform4fv_na(GLint loc, GLsizei count, const GLfloat *v) {
    LOG_CALL("glUniform4fv(%d, %d): %.3f %.3f %.3f %.3f%s", loc, count, v[0], v[1], v[2], v[3],
             count > 1 ? " ..." : "");
    if (count == 4 && cur_program < MAX_PROGS && prog_o2w_loc[cur_program] == loc && loc)
        memcpy(prog_o2w[cur_program], v, sizeof(prog_o2w[0]));
    if (count == 4 && cur_program < MAX_PROGS && prog_vp_loc[cur_program] == loc && loc)
        memcpy(prog_vp[cur_program], v, sizeof(prog_vp[0]));
    glUniform4fv(loc, count, v);
}

static void glUniformMatrix4fv_na(GLint loc, GLsizei count, GLboolean t, const GLfloat *v) {
    LOG_CALL("glUniformMatrix4fv(%d, %d): %.3f %.3f %.3f %.3f ...", loc, count, v[0], v[5], v[10], v[15]);
    glUniformMatrix4fv(loc, count, t, v);
}

static void glUniform1i_na(GLint loc, GLint v) {
    LOG_CALL("glUniform1i(%d, %d)", loc, v);
    glUniform1i(loc, v);
}

// vitaGL crashes drawing with a program that never linked (no vertex shader attached)
static uint8_t prog_linked[MAX_PROGS];
static uint8_t prog_shadow[MAX_PROGS]; // shadow caster programs (no shadows on the Vita)

static void glCompileShader_na(GLuint s) {
    glCompileShader(s);
    GLint ok = 1;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        static int warned = 0;
        if (warned++ < 32) {
            char info[512] = "";
            GLsizei len = 0;
            glGetShaderInfoLog(s, sizeof(info) - 1, &len, info);
            info[len > 0 && len < (GLsizei)sizeof(info) ? len : 0] = 0;
            l_warn("[GL] glCompileShader(%u) failed: %s", s, info);
        }
    }
}

static void glLinkProgram_na_inner(GLuint p) {
    PROG_EVENT("glLinkProgram(%u)", p);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (p < MAX_PROGS) prog_linked[p] = ok ? 1 : 0;

    // Shadow casters (they have unity_LightShadowBias): their draws are skipped
    if (ok && p < MAX_PROGS) {
        GLint nu = 0;
        glGetProgramiv(p, 0x8B86, &nu);
        prog_shadow[p] = 0;
        for (int i = 0; i < nu && i < 64; i++) {
            char n[64] = "";
            GLsizei len = 0; GLint size = 0; GLenum type = 0;
            glGetActiveUniform(p, i, sizeof(n), &len, &size, &type, n);
            if (strcmp(n, "unity_LightShadowBias") == 0) prog_shadow[p] = 1;
        }
        if (prog_shadow[p]) l_info("[GL] program %u is a shadow caster: its draws are skipped", p);
    }

    // What Unity will see of the first programs: attributes and uniforms
    static int described = 0;
    if (ok && described++ < 6) {
        GLint na = 0, nu = 0;
        glGetProgramiv(p, 0x8B89, &na); // ACTIVE_ATTRIBUTES
        glGetProgramiv(p, 0x8B86, &nu); // ACTIVE_UNIFORMS
        l_info("[GL] program %u: %d attributes, %d uniforms", p, na, nu);
        for (int i = 0; i < na && i < 16; i++) {
            char n[64] = "";
            GLsizei len = 0; GLint size = 0; GLenum type = 0;
            glGetActiveAttrib(p, i, sizeof(n), &len, &size, &type, n);
            l_info("[GL]   attribute %d: %s (type 0x%x, size %d) at %d", i, n, type, size, glGetAttribLocation(p, n));
        }
        for (int i = 0; i < nu && i < 32; i++) {
            char n[64] = "";
            GLsizei len = 0; GLint size = 0; GLenum type = 0;
            glGetActiveUniform(p, i, sizeof(n), &len, &size, &type, n);
            l_info("[GL]   uniform %d: %s (type 0x%x, size %d) at %d", i, n, type, size, glGetUniformLocation(p, n));
        }
    }
    if (!ok) {
        static int warned = 0;
        if (warned++ < 32) l_warn("[GL] glLinkProgram(%u) failed", p);
    }
}
static void glLinkProgram_na(GLuint p) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    glLinkProgram_na_inner(p);
    na_stats.link_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    na_stats.link_n++;
}

static void glDeleteProgram_na(GLuint p) {
    if (!p) return;
    PROG_EVENT("glDeleteProgram(%u)%s", p, p == cur_program ? " (current: deferred)" : "");
    if (p < MAX_PROGS) prog_linked[p] = 0;
    if (p == cur_program) {
        if (pending_delete && pending_delete != p) glDeleteProgram(pending_delete);
        pending_delete = p;
        return;
    }
    if (p == pending_delete) pending_delete = 0;
    glDeleteProgram(p);
}

int vglCurrentProgramDrawable(char *info, int info_size); // extras/patches/vitagl-zz-program-check.patch

static int program_ok(void) {
    char info[160] = "";
    if (vglCurrentProgramDrawable(info, sizeof(info))) return 1;
    if (bad_draws++ < 40)
        l_warn("[GL] draw %d skipped: %s (our program %u, linked %d)", draw_no, info, cur_program,
               cur_program < MAX_PROGS ? prog_linked[cur_program] : -1);
    return 0;
}

static uint32_t max_index(const void *p, GLsizei count, GLenum type) {
    uint32_t m = 0;
    if (type == GL_UNSIGNED_SHORT) {
        const uint16_t *s = p;
        for (GLsizei i = 0; i < count; i++) if (s[i] > m) m = s[i];
    } else if (type == GL_UNSIGNED_INT) {
        const uint32_t *s = p;
        for (GLsizei i = 0; i < count; i++) if (s[i] > m) m = s[i];
    } else {
        const uint8_t *s = p;
        for (GLsizei i = 0; i < count; i++) if (s[i] > m) m = s[i];
    }
    return m;
}

// Returns 1 if the draw stays inside its buffers; desc gets a short description.
static float pos_lo[3], pos_hi[3]; // vertex position range of the last checked draw
static void ring_record(int count, uint32_t last);

static int check_draw(GLsizei count, GLenum type, const void *indices, GLint base, GLint first, int elements,
                      char *desc, size_t desc_size) {
    if (count <= 0) return 0;
    if (desc_size) desc[0] = 0;
    if (!program_ok()) return 0;
    // no shadows: a 2D game on a small screen, and the pass cost GPU time
    if (cur_program < MAX_PROGS && prog_shadow[cur_program]) return 0;
    check_empty_textures();
    // Every index and position of the draw is only gone through in a traced
    // frame (L+R+DOWN). It used to be done for every draw, to keep bad draws
    // from the GPU while the glitches were looked for; their cause is fixed
    // (vitaGL's 16-bit attribute offsets), and the passes cost time each frame.
    int full = TRACING_FRAME();
    uint32_t last;
    if (elements) {
        int isz = type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 1;
        vgl_vbo *ib = (vgl_vbo *)cur_vao->index_array_unit;
        const void *src;
        if (ib) {
            uint32_t off = (uint32_t)(uintptr_t)indices;
            if (!ib->ptr || off + (uint32_t)count * isz > (uint32_t)ib->size) {
                if (bad_draws++ < 40)
                    l_warn("[GL] draw %d skipped (prog %u): %d indices at +%u past index buffer of %d bytes (ptr %p)",
                           draw_no, cur_program, count, off, ib->size, ib->ptr);
                return 0;
            }
            src = (const uint8_t *)ib->ptr + off;
        } else {
            src = indices;
            if ((uintptr_t)src < 0x81000000) {
                if (bad_draws++ < 40)
                    l_warn("[GL] draw %d skipped (prog %u): no index buffer, indices %p", draw_no, cur_program, indices);
                return 0;
            }
        }
        last = full ? max_index(src, count, type) + base : 0;
        first_vertex = (type == GL_UNSIGNED_SHORT ? ((const uint16_t *)src)[0] :
                        type == GL_UNSIGNED_INT ? ((const uint32_t *)src)[0] : ((const uint8_t *)src)[0]) + base;
    } else {
        last = first + count - 1;
        first_vertex = first;
    }
    if (full) snprintf(desc, desc_size, "count %d, last vertex %u", count, last);
    last_vertex = last;

    uint32_t state = cur_vao->vertex_attrib_state;
    for (int i = 0; i < 16; i++) {
        if (!(state & (1u << i))) continue;
        uint32_t id = cur_vao->vertex_attrib_vbo[i];
        uint32_t off = cur_vao->vertex_attrib_offsets[i];
        uint32_t stride = cur_vao->vertex_stream_config[i].stride;
        uint32_t comps = cur_vao->vertex_attrib_config[i].componentCount;
        if (!id) {
            if (off < 0x81000000) {
                if (bad_draws++ < 40)
                    l_warn("[GL] draw %d skipped (prog %u): attrib %d enabled without buffer (pointer 0x%x)",
                           draw_no, cur_program, i, off);
                return 0;
            }
            continue; // client-side array
        }
        vgl_vbo *vb = (vgl_vbo *)id;
        uint32_t divided = cur_vao->vertex_attrib_divisor & (1u << i);
        uint32_t need = off + (divided ? 0 : last) * stride + comps;
        if (!vb->ptr || need > (uint32_t)vb->size) {
            if (bad_draws++ < 40)
                l_warn("[GL] draw %d skipped (prog %u): attrib %d needs %u bytes (offset %u, stride %u, last vertex %u), "
                       "buffer has %d (ptr %p)", draw_no, cur_program, i, need, off, stride, last, vb->size, vb->ptr);
            return 0;
        }
    }
    // Small dynamic meshes (text popups like "+38", particles) now and then
    // come with garbage positions: one vertex far away and a letter is drawn
    // as a huge triangle over the screen. Look at their positions and skip
    // draws with impossible values.
    pos_lo[0] = pos_lo[1] = pos_lo[2] = 0;
    pos_hi[0] = pos_hi[1] = pos_hi[2] = 0;
    if (full && (state & 1) && last < 1024 && cur_vao->vertex_attrib_vbo[0] &&
        cur_vao->vertex_attrib_config[0].format == 9 /* F32 */ && cur_vao->vertex_attrib_config[0].componentCount >= 2) {
        vgl_vbo *vb = (vgl_vbo *)cur_vao->vertex_attrib_vbo[0];
        uint32_t off = cur_vao->vertex_attrib_offsets[0], stride = cur_vao->vertex_stream_config[0].stride;
        int comps = cur_vao->vertex_attrib_config[0].componentCount > 3 ? 3 : cur_vao->vertex_attrib_config[0].componentCount;
        int bad = 0;
        for (uint32_t v = 0; v <= last; v++) {
            const float *f = (const float *)((const uint8_t *)vb->ptr + off + v * stride);
            for (int c = 0; c < comps; c++) {
                float x = f[c];
                if (x != x || x > 1e6f || x < -1e6f) bad = 1;
                if (v == 0 || x < pos_lo[c]) pos_lo[c] = x;
                if (v == 0 || x > pos_hi[c]) pos_hi[c] = x;
            }
        }
        if (bad) {
            if (bad_draws++ < 40)
                l_warn("[GL] draw %d skipped (prog %u): impossible vertex positions (%g..%g, %g..%g)", draw_no,
                       cur_program, pos_lo[0], pos_hi[0], pos_lo[1], pos_hi[1]);
            return 0;
        }
    }
    ring_record(count, last);
    return 1;
}

// A whole frame traced on request (L+R+DOWN, see na_input.c / na_egl.c)
volatile int na_trace_swap = -1;
static int frame_traced = 0;

static int trace_this_draw(void) {
    static int last = -2;
    if (na_trace_swap != last) {
        last = na_trace_swap;
        frame_traced = 0;
    }
    if (na_egl_swap_count == na_trace_swap) return frame_traced++ < 500;
    return na_egl_swap_count >= TRACE_FROM_SWAP && draws_traced < TRACED_DRAWS;
}

static void traced(const char *what, const char *desc) {
    draws_traced++;
    // colour of the first vertex (attribute 1 is COLOR0 in Unity's bindings)
    char col[48] = "no color";
    if (cur_vao->vertex_attrib_state & 2) {
        vgl_vbo *vb = (vgl_vbo *)cur_vao->vertex_attrib_vbo[1];
        uint32_t off = cur_vao->vertex_attrib_offsets[1] + first_vertex * cur_vao->vertex_stream_config[1].stride;
        if (vb && vb->ptr && off + 4 <= (uint32_t)vb->size) {
            const uint8_t *c = (const uint8_t *)vb->ptr + off;
            snprintf(col, sizeof(col), "color %u/%u/%u/%u (fmt %u)", c[0], c[1], c[2], c[3],
                     cur_vao->vertex_attrib_config[1].format);
        }
    }
    // first vertex position (attribute 0, floats) and depth state
    char pos[224] = "";
    if (cur_vao->vertex_attrib_state & 1) {
        vgl_vbo *vb = (vgl_vbo *)cur_vao->vertex_attrib_vbo[0];
        uint32_t off = cur_vao->vertex_attrib_offsets[0] + first_vertex * cur_vao->vertex_stream_config[0].stride;
        if (vb && vb->ptr && off + 12 <= (uint32_t)vb->size) {
            const float *f = (const float *)((const uint8_t *)vb->ptr + off);
            const float *m = cur_program < MAX_PROGS ? prog_o2w[cur_program] : NULL;
            snprintf(pos, sizeof(pos), ", at +%u, pos %.2f/%.2f/%.2f (x %.1f..%.1f, y %.1f..%.1f; o2w scale %.2f/%.2f move %.1f/%.1f/%.1f)",
                     (unsigned)cur_vao->vertex_attrib_offsets[0],
                     f[0], f[1], f[2], pos_lo[0], pos_hi[0], pos_lo[1], pos_hi[1], m ? m[0] : 0, m ? m[5] : 0,
                     m ? m[12] : 0, m ? m[13] : 0, m ? m[14] : 0);
        }
    }
    // where the vertices end up on screen (clip space, from the program's
    // ObjectToWorld and MatrixVP) and the culling
    char clip[160] = "";
    if ((cur_vao->vertex_attrib_state & 1) && cur_program < MAX_PROGS && prog_vp_loc[cur_program] &&
        prog_o2w_loc[cur_program] && cur_vao->vertex_attrib_config[0].format == 9 &&
        cur_vao->vertex_attrib_config[0].componentCount >= 3) {
        vgl_vbo *vb = (vgl_vbo *)cur_vao->vertex_attrib_vbo[0];
        uint32_t off = cur_vao->vertex_attrib_offsets[0], stride = cur_vao->vertex_stream_config[0].stride;
        const float *m = prog_o2w[cur_program], *vp = prog_vp[cur_program];
        float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f}, hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
        uint32_t end = last_vertex > 4095 ? 4095 : last_vertex;
        if (vb && vb->ptr && off + end * stride + 12 <= (uint32_t)vb->size) {
            for (uint32_t v = 0; v <= end; v++) {
                const float *f = (const float *)((const uint8_t *)vb->ptr + off + v * stride);
                float wp[4], c[4];
                for (int k = 0; k < 4; k++) wp[k] = m[k] * f[0] + m[4 + k] * f[1] + m[8 + k] * f[2] + m[12 + k];
                for (int k = 0; k < 4; k++) c[k] = vp[k] * wp[0] + vp[4 + k] * wp[1] + vp[8 + k] * wp[2] + vp[12 + k] * wp[3];
                float q[4] = {c[0] / c[3], c[1] / c[3], c[2] / c[3], c[3]};
                for (int k = 0; k < 4; k++) {
                    if (q[k] < lo[k]) lo[k] = q[k];
                    if (q[k] > hi[k]) hi[k] = q[k];
                }
            }
            snprintf(clip, sizeof(clip), ", ndc x %.2f..%.2f y %.2f..%.2f z %.3f..%.3f w %.2f..%.2f", lo[0], hi[0], lo[1],
                     hi[1], lo[2], hi[2], lo[3], hi[3]);
        }
    }
    char cull[48];
    snprintf(cull, sizeof(cull), ", cull %s front 0x%x face 0x%x", glIsEnabled(GL_CULL_FACE) ? "on" : "off", front_face,
             cull_face);
    GLint dfunc = 0;
    GLboolean dmask = 0;
    glGetIntegerv(GL_DEPTH_FUNC, &dfunc);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
    char depth[48];
    snprintf(depth, sizeof(depth), ", depth %s 0x%x%s", glIsEnabled(GL_DEPTH_TEST) ? "on" : "off", dfunc,
             dmask ? " write" : "");
    GLuint t = unit_tex[0];
    int ti = t < MAX_TEX_INFO;
    l_info("[GL] draw %d: %s prog %u, %s%s%s%s%s, %s, blend %s 0x%x/0x%x 0x%x/0x%x, tex %u %dx%d fmt 0x%x, fb %u%s", draw_no, what,
           cur_program, desc, pos, clip, cull, depth, col, blend_on ? "on" : "off", blend_f[0], blend_f[1], blend_f[2], blend_f[3], t,
           ti ? tex_info[t].w : 0, ti ? tex_info[t].h : 0, ti ? tex_info[t].fmt : 0, bound_draw_fb,
           color_mask_off ? ", color writes off" : "");
}

// No glFinish here: vitaGL's ends the scene, and the next one starts with an
// empty depth buffer, so a traced frame did not look like the frame played.
static void traced_done(void) {
    l_info("[GL] draw %d ok", draw_no);
}

static void glDrawArrays_na(GLenum mode, GLint first, GLsizei count) {
    char desc[96];
    draw_no++;
    if (!check_draw(count, 0, NULL, 0, first, 0, desc, sizeof(desc))) return;
    int t = trace_this_draw();
    if (t) traced("DrawArrays", desc);
    masked_draw_begin();
    glDrawArrays(mode, first, count);
    masked_draw_end();
    if (t) traced_done();
}

static void glDrawArraysInstanced_na(GLenum mode, GLint first, GLsizei count, GLsizei n) {
    char desc[96];
    draw_no++;
    if (!check_draw(count, 0, NULL, 0, first, 0, desc, sizeof(desc))) return;
    int t = trace_this_draw();
    if (t) traced("DrawArraysInstanced", desc);
    masked_draw_begin();
    glDrawArraysInstanced(mode, first, count, n);
    masked_draw_end();
    if (t) traced_done();
}

/*
 * Triangles in front of the camera's near plane. The game's camera has
 * perspective (the backgrounds lie far behind), and some effects (the
 * KriptoFX trails and glows near the player) put vertices at or behind the
 * camera. A phone's GPU clips those triangles at the near plane; here they
 * came out as huge stretched triangles over half the screen (streaks), and
 * the one draw took 58 ms. Such triangles are dropped: for an indexed
 * triangle draw whose program has the usual Unity matrices, each vertex's
 * clip position is worked out on the CPU (ObjectToWorld, then MatrixVP), and
 * triangles with a vertex in front of the near plane (z < -w) are left out
 * of a copy of the index list that is drawn instead.
 */
static uint8_t *near_bad;   // per vertex: in front of the near plane
static size_t near_bad_n;
static void *near_idx;      // the filtered index list
static size_t near_idx_size;
static int near_logs = 0;

static const int near_cut = 0; // off: the streaks it was made for had another cause, and it costs a pass per draw

static const void *cut_near_triangles(GLenum mode, GLsizei *count, GLenum type, const void *indices, GLint base) {
    if (!near_cut || mode != GL_TRIANGLES || cur_program >= MAX_PROGS) return NULL;
    if (!prog_vp_loc[cur_program] || !prog_o2w_loc[cur_program]) return NULL;
    const float *vp = prog_vp[cur_program], *m = prog_o2w[cur_program];
    // orthographic: w is 1 everywhere, nothing can be in front of the camera
    if (vp[3] == 0 && vp[7] == 0 && vp[11] == 0) return NULL;
    if (!(cur_vao->vertex_attrib_state & 1) || !cur_vao->vertex_attrib_vbo[0]) return NULL;
    if (cur_vao->vertex_attrib_config[0].format != 9 /* F32 */ || cur_vao->vertex_attrib_config[0].componentCount < 3)
        return NULL;
    int isz = type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 1;
    vgl_vbo *ib = (vgl_vbo *)cur_vao->index_array_unit;
    const uint8_t *src = ib ? (const uint8_t *)ib->ptr + (uintptr_t)indices : (const uint8_t *)indices;
    if (!src) return NULL;
    GLsizei n = *count;
    uint32_t lo = 0xffffffff, hi = 0;
    for (GLsizei i = 0; i < n; i++) {
        uint32_t v = isz == 2 ? ((const uint16_t *)src)[i] : isz == 4 ? ((const uint32_t *)src)[i] : src[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (hi - lo > 65535) return NULL;
    vgl_vbo *vb = (vgl_vbo *)cur_vao->vertex_attrib_vbo[0];
    uint32_t off = cur_vao->vertex_attrib_offsets[0], stride = cur_vao->vertex_stream_config[0].stride;
    if (!vb->ptr || off + (hi + base) * stride + 12 > (uint32_t)vb->size) return NULL;
    size_t nv = hi - lo + 1;
    if (nv > near_bad_n) {
        uint8_t *nb = realloc(near_bad, nv);
        if (!nb) return NULL;
        near_bad = nb;
        near_bad_n = nv;
    }
    int bad = 0;
    for (size_t k = 0; k < nv; k++) {
        const float *f = (const float *)((const uint8_t *)vb->ptr + off + (lo + k + base) * stride);
        float wp[4];
        for (int c = 0; c < 4; c++) wp[c] = m[c] * f[0] + m[4 + c] * f[1] + m[8 + c] * f[2] + m[12 + c];
        float z = vp[2] * wp[0] + vp[6] * wp[1] + vp[10] * wp[2] + vp[14] * wp[3];
        float w = vp[3] * wp[0] + vp[7] * wp[1] + vp[11] * wp[2] + vp[15] * wp[3];
        near_bad[k] = !(z >= -w && w > 0); // NaN counts as bad too
        bad += near_bad[k];
    }
    if (!bad) return NULL;
    if ((size_t)n * isz > near_idx_size) {
        void *ni = realloc(near_idx, (size_t)n * isz);
        if (!ni) return NULL;
        near_idx = ni;
        near_idx_size = (size_t)n * isz;
    }
    GLsizei kept = 0;
    for (GLsizei t = 0; t + 2 < n; t += 3) {
        uint32_t a, b, c;
        if (isz == 2) { const uint16_t *s16 = (const uint16_t *)src; a = s16[t]; b = s16[t + 1]; c = s16[t + 2]; }
        else if (isz == 4) { const uint32_t *s32 = (const uint32_t *)src; a = s32[t]; b = s32[t + 1]; c = s32[t + 2]; }
        else { a = src[t]; b = src[t + 1]; c = src[t + 2]; }
        if (near_bad[a - lo] || near_bad[b - lo] || near_bad[c - lo]) continue;
        if (isz == 2) { uint16_t *d = (uint16_t *)near_idx + kept; d[0] = a; d[1] = b; d[2] = c; }
        else if (isz == 4) { uint32_t *d = (uint32_t *)near_idx + kept; d[0] = a; d[1] = b; d[2] = c; }
        else { uint8_t *d = (uint8_t *)near_idx + kept; d[0] = a; d[1] = b; d[2] = c; }
        kept += 3;
    }
    if (near_logs < 30) {
        near_logs++;
        l_info("[GL] draw %d (prog %u): %d of %u vertices in front of the near plane, %d of %d triangles drawn",
               draw_no, cur_program, bad, (unsigned)nv, kept / 3, n / 3);
    }
    *count = kept;
    return near_idx;
}

// Draws with the filtered index list (client memory; vitaGL copies it)
static void draw_cut(GLenum mode, GLsizei count, GLenum type, const void *idx, GLint base) {
    if (count <= 0) return;
    uint32_t saved = cur_vao->index_array_unit;
    cur_vao->index_array_unit = 0;
    if (base == 0)
        glDrawElements(mode, count, type, idx);
    else
        glDrawElementsBaseVertex(mode, count, type, idx, base);
    cur_vao->index_array_unit = saved;
}

static void glDrawElements_na(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    char desc[96];
    draw_no++;
    if (!check_draw(count, type, indices, 0, 0, 1, desc, sizeof(desc))) return;
    int t = trace_this_draw();
    if (t) traced("DrawElements", desc);
    masked_draw_begin();
    GLsizei n = count;
    const void *cut = cut_near_triangles(mode, &n, type, indices, 0);
    if (cut) draw_cut(mode, n, type, cut, 0);
    else glDrawElements(mode, count, type, indices);
    masked_draw_end();
    if (t) traced_done();
}

static void glDrawElementsBaseVertex_na(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint base) {
    char desc[96];
    draw_no++;
    if (!check_draw(count, type, indices, base, 0, 1, desc, sizeof(desc))) return;
    int t = trace_this_draw();
    if (t) traced("DrawElementsBaseVertex", desc);
    masked_draw_begin();
    GLsizei n = count;
    const void *cut = cut_near_triangles(mode, &n, type, indices, base);
    if (cut) draw_cut(mode, n, type, cut, base);
    else if (base == 0)
        glDrawElements(mode, count, type, indices);
    else
        glDrawElementsBaseVertex(mode, count, type, indices, base);
    masked_draw_end();
    if (t) traced_done();
}

static void glDrawElementsInstanced_na(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei n) {
    char desc[96];
    draw_no++;
    if (!check_draw(count, type, indices, 0, 0, 1, desc, sizeof(desc))) return;
    int t = trace_this_draw();
    if (t) traced("DrawElementsInstanced", desc);
    masked_draw_begin();
    glDrawElementsInstanced(mode, count, type, indices, n);
    masked_draw_end();
    if (t) traced_done();
}

static void glDrawElementsInstancedBaseVertex_na(GLenum mode, GLsizei count, GLenum type, const void *indices,
                                                 GLsizei instances, GLint basevertex) {
    if (basevertex == 0)
        glDrawElementsInstanced_na(mode, count, type, indices, instances);
    else
        glDrawElementsBaseVertex_na(mode, count, type, indices, basevertex);
}

/*
 * Sync objects: vitaGL submits in order, so fences are always signaled.
 */
/*
 * Fences. They used to report "signaled" at once, so Unity reused parts of
 * its dynamic vertex/index buffers (particles, sprite batches) that the GPU
 * was still drawing from the frame before: long streaks of stretched
 * triangles across the level, most with many particles on screen.
 * vitaGL sends a frame's draws to the GPU when the frame is swapped, so a
 * fence from an earlier frame is waited for with glFinish (all submitted
 * work done); one from the current frame has nothing submitted yet that
 * Unity could be overwriting from an earlier frame, and counts as signaled.
 */
extern SceGxmContext *gxm_context; // vitaGL gxm.c
typedef struct { int swap; int done; } na_fence;
static int fence_waits = 0;

static void *glFenceSync_na(GLenum cond, GLbitfield flags) {
    na_fence *f = malloc(sizeof(*f));
    if (!f) return NULL;
    f->swap = na_egl_swap_count;
    f->done = 0;
    return f;
}

static int fence_wait(na_fence *f) {
    if (!f || f->done) return 1;
    if (f->swap < na_egl_swap_count) {
        uint64_t t0 = sceKernelGetProcessTimeWide();
        // The GPU's work of the frames before. Not vitaGL's glFinish:
        // that ends the scene being drawn and goes on in a
        // new one (a second pass over the screen each frame, and without
        // STORE_DEPTH_STENCIL an empty depth buffer for the rest of the
        // frame: what the black blocks under the hills hide showed again).
        sceGxmFinish(gxm_context);
        na_stats.finish_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
        f->done = 1;
        fence_waits++;
    }
    return 1;
}

static GLenum glClientWaitSync_na(void *sync, GLbitfield flags, uint64_t timeout) {
    fence_wait((na_fence *)sync);
    return GL_ALREADY_SIGNALED_;
}
static void glWaitSync_na(void *sync, GLbitfield flags, uint64_t timeout) { fence_wait((na_fence *)sync); }
static void glGetSynciv_na(void *sync, GLenum pname, GLsizei bufsize, GLsizei *length, GLint *values) {
    if (length) *length = 1;
    if (!values || bufsize < 1) return;
    if (pname == 0x9114 /* SYNC_STATUS */) {
        fence_wait((na_fence *)sync);
        values[0] = 0x9119; // SIGNALED
    } else if (pname == 0x9112 /* OBJECT_TYPE */) {
        values[0] = 0x9116; // SYNC_FENCE
    } else if (pname == 0x9113 /* SYNC_CONDITION */) {
        values[0] = 0x9117; // SYNC_GPU_COMMANDS_COMPLETE
    } else {
        values[0] = 0;
    }
}
static void glDeleteSync_na(void *sync) { free(sync); }
static GLboolean glIsSync_na(void *sync) { return sync != NULL; }

/*
 * Stubs: log the first call, return 0
 */
#define STUB(name) \
    static int name##_stub(void) { \
        static int called = 0; \
        if (!called++) l_warn("GL stub called: " #name); \
        return 0; \
    }

STUB(glTexImage3D) STUB(glTexSubImage3D) STUB(glTexStorage3D) STUB(glCompressedTexImage3D)
STUB(glCompressedTexSubImage3D) STUB(glFramebufferTextureLayer) STUB(glFramebufferTexture3D)
STUB(glCopyImageSubData) STUB(glTexBuffer) STUB(glTexStorage2DMultisample)
STUB(glTexStorage3DMultisample) STUB(glTexImage2DMultisample) STUB(glFramebufferTexture2DMultisample)
STUB(glResolveMultisampleFramebuffer) STUB(glTexPageCommitment) STUB(glTextureView)
STUB(glGetTextureParameteriv) STUB(glDispatchCompute) STUB(glDispatchComputeIndirect)
STUB(glMemoryBarrier) STUB(glBindImageTexture) STUB(glDrawArraysIndirect) STUB(glDrawElementsIndirect)
STUB(glPatchParameteri) STUB(glGetProgramInterfaceiv) STUB(glGetProgramResourceIndex)
STUB(glGetProgramResourceName) STUB(glGetProgramResourceiv) STUB(glGenQueriesEXT) STUB(glDeleteQueriesEXT)
STUB(glBeginQueryEXT) STUB(glEndQueryEXT) STUB(glGetQueryiv) STUB(glGetQueryObjectui64v)
STUB(glProgramUniformExtra) STUB(glBlendBarrier) STUB(glMultiview) STUB(glDebug)

typedef struct {
    const char *name;
    void *func;
} gl_entry;

#define E(n) { #n, (void *)&n##_na }
#define S(n, s) { #n, (void *)&s##_stub }
#define HIDE(n) { #n, NULL }

static const gl_entry gl_table[] = {
    E(glGetString), E(glGetStringi), E(glGetIntegerv), E(glGetInteger64v), E(glGetFloatv), E(glGetBooleanv),
    E(glCreateShader), E(glShaderSource), E(glGetProgramiv),
    E(glGetActiveUniformBlockiv), E(glGetActiveUniformBlockName), E(glGetUniformIndices),
    E(glGetActiveUniformsiv), E(glGetShaderPrecisionFormat), E(glDetachShader), E(glValidateProgram),
    E(glIsVertexArray), E(glProgramParameteri),
    E(glTexStorage2D), { "glTexStorage2DEXT", (void *)&glTexStorage2D_na },
    E(glCompressedTexSubImage2D), E(glGetTexLevelParameteriv), E(glGetTexLevelParameterfv),
    E(glGetTexParameteriv), E(glGetInternalformativ),
    E(glClearColor), E(glClearDepthf), E(glClearStencil),
    E(glClearBufferfv), E(glClearBufferiv), E(glClearBufferuiv), E(glClearBufferfi),
    E(glDrawBuffers), E(glInvalidateFramebuffer), E(glRenderbufferStorageMultisample),
    { "glRenderbufferStorageMultisampleEXT", (void *)&glRenderbufferStorageMultisample_na },
    { "glRenderbufferStorageMultisampleIMG", (void *)&glRenderbufferStorageMultisample_na },
    { "glRenderbufferStorageMultisampleAPPLE", (void *)&glRenderbufferStorageMultisample_na },
    E(glGetRenderbufferParameteriv), E(glBindRenderbuffer), E(glRenderbufferStorage),
    E(glFramebufferRenderbuffer), E(glDeleteRenderbuffers), E(glFramebufferTexture2D), E(glFramebufferTexture),
    { "glFramebufferTextureEXT", (void *)&glFramebufferTexture_na }, { "glFramebufferTextureOES", (void *)&glFramebufferTexture_na },
    E(glCheckFramebufferStatus), E(glBlitFramebuffer),
    E(glTexParameteri), E(glTexParameterf), E(glTexParameteriv), E(glSamplerParameteri), E(glSamplerParameterf),
    E(glTexImage2D), E(glTexSubImage2D), E(glCompressedTexImage2D), E(glGenerateMipmap),
    { "glGenerateMipmapOES", (void *)&glGenerateMipmap_na }, { "glGenerateMipmapEXT", (void *)&glGenerateMipmap_na },
    E(glCullFace), E(glFrontFace), E(glEnable), E(glDisable), E(glColorMask), E(glActiveTexture), E(glBindTexture), E(glBlendFunc), E(glBlendFuncSeparate), E(glColorMaski), E(glBlendEquationi), E(glBlendEquationSeparatei),
    E(glBlendFuncSeparatei),
    E(glVertexAttribIPointer), E(glUniform1uiv), E(glUniform2uiv), E(glUniform3uiv), E(glUniform4uiv),
    E(glGetUniformiv), E(glDrawElementsInstancedBaseVertex),
    { "glDrawElementsInstancedBaseVertexEXT", (void *)&glDrawElementsInstancedBaseVertex_na },
    { "glDrawElementsInstancedBaseVertexOES", (void *)&glDrawElementsInstancedBaseVertex_na },
    E(glDrawArrays), E(glDrawArraysInstanced), E(glDrawElements), E(glDrawElementsBaseVertex),
    E(glDrawElementsInstanced), E(glUseProgram), E(glLinkProgram), E(glDeleteProgram), E(glCreateProgram), E(glAttachShader), E(glDeleteShader), E(glCompileShader), E(glBindFramebuffer),
    E(glBindAttribLocation), E(glGetUniformLocation), E(glUniform4fv), E(glUniformMatrix4fv), E(glUniform1i),
    E(glBindBuffer), E(glBindBufferBase), E(glBindBufferRange), E(glDeleteBuffers), E(glBufferData), E(glBufferSubData), E(glMapBufferRange),
    { "glMapBufferRangeEXT", (void *)&glMapBufferRange_na }, E(glUnmapBuffer),
    { "glUnmapBufferOES", (void *)&glUnmapBuffer_na }, E(glFlushMappedBufferRange),
    { "glFlushMappedBufferRangeEXT", (void *)&glFlushMappedBufferRange_na }, E(glGetBufferParameteriv),
    E(glCopyBufferSubData),
    { "glDrawElementsBaseVertexEXT", (void *)&glDrawElementsBaseVertex_na },
    { "glDrawElementsBaseVertexOES", (void *)&glDrawElementsBaseVertex_na },
    { "glDrawArraysInstancedEXT", (void *)&glDrawArraysInstanced_na },
    { "glDrawElementsInstancedEXT", (void *)&glDrawElementsInstanced_na },
    E(glFenceSync), E(glClientWaitSync), E(glWaitSync), E(glDeleteSync), E(glIsSync), E(glGetSynciv),

    // Program binaries lose vitaGL's attribute bindings: let Unity compile from source
    HIDE(glGetProgramBinary), HIDE(glProgramBinary), HIDE(glGetProgramBinaryOES), HIDE(glProgramBinaryOES),

    // Stubs
    S(glTexImage3D, glTexImage3D), S(glTexSubImage3D, glTexSubImage3D), S(glTexStorage3D, glTexStorage3D),
    S(glTexStorage3DEXT, glTexStorage3D), S(glCompressedTexImage3D, glCompressedTexImage3D),
    S(glCompressedTexSubImage3D, glCompressedTexSubImage3D),
    S(glFramebufferTextureLayer, glFramebufferTextureLayer), S(glFramebufferTexture3D, glFramebufferTexture3D),
    S(glCopyImageSubData, glCopyImageSubData), S(glCopyImageSubDataEXT, glCopyImageSubData),
    S(glCopyImageSubDataOES, glCopyImageSubData),
    S(glTexBuffer, glTexBuffer), S(glTexBufferEXT, glTexBuffer),
    S(glTexStorage2DMultisample, glTexStorage2DMultisample), S(glTexStorage3DMultisample, glTexStorage3DMultisample),
    S(glTexImage2DMultisample, glTexImage2DMultisample),
    S(glFramebufferTexture2DMultisampleEXT, glFramebufferTexture2DMultisample),
    S(glFramebufferTexture2DMultisampleIMG, glFramebufferTexture2DMultisample),
    S(glResolveMultisampleFramebufferAPPLE, glResolveMultisampleFramebuffer),
    S(glTexPageCommitmentARB, glTexPageCommitment), S(glTexPageCommitmentEXT, glTexPageCommitment),
    S(glTextureView, glTextureView), S(glTextureViewEXT, glTextureView), S(glTextureViewOES, glTextureView),
    S(glGetTextureParameteriv, glGetTextureParameteriv),
    S(glDispatchCompute, glDispatchCompute), S(glDispatchComputeIndirect, glDispatchComputeIndirect),
    S(glMemoryBarrier, glMemoryBarrier), S(glBindImageTexture, glBindImageTexture),
    S(glDrawArraysIndirect, glDrawArraysIndirect), S(glDrawElementsIndirect, glDrawElementsIndirect),
    S(glPatchParameteri, glPatchParameteri), S(glPatchParameteriEXT, glPatchParameteri),
    S(glPatchParameteriOES, glPatchParameteri),
    S(glGetProgramInterfaceiv, glGetProgramInterfaceiv), S(glGetProgramResourceIndex, glGetProgramResourceIndex),
    S(glGetProgramResourceName, glGetProgramResourceName), S(glGetProgramResourceiv, glGetProgramResourceiv),
    S(glGenQueriesEXT, glGenQueriesEXT), S(glDeleteQueriesEXT, glDeleteQueriesEXT),
    S(glBeginQueryEXT, glBeginQueryEXT), S(glEndQueryEXT, glEndQueryEXT), S(glGetQueryiv, glGetQueryiv),
    S(glGetQueryObjectui64vEXT, glGetQueryObjectui64v), S(glGetQueryObjectui64vNV, glGetQueryObjectui64v),
    S(glProgramUniform1uiv, glProgramUniformExtra), S(glProgramUniform2uiv, glProgramUniformExtra),
    S(glProgramUniform3uiv, glProgramUniformExtra), S(glProgramUniform4uiv, glProgramUniformExtra),
    S(glProgramUniformMatrix2x3fv, glProgramUniformExtra), S(glProgramUniformMatrix2x4fv, glProgramUniformExtra),
    S(glProgramUniformMatrix3x2fv, glProgramUniformExtra), S(glProgramUniformMatrix3x4fv, glProgramUniformExtra),
    S(glProgramUniformMatrix4x2fv, glProgramUniformExtra), S(glProgramUniformMatrix4x3fv, glProgramUniformExtra),
    S(glBlendBarrier, glBlendBarrier), S(glBlendBarrierKHR, glBlendBarrier), S(glBlendBarrierNV, glBlendBarrier),
    S(glFramebufferTextureMultiviewOVR, glMultiview), S(glFramebufferTextureMultisampleMultiviewOVR, glMultiview),
    S(glDebugMessageCallback, glDebug), S(glDebugMessageCallbackKHR, glDebug),
    S(glDebugMessageControl, glDebug), S(glDebugMessageControlKHR, glDebug),
    S(glDebugMessageInsert, glDebug), S(glDebugMessageInsertKHR, glDebug),
    S(glObjectLabel, glDebug), S(glObjectLabelKHR, glDebug), S(glLabelObjectEXT, glDebug),
    S(glGetObjectLabel, glDebug), S(glGetObjectLabelKHR, glDebug), S(glGetObjectLabelEXT, glDebug),
    S(glPushDebugGroup, glDebug), S(glPushDebugGroupKHR, glDebug),
    S(glPopDebugGroup, glDebug), S(glPopDebugGroupKHR, glDebug),
    S(glPushGroupMarkerEXT, glDebug), S(glPopGroupMarkerEXT, glDebug),
    S(eglCreateSyncKHR, glDebug), S(eglDestroySyncKHR, glDebug), S(eglClientWaitSyncKHR, glDebug),
    S(eglGetFrameTimestampSupportedANDROID, glDebug), S(eglGetFrameTimestampsANDROID, glDebug),
    S(eglGetNextFrameIdANDROID, glDebug),
};

#define N_GL_TABLE ((int)(sizeof(gl_table) / sizeof(gl_table[0])))

// Each name is logged once: the log then lists every GL function Unity uses.
#define MAX_LOGGED 512
static const char *logged[MAX_LOGGED];
static int n_logged = 0;

static void log_lookup(const char *name, const char *how) {
    for (int i = 0; i < n_logged; i++)
        if (strcmp(logged[i], name) == 0) return;
    if (n_logged < MAX_LOGGED) logged[n_logged++] = strdup(name);
    l_info("[GL] %s: %s", name, how);
}

void *na_gl_get_proc(const char *name) {
    if (!name || (strncmp(name, "gl", 2) != 0 && strncmp(name, "egl", 3) != 0))
        return NULL;
    for (int i = 0; i < N_GL_TABLE; i++) {
        if (strcmp(name, gl_table[i].name) == 0) {
            log_lookup(name, gl_table[i].func ? "loader" : "hidden");
            return gl_table[i].func;
        }
    }
    if (strncmp(name, "egl", 3) == 0) return NULL;
    void *f = vglGetProcAddress(name);
    log_lookup(name, f ? "vitaGL" : "MISSING");
    return f;
}

/*
 * Screenshots for debugging: the screen and Unity's offscreen color target
 * (the first texture attached to a framebuffer), saved as TGA files next to
 * the log, with a brightness summary in the log.
 */
GLuint na_first_color_target = 0;
#define first_color_target na_first_color_target
static GLuint bound_draw_fb = 0, bound_read_fb = 0;

static void glBindFramebuffer_na(GLenum target, GLuint fb) {
    if (target == GL_FRAMEBUFFER || target == 0x8CA9) bound_draw_fb = fb; // DRAW_FRAMEBUFFER
    if (target == GL_FRAMEBUFFER || target == 0x8CA8) bound_read_fb = fb; // READ_FRAMEBUFFER
    glBindFramebuffer(target, fb);
}

static void save_pixels(const char *name, const uint8_t *px, int w, int h) {
    uint64_t sum[3] = { 0, 0, 0 };
    int lit = 0;
    for (int i = 0; i < w * h; i++) {
        const uint8_t *p = &px[i * 4];
        sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
        if (p[0] > 8 || p[1] > 8 || p[2] > 8) lit++;
    }
    l_info("[GL] screenshot %s: %dx%d, %d%% of pixels not black, average rgb %d/%d/%d", name, w, h,
           (int)((int64_t)lit * 100 / (w * h)), (int)(sum[0] / (w * h)), (int)(sum[1] / (w * h)), (int)(sum[2] / (w * h)));

    char path[128];
    snprintf(path, sizeof(path), DATA_PATH "%s.tga", name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint8_t hdr[18] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8), 32, 8 };
    fwrite(hdr, 1, 18, f);
    uint8_t *row = malloc(w * 4);
    for (int y = 0; y < h; y++) { // GL rows are bottom-up, as TGA's default
        for (int x = 0; x < w; x++) {
            const uint8_t *p = &px[(y * w + x) * 4];
            row[x * 4 + 0] = p[2]; row[x * 4 + 1] = p[1]; row[x * 4 + 2] = p[0]; row[x * 4 + 3] = 255;
        }
        fwrite(row, 1, w * 4, f);
    }
    free(row);
    fclose(f);
}

void na_gl_screenshot(int n) {
    const int w = 960, h = 544;
    uint8_t *px = malloc(w * h * 4);
    if (!px) return;
    glFinish();
    char name[32];

    // What is on screen (Unity has the default framebuffer bound when it swaps)
    if (bound_read_fb != 0) glBindFramebuffer(0x8CA8, 0);
    memset(px, 0, w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    snprintf(name, sizeof(name), "shot%d_screen", n);
    save_pixels(name, px, w, h);

    // Unity's offscreen target
    if (first_color_target) {
        GLuint fb = 0;
        glGenFramebuffers(1, &fb);
        glBindFramebuffer(0x8CA8, fb);
        glFramebufferTexture2D(0x8CA8, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, first_color_target, 0);
        memset(px, 0, w * h * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        snprintf(name, sizeof(name), "shot%d_target", n);
        save_pixels(name, px, w, h);
        glBindFramebuffer(0x8CA8, 0);
        glDeleteFramebuffers(1, &fb);
    }
    glBindFramebuffer(0x8CA8, bound_read_fb);
    glBindFramebuffer(0x8CA9, bound_draw_fb);
    free(px);
    for (int i = 0; i < n_dump_tex; i++) {
        char when[16];
        snprintf(when, sizeof(when), "shot%d", n);
        dump_tex_data(dump_tex[i], when);
    }
}

// The runtime shader compiler's messages (vitaGL only logs them in debug builds)
static void shark_log(const char *msg, shark_log_level level, int line) {
    static int lines = 0;
    if (level == SHARK_LOG_INFO || lines >= 400) return;
    lines++;
    l_warn("[shark] %s: %s (line %d)", level == SHARK_LOG_ERROR ? "error" : "warning", msg, line);
}

void na_gles3_init(void) {
    shark_install_log_cb(shark_log);
    // ETC2 textures (most of the game) are decoded and re-compressed to DXT5
    // instead of being stored as RGBA8 (4x the memory).
    glHint(GL_TEXTURE_COMPRESSION_HINT, GL_NICEST);
    // ...with stb_dxt's normal mode instead of its slow high-quality one:
    // the textures are re-encoded while the game runs, which made it stutter
    extern GLboolean fast_texture_compression;
    fast_texture_compression = GL_TRUE;
}

/*
 * Menu cursor: a small square drawn with scissored clears on the default
 * framebuffer just before the swap (no shader, no Unity state touched
 * except scissor, clear colour and colour mask, which are put back).
 */
void na_gl_draw_cursor(int x, int y) {
    GLint box[4] = { 0, 0, 0, 0 };
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, box);
    GLboolean cm[4] = { 1, 1, 1, 1 };
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    if (bound_draw_fb != 0) glBindFramebuffer(0x8CA9, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_SCISSOR_TEST);
    int gy = 544 - y; // GL counts from the bottom
    glScissor(x - 8, gy - 8, 16, 16);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(x - 6, gy - 6, 12, 12);
    glClearColor(0.95f, 0.85f, 0.30f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(x - 2, gy - 2, 4, 4);
    glClearColor(0.75f, 0.10f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
    glScissor(box[0], box[1], box[2], box[3]);
    if (!sc) glDisable(GL_SCISSOR_TEST);
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    if (bound_draw_fb != 0) glBindFramebuffer(0x8CA9, bound_draw_fb);
}

/*
 * Recent draws: the last RING_DRAWS draws are kept (cheaply) all the time;
 * L+R+DOWN writes them to the log, so a glitch seen a second or two before
 * the buttons are pressed is in it.
 */
#define RING_DRAWS 12000
typedef struct {
    int swap, draw;
    uint16_t prog, tex;
    int count, last;
    float lo[2], hi[2];
    float sx, sy, mx, my;
} ring_draw;
static ring_draw *ring;
static void check_outlier(ring_draw *r);
static int ring_pos = 0, ring_n = 0;

static void ring_record(int count, uint32_t last) {
#ifdef NA_RELEASE
    return;
#endif
    if (!ring) {
        ring = calloc(RING_DRAWS, sizeof(ring_draw));
        if (!ring) return;
    }
    ring_draw *r = &ring[ring_pos];
    r->swap = na_egl_swap_count;
    r->draw = draw_no;
    r->prog = (uint16_t)cur_program;
    r->tex = (uint16_t)unit_tex[0];
    r->count = count;
    r->last = (int)last;
    r->lo[0] = pos_lo[0]; r->lo[1] = pos_lo[1];
    r->hi[0] = pos_hi[0]; r->hi[1] = pos_hi[1];
    const float *m = cur_program < MAX_PROGS ? prog_o2w[cur_program] : NULL;
    r->sx = m ? m[0] : 0; r->sy = m ? m[5] : 0; r->mx = m ? m[12] : 0; r->my = m ? m[13] : 0;
    ring_pos = (ring_pos + 1) % RING_DRAWS;
    if (ring_n < RING_DRAWS) ring_n++;
    // check_outlier(r): off. Its 1 MB dumps froze the game for seconds, and
    // what it caught were batches of sprites far apart, not broken draws.
    (void)check_outlier;
}

static void dump_ring_to(FILE *f) {
    int start = (ring_pos - ring_n + RING_DRAWS) % RING_DRAWS;
    for (int i = 0; i < ring_n; i++) {
        ring_draw *r = &ring[(start + i) % RING_DRAWS];
        GLuint t = r->tex;
        fprintf(f, "f%d d%d p%u t%u(%dx%d) n%d v%d x %.1f..%.1f y %.1f..%.1f s %.2f/%.2f m %.1f/%.1f\n", r->swap, r->draw,
                r->prog, t, t < MAX_TEX_INFO ? tex_info[t].w : 0, t < MAX_TEX_INFO ? tex_info[t].h : 0, r->count, r->last,
                r->lo[0], r->hi[0], r->lo[1], r->hi[1], r->sx, r->sy, r->mx, r->my);
    }
}

// Writes the recent draws to ux0:data/intothedead/glitch_<frame>.txt
static void dump_ring_file(const char *why) {
    if (!ring || !ring_n) return;
    char path[96];
    snprintf(path, sizeof(path), DATA_PATH "glitch_%d.txt", na_egl_swap_count);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\nframe, draw, program, texture, indices, last vertex, x range, y range, o2w scale/move\n", why);
    dump_ring_to(f);
    fclose(f);
    l_warn("[RING] %s: %d recent draws written to %s", why, ring_n, path);
}

/*
 * Glitch detection: a draw whose vertices spread much wider than the same
 * program/texture usually does (a letter of a text popup, a particle quad
 * stretched over the screen) writes the recent draws to a file at once,
 * at most every 20 s.
 */
#define TYPICAL_N 512
static struct { uint16_t prog, tex; float size; } typical[TYPICAL_N];

static void check_outlier(ring_draw *r) {
    if (r->prog < 4 || (r->lo[0] == 0 && r->hi[0] == 0)) return; // programs 1-3: UI/blits
    float size = r->hi[0] - r->lo[0];
    if (r->hi[1] - r->lo[1] > size) size = r->hi[1] - r->lo[1];
    unsigned h = ((unsigned)r->prog * 31 + r->tex) % TYPICAL_N;
    if (typical[h].prog != r->prog || typical[h].tex != r->tex) {
        typical[h].prog = r->prog;
        typical[h].tex = r->tex;
        typical[h].size = size;
        return;
    }
    float t = typical[h].size;
    static uint64_t last_dump = 0;
    if (size > 25.0f && size > t * 6.0f && t > 0.01f) {
        uint64_t now = sceKernelGetProcessTimeWide();
        if (now - last_dump > 20000000ULL) {
            last_dump = now;
            char why[160];
            snprintf(why, sizeof(why), "draw %d (program %u, texture %u): size %.1f, usually %.1f", r->draw, r->prog,
                     r->tex, size, t);
            dump_ring_file(why);
        }
        return; // keep the typical size
    }
    typical[h].size = t * 0.95f + size * 0.05f;
}

void na_gl_dump_recent_draws(void) {
    dump_ring_file("L+R+DOWN");
    ring_n = 0;
}
