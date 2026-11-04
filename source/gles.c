#include <math.h>
/**
 * @file gles.c
 * @brief OpenGL ES 1.1 pipeline translation over vitaGL.
 * @details The game renders into a virtual 320x480 portrait renderbuffer created on iOS by EAGLView
 *          (with internal projection rotation to landscape). Here that renderbuffer is an FBO with a texture
 *          of the same size: viewport, scissor, and glCopyTexImage2D remain in iOS coordinate space untouched.
 *          During presentation (gles_present), the FBO is drawn rotated and scaled onto the native 960x544 screen.
 *
 *          All imported gl* functions take only integer/pointer arguments (*x variants): no SoftFP issues.
 *          GL_FIXED vertex arrays are converted to float immediately before draw calls.
 */
#include <stdlib.h>
#include <string.h>
#include <vitaGL.h>

#include "gles.h"
#include "hle.h"
#include "imports.h"
#include "log.h"
#include "macho.h"
#include "objc.h"

#define FB_W 320
#define FB_H 480
#define SCR_W 960
#define SCR_H 544
// Destination rectangle: 480x320 scaled to Vita screen height (1.7x) and centered horizontally.
#define DST_H SCR_H
#define DST_W (DST_H * FB_H / FB_W)
#define DST_X ((SCR_W - DST_W) / 2)

#define GL_FRAMEBUFFER_COMPLETE_OES 0x8CD5
#define GL_RENDERBUFFER_WIDTH_OES 0x8D42
#define GL_RENDERBUFFER_HEIGHT_OES 0x8D43

static GLuint fbo, fbo_tex;

// Cached state overwritten during blit that glGet cannot fully query:
typedef struct {
    GLint size;
    GLenum type;
    GLsizei stride;
    const void *ptr;
} array_ptr;
static array_ptr vtx, tc[2], col;
static void *vtx_caller, *tc_caller; // guest code that set the arrays (Run 42 geometry diagnostics)
static int client_unit; // glClientActiveTexture - GL_TEXTURE0
static GLint env_mode = GL_MODULATE;
static GLfixed color[4] = {0x10000, 0x10000, 0x10000, 0x10000};

// ---------------------------------------------------------------- GL_FIXED arrays

static float *conv_buf[4];
static int conv_cap[4];

static const void *fixed_to_float(const array_ptr *a, int n, int slot) {
    int need = n * a->size;
    if (need > conv_cap[slot]) {
        conv_buf[slot] = realloc(conv_buf[slot], need * sizeof(float));
        conv_cap[slot] = need;
    }
    int step = a->stride ? a->stride / 4 : a->size;
    const int32_t *src = a->ptr;
    float *dst = conv_buf[slot];
    for (int i = 0; i < n; i++)
        for (int c = 0; c < a->size; c++)
            dst[i * a->size + c] = src[i * step + c] * (1.0f / 65536.0f);
    return dst;
}

static void apply_vertex(int n) {
    if (!vtx.size || (vtx.type == GL_FIXED && !n)) // GL_FIXED arrays are converted at draw time
        return;
    if (vtx.type == GL_FIXED)
        glVertexPointer(vtx.size, GL_FLOAT, 0, fixed_to_float(&vtx, n, 0));
    else
        glVertexPointer(vtx.size, vtx.type, vtx.stride, vtx.ptr);
}

static void apply_texcoord(int unit, int n) {
    array_ptr *a = &tc[unit];
    if (!a->size || (a->type == GL_FIXED && !n))
        return;
    glClientActiveTexture(GL_TEXTURE0 + unit);
    if (a->type == GL_FIXED)
        glTexCoordPointer(a->size, GL_FLOAT, 0, fixed_to_float(a, n, 1 + unit));
    else
        glTexCoordPointer(a->size, a->type, a->stride, a->ptr);
    glClientActiveTexture(GL_TEXTURE0 + client_unit);
}

// vitaGL's glColorPointer rejects GL_FIXED (GL_INVALID_ENUM, pointer left stale): convert at draw time.
static void apply_color(int n) {
    if (!col.size || (col.type == GL_FIXED && !n))
        return;
    if (col.type == GL_FIXED)
        glColorPointer(col.size, GL_FLOAT, 0, fixed_to_float(&col, n, 3));
    else
        glColorPointer(col.size, col.type, col.stride, col.ptr);
}

static void g_glColorPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    static int logged;
    if (logged++ < 4)
        l_info("[GLES] glColorPointer size=%d type=0x%X stride=%d", size, type, stride);
    col = (array_ptr){size, type, stride, ptr};
    apply_color(0);
}

static void g_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    vtx_caller = __builtin_return_address(0);
    vtx = (array_ptr){size, type, stride, ptr};
    apply_vertex(0);
}

static void g_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    if (!client_unit)
        tc_caller = __builtin_return_address(0);
    tc[client_unit] = (array_ptr){size, type, stride, ptr};
    apply_texcoord(client_unit, 0);
}

static void g_glClientActiveTexture(GLenum unit) {
    client_unit = (unit - GL_TEXTURE0) & 1;
    glClientActiveTexture(unit);
}

typedef struct { short w, h, levels; GLenum fmt; GLint minf; int nz; } tex_info;
static tex_info texinfo[4096];
static uint32_t gles_draw_calls;
static int trace_draws; // >0: log each draw of this frame (menu black-screen diagnostics)

// Minimal view of vitaGL's internal vbo struct (ptr is first member)
typedef struct {
    void *ptr;
    int32_t size;
} vgl_vbo;

// The game engine's internal GL state cache pointer is at *(uint32_t**)(MACHO_SLIDE + 0x3408a4).
// Offsets:
//   0x1C: current shader program
//   0x20: current GL_ARRAY_BUFFER binding
//   0x24: current GL_ELEMENT_ARRAY_BUFFER binding
//   0x30: current GL_FRAMEBUFFER binding
static inline uint32_t *get_engine_state_cache(void) {
    uint32_t **sc_pp = (uint32_t **)(MACHO_SLIDE + 0x3408a4);
    if (!sc_pp || !*sc_pp)
        return NULL;
    return *sc_pp;
}

static void sync_state_cache_buffer(GLenum target, GLuint buffer) {
    uint32_t *sc = get_engine_state_cache();
    if (!sc)
        return;
    if (target == GL_ARRAY_BUFFER)
        sc[0x20 / 4] = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER)
        sc[0x24 / 4] = buffer;
}

static void g_glBindBuffer(GLenum target, GLuint buffer) {
    static int logged;
    if (logged < 40) {
        logged++;
        l_info("[GLES] glBindBuffer target=0x%X buffer=%u", target, buffer);
    }
    glBindBuffer(target, buffer);
    sync_state_cache_buffer(target, buffer);
}

