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
    col = (array_ptr){size, type, stride, ptr};
    apply_color(0);
}

static void g_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    vtx = (array_ptr){size, type, stride, ptr};
    apply_vertex(0);
}

static void g_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    tc[client_unit] = (array_ptr){size, type, stride, ptr};
    apply_texcoord(client_unit, 0);
}

static void g_glClientActiveTexture(GLenum unit) {
    client_unit = (unit - GL_TEXTURE0) & 1;
    glClientActiveTexture(unit);
}

static uint32_t gles_draw_calls;

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

static void g_glEnable(GLenum c) {
    if (c == 0x1702 || c == 0x0BD0) // GL_TEXTURE matrix mode or GL_DITHER (unsupported by vitaGL)
        return;
    glEnable(c);
}
static void g_glDisable(GLenum c) {
    if (c == 0x1702 || c == 0x0BD0)
        return;
    glDisable(c);
}
static void g_glHint(GLenum t, GLenum m) {
    if (t == 0xC54 || t == 0x8192) // GL_GENERATE_MIPMAP_HINT (unsupported by vitaGL)
        return;
    if (m == 0)
        m = GL_DONT_CARE;
    glHint(t, m);
}
static void g_glTexEnvf(GLenum t, GLenum p, GLfloat v) { if (p == GL_TEXTURE_ENV_MODE) env_mode = (GLint)v; glTexEnvf(t, p, v); }
static void g_glTexEnvx(GLenum t, GLenum p, GLfixed v) { if (p == GL_TEXTURE_ENV_MODE) env_mode = v; glTexEnvx(t, p, v); }

uint32_t gles_get_draw_calls(void) {
    return gles_draw_calls;
}

void heap_pin(const void *p); // darwin.c

// Run 47: the game draws from arrays it already freed (benign on iOS). Every client array a draw reads is pinned
// so the allocator never hands it to someone else (no-op for live blocks and non-heap pointers).
static void pin_draw_arrays(const void *indices) {
    heap_pin(vtx.ptr), heap_pin(tc[0].ptr), heap_pin(tc[1].ptr), heap_pin(col.ptr), heap_pin(indices);
}

static void g_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    gles_draw_calls++;
    pin_draw_arrays(NULL);
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

static void g_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
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

    glDrawElements(mode, count, type, indices);
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

void gles_present(void) {
    static uint32_t s_presents;
    objc_pump();
    s_presents++;
    if (s_presents == 1 || (s_presents % 120) == 0) {
        l_info("[GLES] Frame %u presented (%u draw calls this frame)", s_presents, gles_draw_calls);
    }
    gles_draw_calls = 0;

    // Every cap the guest can leave on that would hide the blit quad: the 3D menu leaves lighting/culling
    // enabled, which lit the normal-less quad black (or culled it). GL_TEXTURE_2D must stay last (enabled).
    static const GLenum caps[] = {GL_BLEND, GL_ALPHA_TEST, GL_SCISSOR_TEST, GL_DEPTH_TEST,
                                  GL_LIGHTING, GL_CULL_FACE, GL_FOG, GL_TEXTURE_2D};
    enum { NCAPS = sizeof(caps) / sizeof(caps[0]) };
    GLboolean on[NCAPS], tca, va, ca, na;
    GLint tex, mode, active, vp[4];
    for (int i = 0; i < NCAPS; i++)
        on[i] = glIsEnabled(caps[i]);
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
    D(glActiveTexture), D(glAlphaFunc), D(glAttachShader),
    D(glBindAttribLocation), F(glBindBuffer, g_glBindBuffer),
    F(glBindFramebuffer, g_glBindFramebufferOES), F(glBindFramebufferOES, g_glBindFramebufferOES),
    F(glBindRenderbuffer, g_glBindRenderbufferOES), F(glBindRenderbufferOES, g_glBindRenderbufferOES),
    D(glBindTexture), D(glBlendFunc),
    D(glBufferData), D(glBufferSubData),
    D(glClear), D(glClearColor),
    F(glColorPointer, g_glColorPointer), F(glColor4x, g_glColor4x),
    D(glCompileShader), D(glCompressedTexImage2D),
    D(glCopyTexImage2D), D(glCopyTexSubImage2D),
    D(glCreateProgram), D(glCreateShader),
    D(glCullFace),
    F(glDeleteBuffers, g_glDeleteBuffers), F(glDeleteFramebuffersOES, g_glDeleteFramebuffersOES),
    D(glDeleteProgram), F(glDeleteRenderbuffersOES, glDeleteRenderbuffers),
    D(glDeleteShader), D(glDeleteTextures),
    D(glDepthFunc), D(glDepthMask),
    F(glDisable, g_glDisable), D(glDisableClientState), F(glDisableVertexAttribArray, g_glDisableVertexAttribArray),
    F(glDrawArrays, g_glDrawArrays), F(glDrawElements, g_glDrawElements),
    F(glEnable, g_glEnable), D(glEnableClientState), F(glEnableVertexAttribArray, g_glEnableVertexAttribArray),
    D(glFinish), D(glFogf), D(glFogfv),
    F(glFramebufferRenderbuffer, g_glFramebufferRenderbufferOES), F(glFramebufferRenderbufferOES, g_glFramebufferRenderbufferOES),
    D(glFramebufferTexture2D), D(glFrontFace),
    D(glGenBuffers), F(glGenFramebuffers, glGenFramebuffers), F(glGenFramebuffersOES, glGenFramebuffers),
    F(glGenRenderbuffers, glGenRenderbuffers), F(glGenRenderbuffersOES, glGenRenderbuffers),
    D(glGenTextures),
    D(glGetAttribLocation), D(glGetError), D(glGetIntegerv),
    D(glGetProgramInfoLog), D(glGetProgramiv),
    D(glGetShaderInfoLog), D(glGetShaderiv),
    D(glGetUniformLocation), F(glHint, g_glHint),
    D(glIsEnabled),
    D(glLightModelfv), F(glLightf, g_glLightf), D(glLightfv),
    D(glLinkProgram), D(glLoadIdentity), D(glLoadMatrixf), D(glLoadMatrixx),
    D(glMaterialf), D(glMaterialfv), D(glMatrixMode), D(glMultMatrixf),
    D(glNormalPointer), D(glOrthox), D(glPixelStorei),
    D(glPopMatrix), D(glPushMatrix),
    F(glRenderbufferStorage, glRenderbufferStorage), F(glRenderbufferStorageOES, glRenderbufferStorage),
    D(glRotatef), D(glRotatex), D(glScalef), D(glScalex),
    D(glScissor), D(glShadeModel), D(glShaderSource),
    F(glTexCoordPointer, g_glTexCoordPointer),
    F(glTexEnvf, g_glTexEnvf), D(glTexEnvfv), F(glTexEnvi, g_glTexEnvi), F(glTexEnvx, g_glTexEnvx), F(glTexEnvxv, g_glTexEnvxv),
    D(glTexImage2D), D(glTexParameteri), F(glTexParameterx, g_glTexParameterx), D(glTexSubImage2D),
    D(glTranslatef), D(glTranslatex),
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
