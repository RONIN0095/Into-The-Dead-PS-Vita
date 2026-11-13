/*
 * Host-side test for na_shaders_translate(): reads shader blobs extracted
 * from the game (tools/extract_shaders.py), translates every
 * "#version 300 es" stage and prints statistics plus a sample.
 *
 *   cc -O1 -Isource tools/shader_test.c source/na_shaders.c -o shader_test
 *   ./shader_test shaders/<name>.bin ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "na_shaders.h"

// A small shader in the shape hlslcc produces (written for this test)
static const char *sample_vs =
    "#version 300 es\n"
    "#define HLSLCC_ENABLE_UNIFORM_BUFFERS 1\n"
    "#if HLSLCC_ENABLE_UNIFORM_BUFFERS\n#define UNITY_UNIFORM\n#else\n#define UNITY_UNIFORM uniform\n#endif\n"
    "#define UNITY_SUPPORTS_UNIFORM_LOCATION 1\n"
    "#if UNITY_SUPPORTS_UNIFORM_LOCATION\n#define UNITY_LOCATION(x) layout(location = x)\n"
    "#define UNITY_BINDING(x) layout(binding = x, std140)\n#else\n#define UNITY_LOCATION(x)\n"
    "#define UNITY_BINDING(x) layout(std140)\n#endif\n"
    "#if HLSLCC_ENABLE_UNIFORM_BUFFERS\nUNITY_BINDING(0) uniform PerDraw {\n#endif\n"
    "\tUNITY_UNIFORM vec4 hlslcc_mtx4x4m[4];\n"
    "#if HLSLCC_ENABLE_UNIFORM_BUFFERS\n};\n#endif\n"
    "in highp vec4 in_POSITION0;\n"
    "out highp vec2 vs_TEXCOORD0;\n"
    "flat out highp int vs_INDEX0;\n"
    "void main() { gl_Position = hlslcc_mtx4x4m[0] * in_POSITION0.x; vs_TEXCOORD0 = in_POSITION0.xy; vs_INDEX0 = 0; }\n";
static const char *sample_fs =
    "#version 300 es\n"
    "precision highp float;\n"
    "UNITY_LOCATION(0) uniform mediump sampler2D _MainTex;\n"
    "in highp vec2 vs_TEXCOORD0;\n"
    "layout(location = 0) out mediump vec4 SV_Target0;\n"
    "void main() { SV_Target0 = texture(_MainTex, vs_TEXCOORD0) + textureLod(_MainTex, vs_TEXCOORD0, 0.0); }\n";

static int expect(const char *out, const char *what, int present) {
    int found = strstr(out, what) != NULL;
    if (found != present) {
        printf("FAIL: '%s' should %sbe in:\n%s\n", what, present ? "" : "not ", out);
        return 1;
    }
    return 0;
}

static int self_test(void) {
    int fails = 0, c;
    char *v = na_shaders_translate(sample_vs, 0, &c);
    char *f = na_shaders_translate(sample_fs, 1, &c);
    if (!v || !f) {
        printf("FAIL: not translated\n");
        return 1;
    }
    fails += expect(v, "#version", 0);
    fails += expect(v, "#define HLSLCC_ENABLE_UNIFORM_BUFFERS 0", 1);
    fails += expect(v, "#define UNITY_SUPPORTS_UNIFORM_LOCATION 0", 1);
    fails += expect(v, "layout", 0);
    fails += expect(v, "attribute highp vec4 in_POSITION0;", 1);
    fails += expect(v, "varying highp vec2 vs_TEXCOORD0;", 1);
    fails += expect(v, "varying highp int vs_INDEX0;", 1);
    fails += expect(f, "varying highp vec2 vs_TEXCOORD0;", 1);
    fails += expect(f, "#define SV_Target0 gl_FragColor", 1);
    fails += expect(f, "vec4 vgl_texture(sampler2D s, vec2 c)", 1);
    fails += expect(f, "vgl_texture(_MainTex, vs_TEXCOORD0)", 1);
    fails += expect(f, "vgl_textureLod(_MainTex", 1);
    fails += expect(f, " texture(", 0);
    fails += expect(f, "layout", 0);
    free(v);
    free(f);
    printf("self test: %s\n", fails ? "FAILED" : "ok");
    return fails;
}

int main(int argc, char **argv) {
    int stages = 0, leftovers = 0, shown = 0;
    if (argc < 2)
        return self_test() ? 1 : 0;
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *d = malloc(n + 1);
        if (fread(d, 1, n, f) != (size_t)n) n = 0;
        d[n] = 0;
        fclose(f);
        // blobs hold NUL-separated strings; programs are "#ifdef VERTEX ... #ifdef FRAGMENT ..."
        for (long i = 0; i < n;) {
            char *s = d + i;
            size_t len = strlen(s);
            char *v = strstr(s, "#ifdef VERTEX");
            char *fr = v ? strstr(v, "#ifdef FRAGMENT") : NULL;
            if (v && fr) {
                char *vs = strndup(v, fr - v);
                char *fs = strdup(fr);
                for (int st = 0; st < 2; st++) {
                    int changed;
                    char *out = na_shaders_translate(st ? fs : vs, st, &changed);
                    if (!out) continue;
                    stages++;
                    const char *bad[] = { "\nin ", "\nout ", "layout(", "#version" };
                    for (int b = 0; b < 4; b++)
                        if (strstr(out, bad[b])) { leftovers++; if (shown < 3) { printf("--- leftover '%s' in %s\n", bad[b], argv[a]); } break; }
                    if (shown < 2 && st == 1) { printf("===== %s (fragment)\n%s\n", argv[a], out); shown++; }
                    free(out);
                }
                free(vs);
                free(fs);
            }
            i += len + 1;
        }
        free(d);
    }
    printf("stages translated: %d, with leftovers: %d\n", stages, leftovers);
    return 0;
}