static void g_glDeleteBuffers(GLsizei n, const GLuint *buffers) {
    glDeleteBuffers(n, buffers);
    uint32_t *sc = get_engine_state_cache();
    if (!sc)
        return;
    for (int i = 0; i < n; i++) {
        if (sc[0x20 / 4] == buffers[i])
            sc[0x20 / 4] = 0;
        if (sc[0x24 / 4] == buffers[i])
            sc[0x24 / 4] = 0;
    }
}

static void trace_draw(const char *fn, GLenum mode, int count, GLenum itype, const void *indices) {
    if (!trace_draws || trace_draws > 40)
        return;
    trace_draws++;
    GLint ab = 0, eb = 0, tex = 0, fb = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &eb);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    l_info("[DRAW] %s mode=%u n=%d itype=0x%X idx=%p vtx=%d/0x%X/%d/%p tc0=%d/0x%X/%p vbo=%d ibo=%d tex=%d fb=%d va=%d tca=%d ca=%d "
           "light=%d blend=%d depth=%d env=0x%X err=0x%X",
           fn, mode, count, itype, indices, vtx.size, vtx.type, vtx.stride, vtx.ptr, tc[0].size, tc[0].type, tc[0].ptr, ab, eb, tex,
           fb, glIsEnabled(GL_VERTEX_ARRAY), glIsEnabled(GL_TEXTURE_COORD_ARRAY), glIsEnabled(GL_COLOR_ARRAY),
           glIsEnabled(GL_LIGHTING), glIsEnabled(GL_BLEND), glIsEnabled(GL_DEPTH_TEST), env_mode, glGetError());
    float pm[16], mv[16];
    glGetFloatv(GL_PROJECTION_MATRIX, pm);
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    const float *v = vtx.type == GL_FLOAT && !ab ? vtx.ptr : NULL;
    GLint bs, bd;
    glGetIntegerv(GL_BLEND_SRC, &bs);
    glGetIntegerv(GL_BLEND_DST, &bd);
    const uint8_t *c = col.type == GL_UNSIGNED_BYTE && !ab ? col.ptr : NULL;
    l_info("[DRAW]   blend=0x%X/0x%X c0=%02X%02X%02X%02X", bs, bd, c ? c[0] : 0, c ? c[1] : 0, c ? c[2] : 0, c ? c[3] : 0);
    GLint vpb[4], scb[4];
    glGetIntegerv(GL_VIEWPORT, vpb);
    glGetIntegerv(GL_SCISSOR_BOX, scb);
    l_info("[DRAW]   viewport=(%d,%d,%d,%d) scissor=(%d,%d,%d,%d) on=%d", vpb[0], vpb[1], vpb[2], vpb[3], scb[0], scb[1],
           scb[2], scb[3], glIsEnabled(GL_SCISSOR_TEST));
    tex_info *ti = (unsigned)tex < 4096 ? &texinfo[tex] : NULL;
    if (ti)
        l_info("[DRAW]   texinfo %dx%d fmt=0x%X levels=%d minf=0x%X nonzero_bytes=%d", ti->w, ti->h, ti->fmt, ti->levels,
               ti->minf, ti->nz);
    l_info("[DRAW]   col=%d/0x%X/%d v0=(%.1f,%.1f,%.1f) proj=[%.4f %.4f %.4f %.4f | %.2f %.2f %.2f] mv=[%.3f %.3f %.3f | %.1f %.1f %.1f]",
           col.size, col.type, col.stride, v ? v[0] : 0, v ? v[1] : 0, v ? v[2] : 0, pm[0], pm[5], pm[10], pm[15],
           pm[12], pm[13], pm[14], mv[0], mv[5], mv[10], mv[12], mv[13], mv[14]);
}

// Matrix calls of the traced frame: the menu's projection reads back as garbage (scale ~0, translate +1,+1).
#define MTRACE(...) do { if (trace_draws && trace_draws <= 40) l_info("[MTX] " __VA_ARGS__); } while (0)
static void g_glMatrixMode(GLenum m) { MTRACE("MatrixMode 0x%X", m); glMatrixMode(m); }
static void g_glLoadIdentity(void) { MTRACE("LoadIdentity"); glLoadIdentity(); }
static void g_glPushMatrix(void) { MTRACE("PushMatrix"); glPushMatrix(); }
static void g_glPopMatrix(void) { MTRACE("PopMatrix"); glPopMatrix(); }
static void g_glOrthox(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f) {
    MTRACE("Orthox %d %d %d %d %d %d (/65536)", l, r, b, t, n, f);
    glOrthox(l, r, b, t, n, f);
}
static void g_glLoadMatrixf(const GLfloat *m) {
    MTRACE("LoadMatrixf [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]", m[0], m[1],
           m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    glLoadMatrixf(m);
}
static void g_glMultMatrixf(const GLfloat *m) {
    MTRACE("MultMatrixf [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]", m[0], m[1],
           m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    glMultMatrixf(m);
}
static void g_glTranslatex(GLfixed x, GLfixed y, GLfixed z) { MTRACE("Translatex %d %d %d", x, y, z); glTranslatex(x, y, z); }
static void g_glRotatex(GLfixed a, GLfixed x, GLfixed y, GLfixed z) { MTRACE("Rotatex %d %d %d %d", a, x, y, z); glRotatex(a, x, y, z); }
static void g_glScalex(GLfixed x, GLfixed y, GLfixed z) { MTRACE("Scalex %d %d %d", x, y, z); glScalex(x, y, z); }
static void g_glTranslatef(GLfloat x, GLfloat y, GLfloat z) { MTRACE("Translatef %.3f %.3f %.3f", x, y, z); glTranslatef(x, y, z); }
static void g_glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) { MTRACE("Rotatef %.3f %.3f %.3f %.3f", a, x, y, z); glRotatef(a, x, y, z); }
static void g_glScalef(GLfloat x, GLfloat y, GLfloat z) { MTRACE("Scalef %.3f %.3f %.3f", x, y, z); glScalef(x, y, z); }

// GL error hunt: the title-screen background draw is preceded by GL_INVALID_ENUM every frame. Log the first
// offending calls (vitaGL glGetError is a plain variable read).
static int gl_err_logged;
#define ERRCHK(fmt, ...) do { GLenum e_ = glGetError(); if (e_ && gl_err_logged < 40) { gl_err_logged++; l_warn("[GLERR] 0x%X after " fmt, e_, __VA_ARGS__); } } while (0)
static void g_glEnable(GLenum c) {
    if (c == 0x1702 || c == 0x0BD0) // GL_TEXTURE matrix mode or GL_DITHER (unsupported by vitaGL)
        return;
    glEnable(c);
    ERRCHK("glEnable(0x%X)", c);
}
static void g_glDisable(GLenum c) {
    if (c == 0x1702 || c == 0x0BD0)
        return;
    glDisable(c);
    ERRCHK("glDisable(0x%X)", c);
}
static void g_glEnableClientState(GLenum c) { glEnableClientState(c); ERRCHK("glEnableClientState(0x%X)", c); }
static void g_glDisableClientState(GLenum c) { glDisableClientState(c); ERRCHK("glDisableClientState(0x%X)", c); }
static void g_glHint(GLenum t, GLenum m) {
    if (t == 0xC54 || t == 0x8192) // GL_GENERATE_MIPMAP_HINT (unsupported by vitaGL)
        return;
    if (m == 0)
        m = GL_DONT_CARE;
    glHint(t, m);
    ERRCHK("glHint(0x%X 0x%X)", t, m);
}
static void g_glBlendFunc(GLenum a, GLenum b) { glBlendFunc(a, b); ERRCHK("glBlendFunc(0x%X 0x%X)", a, b); }
static void g_glAlphaFunc(GLenum f, GLfloat r) { glAlphaFunc(f, r); ERRCHK("glAlphaFunc(0x%X %f)", f, r); }
static void g_glShadeModel(GLenum m) { glShadeModel(m); ERRCHK("glShadeModel(0x%X)", m); }
static void g_glDepthFunc(GLenum f) { glDepthFunc(f); ERRCHK("glDepthFunc(0x%X)", f); }
static void g_glTexEnvf(GLenum t, GLenum p, GLfloat v) { if (p == GL_TEXTURE_ENV_MODE) env_mode = (GLint)v; glTexEnvf(t, p, v); ERRCHK("glTexEnvf(0x%X 0x%X %f)", t, p, v); }
static void g_glTexEnvx(GLenum t, GLenum p, GLfixed v) { if (p == GL_TEXTURE_ENV_MODE) env_mode = v; glTexEnvx(t, p, v); ERRCHK("glTexEnvx(0x%X 0x%X %d)", t, p, v); }
static void g_glTexEnvfv(GLenum t, GLenum p, const GLfloat *v) { glTexEnvfv(t, p, (GLfloat *)v); ERRCHK("glTexEnvfv(0x%X 0x%X %f)", t, p, v[0]); }
static void g_glFogf(GLenum p, GLfloat v) { glFogf(p, v); ERRCHK("glFogf(0x%X %f)", p, v); }
static void g_glFogfv(GLenum p, const GLfloat *v) { glFogfv(p, v); ERRCHK("glFogfv(0x%X %f)", p, v[0]); }
static void g_glBindTexture(GLenum t, GLuint id) { glBindTexture(t, id); ERRCHK("glBindTexture(0x%X %u)", t, id); }
static void g_glActiveTexture(GLenum t) { glActiveTexture(t); ERRCHK("glActiveTexture(0x%X)", t); }
static void g_glCullFace(GLenum m) { glCullFace(m); ERRCHK("glCullFace(0x%X)", m); }
static void g_glFrontFace(GLenum m) { glFrontFace(m); ERRCHK("glFrontFace(0x%X)", m); }
static void g_glPixelStorei(GLenum p, GLint v) { glPixelStorei(p, v); ERRCHK("glPixelStorei(0x%X %d)", p, v); }
static void g_glMaterialf(GLenum f, GLenum p, GLfloat v) { glMaterialf(f, p, v); ERRCHK("glMaterialf(0x%X 0x%X %f)", f, p, v); }
static void g_glMaterialfv(GLenum f, GLenum p, const GLfloat *v) { glMaterialfv(f, p, v); ERRCHK("glMaterialfv(0x%X 0x%X %f)", f, p, v[0]); }
static void g_glLightfv(GLenum l, GLenum p, const GLfloat *v) { glLightfv(l, p, v); ERRCHK("glLightfv(0x%X 0x%X %f)", l, p, v[0]); }
static void g_glLightModelfv(GLenum p, const GLfloat *v) { glLightModelfv(p, v); ERRCHK("glLightModelfv(0x%X %f)", p, v[0]); }

// Per-texture upload record (texinfo), printed with each traced draw: which bound texture is empty or incomplete.

static tex_info *cur_tex(void) {
    GLint t;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &t);
    return (unsigned)t < 4096 ? &texinfo[t] : NULL;
}

static void g_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt,
                           GLenum type, const void *data) {
    glTexImage2D(target, level, ifmt, w, h, border, fmt, type, data);
    GLenum err = glGetError();
    tex_info *ti = cur_tex();
    if (!ti)
        return;
    if (level == 0) {
        int bytes = w * h * (type == GL_UNSIGNED_BYTE ? (fmt == GL_RGBA ? 4 : fmt == GL_RGB ? 3 : 1) : 2), nz = 0;
        for (int i = 0; data && i < bytes; i++)
            nz += ((const uint8_t *)data)[i] != 0;
        *ti = (tex_info){w, h, 1, type, ti->minf, nz};
        if (err)
            l_warn("[TEX] glTexImage2D %dx%d fmt=0x%X type=0x%X err=0x%X", w, h, fmt, type, err);
    } else
        ti->levels++;
}

static void g_glCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border,
                                     GLsizei size, const void *data) {
    glCompressedTexImage2D(target, level, ifmt, w, h, border, size, data);
    GLenum err = glGetError();
    tex_info *ti = cur_tex();
    if (!ti)
        return;
    if (level == 0) {
        int nz = 0;
        for (int i = 0; data && i < size; i++)
            nz += ((const uint8_t *)data)[i] != 0;
        *ti = (tex_info){w, h, 1, ifmt, ti->minf, nz};
        if (err)
            l_warn("[TEX] glCompressedTexImage2D %dx%d ifmt=0x%X err=0x%X", w, h, ifmt, err);
    } else
        ti->levels++;
}

static void g_glTexParameteri(GLenum target, GLenum pname, GLint v) {
    glTexParameteri(target, pname, v);
    tex_info *ti = cur_tex();
    if (ti && pname == GL_TEXTURE_MIN_FILTER)
        ti->minf = v;
}

uint32_t gles_get_draw_calls(void) {
    return gles_draw_calls;
}

// What the traced draw really feeds the GPU (indexed vertices only) and how many FBO pixels it touched.
static void trace_geometry(int count, const uint16_t *idx) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
    int a0 = 255, a1 = 0, imax = 0;
    int vs = vtx.stride ? vtx.stride / 4 : vtx.size, ts = tc[0].stride ? tc[0].stride / 4 : tc[0].size;
    for (int i = 0; i < count; i++) {
        int k = idx[i];
        imax = k > imax ? k : imax;
        const float *p = (const float *)vtx.ptr + k * vs, *t = (const float *)tc[0].ptr + k * ts;
        x0 = fminf(x0, p[0]), x1 = fmaxf(x1, p[0]), y0 = fminf(y0, p[1]), y1 = fmaxf(y1, p[1]);
        u0 = fminf(u0, t[0]), u1 = fmaxf(u1, t[0]), v0 = fminf(v0, t[1]), v1 = fmaxf(v1, t[1]);
        if (col.type == GL_UNSIGNED_BYTE && col.ptr) {
            int a = ((const uint8_t *)col.ptr)[k * (col.stride ? col.stride : 4) + 3];
            a0 = a < a0 ? a : a0, a1 = a > a1 ? a : a1;
        }
    }
    l_info("[GEOM] idx0..%d xy=(%.1f,%.1f)-(%.1f,%.1f) uv=(%.3f,%.3f)-(%.3f,%.3f) alpha=%d..%d", imax, x0, y0, x1, y1, u0, v0,
           u1, v1, a0, a1);
}

static void trace_coverage(void) {
    static uint32_t *px;
    if (!px)
        px = malloc(FB_W * FB_H * 4);
    glFinish();
    glReadPixels(0, 0, FB_W, FB_H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    int n = 0;
    for (int i = 0; i < FB_W * FB_H; i++)
        n += px[i] != 0;
    l_info("[GEOM] after draw: %d FBO pixels non-zero", n);
}

void heap_pin(const void *p); // darwin.c

// Run 47: the game draws from arrays it already freed (benign on iOS). Every client array a draw reads is pinned
// so the allocator never hands it to someone else (no-op for live blocks and non-heap pointers).
static void pin_draw_arrays(const void *indices) {
    GLint ab = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
    if (!ab) {
        heap_pin(vtx.ptr), heap_pin(tc[0].ptr), heap_pin(tc[1].ptr), heap_pin(col.ptr);
    }
    if ((uintptr_t)indices >= 4096)
        heap_pin(indices);
}

static void g_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    gles_draw_calls++;
    pin_draw_arrays(NULL);
    trace_draw("DrawArrays", mode, count, 0, NULL);
    int n = first + count;
    if (vtx.type == GL_FIXED)
        apply_vertex(n);
    for (int u = 0; u < 2; u++)
        if (tc[u].type == GL_FIXED)
            apply_texcoord(u, n);
    if (col.type == GL_FIXED)
        apply_color(n);
    glDrawArrays(mode, first, count);
}

static inline GLuint get_current_program(void) {
    GLint prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    if (prog)
        return (GLuint)prog;
    uint32_t *sc = get_engine_state_cache();
    if (sc && sc[0x1C / 4])
        return sc[0x1C / 4];
    return 0;
}

static void g_glEnableVertexAttribArray(GLuint index) {
    if (!get_current_program()) {
        if (index == 0)
            glEnableClientState(GL_VERTEX_ARRAY);
        else if (index == 1)
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        else if (index == 2)
            glEnableClientState(GL_COLOR_ARRAY);
        return;
    }
    glEnableVertexAttribArray(index);
}

static void g_glDisableVertexAttribArray(GLuint index) {
    if (!get_current_program()) {
        if (index == 0)
            glDisableClientState(GL_VERTEX_ARRAY);
        else if (index == 1)
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        else if (index == 2)
            glDisableClientState(GL_COLOR_ARRAY);
        return;
    }
    glDisableVertexAttribArray(index);
}

static void g_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *ptr) {
    (void)normalized;
    if (!get_current_program()) {
        if (index == 0)
            g_glVertexPointer(size, type, stride, ptr);
        else if (index == 1)
            g_glTexCoordPointer(size, type, stride, ptr);
        else if (index == 2)
            g_glColorPointer(size, type, stride, ptr);
        if (index == 0)
            vtx_caller = __builtin_return_address(0);
        else if (index == 1 && !client_unit)
            tc_caller = __builtin_return_address(0);
        return;
    }
    glVertexAttribPointer(index, size, type, normalized, stride, ptr);
}

static uint16_t s_quad_indices[512 * 6];
static int s_quad_indices_inited = 0;

static void init_quad_indices(void) {
    if (s_quad_indices_inited)
        return;
    for (int q = 0; q < 512; q++) {
        uint16_t base = q * 4;
        s_quad_indices[q * 6 + 0] = base + 0;
        s_quad_indices[q * 6 + 1] = base + 3;
        s_quad_indices[q * 6 + 2] = base + 2;
        s_quad_indices[q * 6 + 3] = base + 0;
        s_quad_indices[q * 6 + 4] = base + 2;
        s_quad_indices[q * 6 + 5] = base + 1;
    }
    s_quad_indices_inited = 1;
}

// ---------------------------------------------------------------- bad-geometry detector (Run 46)
// Runs on EVERY client-side indexed float draw. A vertex is bad if its x/y/z is NaN, |v| > 1e6, or one of the
// heap poison words (0xFDFDFDFD redzone = read past a block's end, 0xDDDDDDDD = block already freed, see darwin.c).
// Triangles that touch a bad vertex are dropped (they are the long black wedges) and the draw is logged in full once
// per (caller, vtx, count): the guest mesh object (r4 at the call, see g_glDrawElements_thunk), its vtable and fields,
// and which heap block each array lives in.
void heap_describe(const void *p, char *out, int n); // darwin.c
uint32_t draw_r4 __attribute__((used)); // guest r4 at glDrawElements: the mesh object in the 0x200ED0 renderer

static inline int bad_word(uint32_t w) {
    if (w == 0xFDFDFDFDu || w == 0xDDDDDDDDu)
        return 1;
    float f;
    memcpy(&f, &w, 4);
    return !(f > -1e6f && f < 1e6f); // also NaN
}

static uint16_t clean_idx[65536];
static uint8_t vbad[65536]; // 0 unknown, 1 ok, 2 bad (per draw)

static void baddraw_log(void *caller, int count, const uint16_t *idx, int imax, int nbad_tri, int first_bad_pos,
                        uint32_t bad_v, uint32_t bad_w, int tc_on) {
    static struct { uint32_t c, v, n; } seen[96];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (seen[i].c == (uint32_t)caller && seen[i].v == (uint32_t)vtx.ptr && seen[i].n == (uint32_t)count)
            return;
    if (nseen >= 96)
        return;
    seen[nseen].c = (uint32_t)caller, seen[nseen].v = (uint32_t)vtx.ptr, seen[nseen].n = count, nseen++;
    int vs = vtx.stride ? vtx.stride / 4 : vtx.size, ts = tc[0].stride ? tc[0].stride / 4 : tc[0].size;
    char bi[96], bv[96], bt[96], bo[96];
    heap_describe(idx, bi, sizeof bi), heap_describe(vtx.ptr, bv, sizeof bv), heap_describe(tc[0].ptr, bt, sizeof bt);
    heap_describe((void *)draw_r4, bo, sizeof bo);
    l_warn("[BADDRAW] #%d caller=0x%06X n=%d (n%%3=%d) imax=%d bad_tris=%d/%d first_bad: idx[%d]=%u word=0x%08X | vtx set by 0x%06X, tc set by "
           "0x%06X, tc_on=%d, vs=%d ts=%d",
           nseen, (unsigned)((uintptr_t)caller - MACHO_SLIDE), count, count % 3, imax, nbad_tri, count / 3, first_bad_pos,
           (unsigned)bad_v, (unsigned)bad_w, (unsigned)((uintptr_t)vtx_caller - MACHO_SLIDE),
           (unsigned)((uintptr_t)tc_caller - MACHO_SLIDE), tc_on, vs, ts);
    l_warn("[BADDRAW]   idx=%s need %d", bi, count * 2);
    l_warn("[BADDRAW]   vtx=%s need %d", bv, (imax + 1) * vs * 4);
    l_warn("[BADDRAW]   tc =%s need %d", bt, (imax + 1) * ts * 4);
    l_warn("[BADDRAW]   r4=0x%08X %s", (unsigned)draw_r4, bo);
    if (draw_r4 >= 0x81000000u && draw_r4 < 0xA0000000u) {
        const uint32_t *o = (const uint32_t *)draw_r4;
        uint32_t vt = o[0];
        l_warn("[BADDRAW]   r4 vtable=0x%06X fields[0..0x60]: %08X %08X %08X %08X %08X %08X %08X %08X | %08X %08X %08X %08X %08X "
               "%08X %08X %08X | %08X %08X %08X %08X %08X %08X %08X %08X",
               (unsigned)(vt - MACHO_SLIDE), o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7], o[8], o[9], o[10], o[11], o[12],
               o[13], o[14], o[15], o[16], o[17], o[18], o[19], o[20], o[21], o[22], o[23]);
        if (vt >= MACHO_SLIDE && vt < MACHO_SLIDE + 0x35B000) {
            const uint32_t *v = (const uint32_t *)vt;
            l_warn("[BADDRAW]   vtable[0..16]: %06X %06X %06X %06X %06X %06X %06X %06X %06X %06X %06X %06X %06X %06X %06X %06X",
                   v[0] - MACHO_SLIDE, v[1] - MACHO_SLIDE, v[2] - MACHO_SLIDE, v[3] - MACHO_SLIDE, v[4] - MACHO_SLIDE,
                   v[5] - MACHO_SLIDE, v[6] - MACHO_SLIDE, v[7] - MACHO_SLIDE, v[8] - MACHO_SLIDE, v[9] - MACHO_SLIDE,
                   v[10] - MACHO_SLIDE, v[11] - MACHO_SLIDE, v[12] - MACHO_SLIDE, v[13] - MACHO_SLIDE, v[14] - MACHO_SLIDE,
                   v[15] - MACHO_SLIDE);
        }
    }
    l_warn("[BADDRAW]   idx[0..11]=%u %u %u %u %u %u %u %u %u %u %u %u", idx[0], idx[1], idx[2], idx[3], idx[4], idx[5], idx[6],
           idx[7], idx[8], idx[9], idx[10], idx[11]);
}

// Returns indices to draw (original or filtered into clean_idx) and updates *count. NULL: draw nothing.
static const uint16_t *filter_bad_geometry(void *caller, GLenum mode, int *count, const uint16_t *idx) {
    if (mode != GL_TRIANGLES || vtx.type != GL_FLOAT || !vtx.ptr || !glIsEnabled(GL_VERTEX_ARRAY))
        return idx;
    int tc_on = glIsEnabled(GL_TEXTURE_COORD_ARRAY) && tc[0].type == GL_FLOAT && tc[0].ptr;
    int n = *count, vs = vtx.stride ? vtx.stride / 4 : vtx.size, ts = tc[0].stride ? tc[0].stride / 4 : tc[0].size;
    int imax = 0;
    for (int i = 0; i < n; i++)
        imax = idx[i] > imax ? idx[i] : imax;
    memset(vbad, 0, imax + 1);
    const uint32_t *V = vtx.ptr, *T = tc[0].ptr;
    int nbad = 0, first = -1, out = 0;
    uint32_t bw = 0;
    for (int t = 0; t + 2 < n; t += 3) {
        int bad = 0;
        for (int k = 0; k < 3; k++) {
            int v = idx[t + k];
            if (!vbad[v]) {
                int b = 0;
                for (int c = 0; c < vtx.size && !b; c++)
                    if (bad_word(V[v * vs + c]))
                        b = 1, bw = V[v * vs + c];
                for (int c = 0; tc_on && c < 2 && !b; c++)
                    if (bad_word(T[v * ts + c]))
                        b = 1, bw = T[v * ts + c];
                vbad[v] = b ? 2 : 1;
            }
            if (vbad[v] == 2) {
                bad = 1;
                if (first < 0)
                    first = t + k;
            }
        }
        if (bad)
            nbad++;
        else
            clean_idx[out++] = idx[t], clean_idx[out++] = idx[t + 1], clean_idx[out++] = idx[t + 2];
    }
    if (!nbad && n % 3 == 0)
        return idx;
    baddraw_log(caller, n, idx, imax, nbad, first, first >= 0 ? idx[first] : 0, bw, tc_on);
    *count = out; // also drops a trailing partial triangle (n % 3 != 0)
    if (!nbad)
        return idx;
    return out ? clean_idx : NULL;
}

__attribute__((used, noinline)) void g_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);
__attribute__((used, noinline)) void g_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    gles_draw_calls++;
    void *caller = __builtin_return_address(0);
    init_quad_indices();

    GLint eb = 0;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &eb);

    // If indices is an offset into an IBO (NULL or < 4096), an IBO MUST be bound in OpenGL.
    // If not bound, resync from the engine's internal state cache.
    if ((uintptr_t)indices < 4096) {
        if (!eb) {
            uint32_t *sc = get_engine_state_cache();
            if (sc && sc[0x24 / 4] != 0) {
                l_warn("[GLES] glDrawElements indices=%p with ibo=0! Restoring cached ibo=%u",
                       indices, sc[0x24 / 4]);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sc[0x24 / 4]);
                eb = sc[0x24 / 4];
            } else {
                l_error("[GLES] glDrawElements indices=%p with NO IBO bound! Dropping call to avoid crash", indices);
                return;
            }
        }
    }

    pin_draw_arrays(eb ? NULL : indices); // before the quad repair below swaps in host indices
    // Quad index table repair for the 2D GUI meshes whose shared index buffer holds pointers
    // (0x200f7c: raw words 0x8336xxxx). Run 41: matching "not 0,3,2" also caught every 3D mesh
    // (same generic draw 0x200f7c), drawing them with fake quad indices -> broken geometry.
    if (mode == GL_TRIANGLES && !eb && type == GL_UNSIGNED_SHORT && (uintptr_t)indices >= 4096 && count >= 6 && (count % 6) == 0) {
        const uint16_t *u16 = (const uint16_t *)indices;
        // High halves of 0x8xxxxxxx/0x9xxxxxxx pointers; a real mesh never has 3 indices >= 32768 here.
        int holds_pointers = u16[1] >= 0x8000 && u16[1] < 0xA000 && u16[3] >= 0x8000 && u16[3] < 0xA000 &&
                             u16[5] >= 0x8000 && u16[5] < 0xA000;
        if (holds_pointers) {
            static int bad_idx_logged;
            if (bad_idx_logged < 20) {
                bad_idx_logged++;
                l_warn("[GLES] Quad draw (caller=%p, count=%d) raw indices=[%u, %u, %u, %u, %u, %u] -> substituting clean quad index buffer",
                       caller, count, u16[0], u16[1], u16[2], u16[3], u16[4], u16[5]);
            }
            // Never write back into guest memory: these "indices" can be live guest data (Run 39: sprite page table).
            indices = s_quad_indices;
            if (count > 512 * 6)
                count = 512 * 6;
        }
    }

    trace_draw("DrawElements", mode, count, type, indices);

    // GL_FIXED arrays are converted at draw time: they need the vertex count, i.e. max index + 1.
    // ONLY scan client indices when NO element array buffer is bound.
    if (!eb && (vtx.type == GL_FIXED || tc[0].type == GL_FIXED || tc[1].type == GL_FIXED || col.type == GL_FIXED)) {
        int n = 0;
        for (int i = 0; i < count; i++) {
            int v = type == GL_UNSIGNED_BYTE ? ((const uint8_t *)indices)[i] : ((const uint16_t *)indices)[i];
            if (v >= n)
                n = v + 1;
        }
        if (vtx.type == GL_FIXED)
            apply_vertex(n);
        for (int u = 0; u < 2; u++)
            if (tc[u].type == GL_FIXED)
                apply_texcoord(u, n);
        if (col.type == GL_FIXED)
            apply_color(n);
    }

    if (!eb && type == GL_UNSIGNED_SHORT && (uintptr_t)indices >= 4096 && count <= 65535) {
        indices = filter_bad_geometry(caller, mode, &count, indices);
        if (!indices || !count)
            return;
    }

    if (trace_draws && trace_draws <= 41 && vtx.type == GL_FLOAT && tc[0].type == GL_FLOAT && type == GL_UNSIGNED_SHORT) {
        const uint16_t *idx_ptr = NULL;
        if (eb) {
            vgl_vbo *gpu_buf = (vgl_vbo *)eb;
            if (gpu_buf && gpu_buf->ptr)
                idx_ptr = (const uint16_t *)((const uint8_t *)gpu_buf->ptr + (uintptr_t)indices);
        } else if ((uintptr_t)indices >= 4096) {
            idx_ptr = (const uint16_t *)indices;
        }
        if (idx_ptr) {
            l_info("[GEOM] draw from guest 0x%06X, vtx set by 0x%06X, tc set by 0x%06X, idx[0..5]=%u %u %u %u %u %u",
                   (unsigned)((uintptr_t)caller - MACHO_SLIDE), (unsigned)((uintptr_t)vtx_caller - MACHO_SLIDE),
                   (unsigned)((uintptr_t)tc_caller - MACHO_SLIDE), idx_ptr[0], idx_ptr[1], idx_ptr[2], idx_ptr[3],
                   idx_ptr[4], idx_ptr[5]);
            trace_geometry(count, idx_ptr);
        }
    }

    glDrawElements(mode, count, type, indices);
    if (trace_draws && trace_draws <= 41)
        trace_coverage();
}

// Records the guest's r4 (callee-saved: still the caller's value here) then tail-calls g_glDrawElements with lr intact.
__attribute__((naked, target("arm"))) static void g_glDrawElements_thunk(void) {
    __asm__ volatile("ldr ip, =draw_r4\n"
                     "str r4, [ip]\n"
                     "b g_glDrawElements\n"
                     ".ltorg\n");
}

static void g_glGetPointerv(GLenum pname, void **out) {
    if (pname == GL_VERTEX_ARRAY_POINTER)
        *out = (void *)vtx.ptr;
    else if (pname == GL_TEXTURE_COORD_ARRAY_POINTER)
        *out = (void *)tc[client_unit].ptr;
    else
        glGetPointerv(pname, out);
}

// ---------------------------------------------------------------- *x state

static void g_glColor4x(GLfixed r, GLfixed g, GLfixed b, GLfixed a) {
    color[0] = r, color[1] = g, color[2] = b, color[3] = a;
    glColor4x(r, g, b, a);
}

static void g_glTexEnvi(GLenum target, GLenum pname, GLint v) {
    if (pname == GL_TEXTURE_ENV_MODE)
        env_mode = v;
    glTexEnvi(target, pname, v);
}

static void g_glTexEnvxv(GLenum target, GLenum pname, const GLfixed *v) {
    if (pname == GL_TEXTURE_ENV_MODE) // enum value, not fixed point
        return g_glTexEnvi(target, pname, v[0]);
    glTexEnvxv(target, pname, (GLfixed *)v);
}

// Texture parameters used by the engine are enums (filters, wrap modes): pass unscaled.
static void g_glTexParameterx(GLenum target, GLenum pname, GLfixed v) { glTexParameteri(target, pname, v); }

static void g_glGetFixedv(GLenum pname, GLfixed *out) {
    float f[16];
    int n = 1;
    if (pname == GL_MODELVIEW_MATRIX || pname == GL_PROJECTION_MATRIX || pname == GL_TEXTURE_MATRIX)
        n = 16;
    else if (pname == GL_CURRENT_COLOR) {
        memcpy(out, color, sizeof(color));
        return;
    }
    glGetFloatv(pname, f);
    for (int i = 0; i < n; i++)
        out[i] = (GLfixed)(f[i] * 65536.0f);
}

static void g_glGetLightxv(GLenum light, GLenum pname, GLfixed *out) { memset(out, 0, 4 * sizeof(GLfixed)); }

// ---------------------------------------------------------------- EAGLView framebuffer emulation

static GLuint bound_fb;
static GLuint bound_rb, drawable_rb;
static GLuint depth_fbs[64];
static int ndepth_fbs;

void gles_bind_drawable(void) {
    drawable_rb = bound_rb;
    l_info("EAGL drawable renderbuffer = %u (%ux%u)", drawable_rb, FB_W, FB_H);
}

static void g_glBindFramebufferOES(GLenum target, GLuint fb) {
    bound_fb = fb;
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
}

static void g_glBindRenderbufferOES(GLenum target, GLuint rb) {
    bound_rb = rb;
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
}

static void g_glFramebufferRenderbufferOES(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb) {
    if (rb && rb == drawable_rb && attachment == GL_COLOR_ATTACHMENT0) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint *)&fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
        l_info("EAGL window framebuffer = %u (attached fbo_tex)", fbo);
        return;
    }
    if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_STENCIL_ATTACHMENT) {
        GLint cur;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &cur);
        for (int i = 0; i < ndepth_fbs; i++)
            if (depth_fbs[i] == (GLuint)cur)
                return;
        if (rb && ndepth_fbs < 64)
            depth_fbs[ndepth_fbs++] = cur;
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER, rb);
}

static void g_glDeleteFramebuffersOES(GLsizei n, const GLuint *ids) {
    for (int k = 0; k < n; k++)
        for (int i = 0; i < ndepth_fbs; i++)
            if (depth_fbs[i] == ids[k])
                depth_fbs[i--] = depth_fbs[--ndepth_fbs];
    glDeleteFramebuffers(n, ids);
}

static void g_glNoop(void) {}

static void g_glGetRenderbufferParameterivOES(GLenum target, GLenum pname, GLint *v) {
    *v = pname == GL_RENDERBUFFER_WIDTH_OES ? FB_W : pname == GL_RENDERBUFFER_HEIGHT_OES ? FB_H : 0;
}

// ---------------------------------------------------------------- init / present

void gles_init(void) {
    vglUseTripleBuffering(GL_FALSE);
    // vitaGL takes every pool but these thresholds. sceVideodecInitLibrary + the decoder frame buffer need
    // PHYCONT pages: with phycont_threshold 0 (vglInitExtended) it failed 0x80024302 with phycont 0 KB free.
    vglInitWithCustomThreshold(0, SCR_W, SCR_H, 16 * 1024 * 1024, 0, 16 * 1024 * 1024,
                               0x8C6000 /* vitaGL default cdlg threshold */, SCE_GXM_MULTISAMPLE_NONE);
    glGenTextures(1, &fbo_tex);
    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FB_W, FB_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glViewport(0, 0, FB_W, FB_H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    vglSwapBuffers(GL_FALSE);
    l_info("vitaGL: FBO %ux%u -> screen %ux%u at x=%d", FB_W, FB_H, DST_W, DST_H, DST_X);
}

static uint32_t gles_clears;
static void g_glClear(GLbitfield mask) {
    gles_clears++;
    if (gles_clears == 1 || (gles_clears % 120) == 0) {
        l_info("[GLES] glClear mask=0x%X (total clears %u)", mask, gles_clears);
    }
    glClear(mask);
}

void heap_redzone_sweep(void); // darwin.c

void gles_present(void) {
    static uint32_t s_presents;
    objc_pump();
    s_presents++;
    if (s_presents == 1 || (s_presents % 120) == 0) {
        l_info("[GLES] Frame %u presented (%u draw calls this frame)", s_presents, gles_draw_calls);
        heap_redzone_sweep();
        // Intro state 0x89c80 calls startMovie: only when IsLoading() (0x215720) == 0 and [**0x34051c + 9] == 0
        // (literal 0x8a01c; 0x34051c is a local nl_symbol_ptr to the global 0x34a830, null until created).
        uint32_t mp = *(uint32_t *)(MACHO_SLIDE + 0x35a724);
        uint32_t pp = *(uint32_t *)(MACHO_SLIDE + 0x34051c), obj = pp ? *(uint32_t *)pp : 0;
        l_info("[MOVIE] player=0x%08X isLoading=%u gate[+9]=%s", mp, *(uint8_t *)(MACHO_SLIDE + 0x34df40),
               obj ? (*(uint8_t *)(obj + 9) ? "1" : "0") : "null");
    }
    gles_draw_calls = 0;
    trace_draws = (s_presents == 120 || s_presents == 240 || (s_presents % 600) == 0); // trace every draw of the next frame

    if (s_presents == 1 || (s_presents % 120) == 0) {
        // Did the guest draw anything into the FBO? Sample a few pixels before the blit.
        // Whole-FBO scan: count non-black pixels and their bounding box (point samples missed sparse menu sprites).
        static uint32_t *px;
        if (!px)
            px = malloc(FB_W * FB_H * 4);
        glReadPixels(0, 0, FB_W, FB_H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        int lit = 0, x0 = FB_W, y0 = FB_H, x1 = -1, y1 = -1;
        uint32_t sample = 0;
        for (int y = 0; y < FB_H; y++)
            for (int x = 0; x < FB_W; x++)
                if (px[y * FB_W + x] & 0x00FFFFFF) {
                    if (!lit++)
                        sample = px[y * FB_W + x];
                    x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1, y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
                }
        l_info("[GLES] fbo non-black=%d bbox=(%d,%d)-(%d,%d) first=%08X", lit, x0, y0, x1, y1, sample);
    }

    // Every cap the guest can leave on that would hide the blit quad: the 3D menu leaves lighting/culling
    // enabled, which lit the normal-less quad black (or culled it). GL_TEXTURE_2D must stay last (enabled).
    static const GLenum caps[] = {GL_BLEND, GL_ALPHA_TEST, GL_SCISSOR_TEST, GL_DEPTH_TEST,
                                  GL_LIGHTING, GL_CULL_FACE, GL_FOG, GL_TEXTURE_2D};
    enum { NCAPS = sizeof(caps) / sizeof(caps[0]) };
    GLboolean on[NCAPS], tca, va, ca, na;
    GLint tex, mode, active, vp[4];
    for (int i = 0; i < NCAPS; i++)
        on[i] = glIsEnabled(caps[i]);
    if (s_presents == 1 || (s_presents % 120) == 0)
        l_info("[GLES] guest caps blend=%d alpha=%d scissor=%d depth=%d light=%d cull=%d fog=%d tex=%d", on[0], on[1],
               on[2], on[3], on[4], on[5], on[6], on[7]);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    tca = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
    va = glIsEnabled(GL_VERTEX_ARRAY);
    ca = glIsEnabled(GL_COLOR_ARRAY);
    na = glIsEnabled(GL_NORMAL_ARRAY);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glGetIntegerv(GL_VIEWPORT, vp);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, SCR_W, SCR_H);
    for (int i = 0; i < NCAPS - 1; i++)
        glDisable(caps[i]);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0, SCR_W, SCR_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glClear(GL_COLOR_BUFFER_BIT);
    // Screen corners (top-left, top-right, bottom-left, bottom-right) -> vertical FBO texture.
    // Assumed landscape-right rotation (top edge of FBO aligns to left edge of screen).
    // Adjust UV mapping table if target game renders mirrored or inverted.
    static const float uv[] = {1, 1, 1, 0, 0, 1, 0, 0};
    const float xy[] = {DST_X, 0, DST_X + DST_W, 0, DST_X, SCR_H, DST_X + DST_W, SCR_H};
    glVertexPointer(2, GL_FLOAT, 0, xy);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    vglSwapBuffers(ime_active()); // GL_TRUE while system dialog is active

    // Restore guest OpenGL state.
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(mode);
    glBindFramebuffer(GL_FRAMEBUFFER, bound_fb ? bound_fb : fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, env_mode);
    for (int i = 0; i < NCAPS; i++)
        on[i] ? glEnable(caps[i]) : glDisable(caps[i]);
    if (!va)
        glDisableClientState(GL_VERTEX_ARRAY);
    if (!tca)
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (ca)
        glEnableClientState(GL_COLOR_ARRAY);
    if (na)
        glEnableClientState(GL_NORMAL_ARRAY);
    glColor4x(color[0], color[1], color[2], color[3]);
    apply_vertex(0);
    apply_texcoord(0, 0);
    apply_texcoord(1, 0);
    apply_color(0);
    glClientActiveTexture(GL_TEXTURE0 + client_unit);
    glActiveTexture(active);

    // Keep engine's GL state cache in sync: clear cached buffer bindings between frames
    // so any draw call requiring an IBO/VBO is forced to explicitly bind it.
    uint32_t *sc = get_engine_state_cache();
    if (sc) {
        sc[0x20 / 4] = 0;
        sc[0x24 / 4] = 0;
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static void g_glLightf(GLenum light, GLenum pname, GLfloat param) {
    GLfloat v = param;
    glLightfv(light, pname, &v);
}

static void g_glValidateProgram(GLuint program) {
    (void)program;
}

// ---------------------------------------------------------------- import table

typedef struct { void *isa; uint32_t flags; const char *str; uint32_t len; } cf_str_gles;
#define DEF_CFSTR_GLES(name, str_val) \
    static const cf_str_gles cf_##name = { NULL, 0x7C8, str_val, sizeof(str_val) - 1 }; \
    static const void *ptr_##name = &cf_##name

DEF_CFSTR_GLES(kEAGLColorFormatRGB565, "kEAGLColorFormatRGB565");
DEF_CFSTR_GLES(kEAGLDrawablePropertyColorFormat, "kEAGLDrawablePropertyColorFormat");
DEF_CFSTR_GLES(kEAGLDrawablePropertyRetainedBacking, "kEAGLDrawablePropertyRetainedBacking");

const import_entry gles_imports[] = {
    F(glActiveTexture, g_glActiveTexture), F(glAlphaFunc, g_glAlphaFunc), D(glAttachShader),
    D(glBindAttribLocation), F(glBindBuffer, g_glBindBuffer),
    F(glBindFramebuffer, g_glBindFramebufferOES), F(glBindFramebufferOES, g_glBindFramebufferOES),
    F(glBindRenderbuffer, g_glBindRenderbufferOES), F(glBindRenderbufferOES, g_glBindRenderbufferOES),
    F(glBindTexture, g_glBindTexture), F(glBlendFunc, g_glBlendFunc),
    D(glBufferData), D(glBufferSubData),
    F(glClear, g_glClear), D(glClearColor),
    F(glColorPointer, g_glColorPointer), F(glColor4x, g_glColor4x),
    D(glCompileShader), F(glCompressedTexImage2D, g_glCompressedTexImage2D),
    D(glCopyTexImage2D), D(glCopyTexSubImage2D),
    D(glCreateProgram), D(glCreateShader),
    F(glCullFace, g_glCullFace),
    F(glDeleteBuffers, g_glDeleteBuffers), F(glDeleteFramebuffersOES, g_glDeleteFramebuffersOES),
    D(glDeleteProgram), F(glDeleteRenderbuffersOES, glDeleteRenderbuffers),
    D(glDeleteShader), D(glDeleteTextures),
    F(glDepthFunc, g_glDepthFunc), D(glDepthMask),
    F(glDisable, g_glDisable), F(glDisableClientState, g_glDisableClientState), F(glDisableVertexAttribArray, g_glDisableVertexAttribArray),
    F(glDrawArrays, g_glDrawArrays), F(glDrawElements, g_glDrawElements_thunk),
    F(glEnable, g_glEnable), F(glEnableClientState, g_glEnableClientState), F(glEnableVertexAttribArray, g_glEnableVertexAttribArray),
    D(glFinish), F(glFogf, g_glFogf), F(glFogfv, g_glFogfv),
    F(glFramebufferRenderbuffer, g_glFramebufferRenderbufferOES), F(glFramebufferRenderbufferOES, g_glFramebufferRenderbufferOES),
    D(glFramebufferTexture2D), F(glFrontFace, g_glFrontFace),
    D(glGenBuffers), F(glGenFramebuffers, glGenFramebuffers), F(glGenFramebuffersOES, glGenFramebuffers),
    F(glGenRenderbuffers, glGenRenderbuffers), F(glGenRenderbuffersOES, glGenRenderbuffers),
    D(glGenTextures),
    D(glGetAttribLocation), D(glGetError), D(glGetIntegerv),
    D(glGetProgramInfoLog), D(glGetProgramiv),
    D(glGetShaderInfoLog), D(glGetShaderiv),
    D(glGetUniformLocation), F(glHint, g_glHint),
    D(glIsEnabled),
    F(glLightModelfv, g_glLightModelfv), F(glLightf, g_glLightf), F(glLightfv, g_glLightfv),
    D(glLinkProgram), F(glLoadIdentity, g_glLoadIdentity), F(glLoadMatrixf, g_glLoadMatrixf), D(glLoadMatrixx),
    F(glMaterialf, g_glMaterialf), F(glMaterialfv, g_glMaterialfv), F(glMatrixMode, g_glMatrixMode), F(glMultMatrixf, g_glMultMatrixf),
    D(glNormalPointer), F(glOrthox, g_glOrthox), F(glPixelStorei, g_glPixelStorei),
    F(glPopMatrix, g_glPopMatrix), F(glPushMatrix, g_glPushMatrix),
    F(glRenderbufferStorage, glRenderbufferStorage), F(glRenderbufferStorageOES, glRenderbufferStorage),
    F(glRotatef, g_glRotatef), F(glRotatex, g_glRotatex), F(glScalef, g_glScalef), F(glScalex, g_glScalex),
    D(glScissor), F(glShadeModel, g_glShadeModel), D(glShaderSource),
    F(glTexCoordPointer, g_glTexCoordPointer),
    F(glTexEnvf, g_glTexEnvf), F(glTexEnvfv, g_glTexEnvfv), F(glTexEnvi, g_glTexEnvi), F(glTexEnvx, g_glTexEnvx), F(glTexEnvxv, g_glTexEnvxv),
    F(glTexImage2D, g_glTexImage2D), F(glTexParameteri, g_glTexParameteri), F(glTexParameterx, g_glTexParameterx), D(glTexSubImage2D),
    F(glTranslatef, g_glTranslatef), F(glTranslatex, g_glTranslatex),
    D(glUniform1f), D(glUniform1i), D(glUniform2fv), D(glUniform3fv), D(glUniform4fv), D(glUniformMatrix4fv),
    D(glUseProgram), F(glValidateProgram, g_glValidateProgram),
    F(glVertexPointer, g_glVertexPointer), F(glVertexAttribPointer, g_glVertexAttribPointer),
    D(glViewport),
    F(glGetFixedv, g_glGetFixedv), F(glGetLightxv, g_glGetLightxv),
    F(glClientActiveTexture, g_glClientActiveTexture),
    F(glGetPointerv, g_glGetPointerv),
    F(glCheckFramebufferStatusOES, glCheckFramebufferStatus),
    F(glGetRenderbufferParameterivOES, g_glGetRenderbufferParameterivOES),
    V(kEAGLColorFormatRGB565, &ptr_kEAGLColorFormatRGB565),
    V(kEAGLDrawablePropertyColorFormat, &ptr_kEAGLDrawablePropertyColorFormat),
    V(kEAGLDrawablePropertyRetainedBacking, &ptr_kEAGLDrawablePropertyRetainedBacking),
};
const int gles_imports_count = sizeof(gles_imports) / sizeof(gles_imports[0]);
