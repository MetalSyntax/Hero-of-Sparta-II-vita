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
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vitaGL.h>

#include "cgbi.h"
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
// Fullscreen: the 480x320 game image is stretched to the whole 960x544 screen (x2 horizontally, x1.7
// vertically). input.c maps touches with the same stretch. (Run 51: the Zombie Infection style wider field of
// view left the iPhone-anchored HUD and cutscene letterbox short of the screen edges, with bare 3D beside them.)
#define DST_H SCR_H
#define DST_W SCR_W
#define DST_X 0

#define GL_FRAMEBUFFER_COMPLETE_OES 0x8CD5
#define GL_RENDERBUFFER_WIDTH_OES 0x8D42
#define GL_RENDERBUFFER_HEIGHT_OES 0x8D43

static GLuint fbo, fbo_tex;
// Shown instead of the empty FBO until the game draws its first frame (~20 s of sound loading at boot).
static GLuint splash_tex;

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
static const void *hud_filter(GLenum mode, int n, int count);
static void hud_restore(const void *f);

// Run 47: the game draws from arrays it already freed (benign on iOS). Every client array a draw reads is pinned
// so the allocator never hands it to someone else (no-op for live blocks and non-heap pointers).
static void pin_draw_arrays(const void *indices) {
    heap_pin(vtx.ptr), heap_pin(tc[0].ptr), heap_pin(tc[1].ptr), heap_pin(col.ptr), heap_pin(indices);
}

static uint64_t draw_us; // time spent in draw calls (wrapper + vitaGL), reported with the frame log
static void g_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
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
    const void *h = hud_filter(mode, n, count);
    glDrawArrays(mode, first, count);
    hud_restore(h);
    draw_us += sceKernelGetProcessTimeWide() - t0;
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
    uint64_t t0 = sceKernelGetProcessTimeWide();
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

    const void *h = NULL;
    if (!eb) {
        int n = 0;
        for (int i = 0; i < count; i++) {
            int v = type == GL_UNSIGNED_BYTE ? ((const uint8_t *)indices)[i] : ((const uint16_t *)indices)[i];
            if (v >= n)
                n = v + 1;
        }
        h = hud_filter(mode, n, count);
    }
    glDrawElements(mode, count, type, indices);
    hud_restore(h);
    draw_us += sceKernelGetProcessTimeWide() - t0;
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

// Draws into the window framebuffer (where the HUD lives), as opposed to offscreen render targets.
static int wide_target(void) { return fbo && bound_fb == fbo; }

// ---------------------------------------------------------------- touch controls opacity (L + R)

// The game has no opacity setting and no symbols, so the on-screen controls are hidden at draw level: in 2D
// (orthographic) quad batches drawn with the HUD texture, every quad sitting on a control (joystick, attack,
// jump, defense; positions in 480x320 game space, measured on console screenshots) collapses to a point.
// The HUD texture is learned from the first square sprite drawn exactly on a control, so menus that use
// other textures are never touched. Touch input is unaffected (it is read before drawing).
int gles_hud_hidden = 1;
static const struct { float x, y, r; } hud_spots[] = {
    {69, 250, 60},  // virtual joystick (base + knob travel)
    {395, 234, 52}, // attack (circle + crosshair lines)
    {455, 191, 26}, // jump
    {351, 294, 26}, // defense
};
#define NSPOTS (sizeof(hud_spots) / sizeof(hud_spots[0]))
static GLuint hud_tex[4];
static float *hud_buf;
static int hud_cap;

static void g_glDeleteTextures(GLsizei n, const GLuint *ids) {
    for (int k = 0; k < n; k++)
        for (unsigned i = 0; i < 4; i++)
            if (hud_tex[i] == ids[k])
                hud_tex[i] = 0;
    glDeleteTextures(n, ids);
}

static float vtx_get(int i, int c) {
    int es = vtx.type == GL_SHORT ? 2 : vtx.type == GL_BYTE ? 1 : 4;
    const uint8_t *p = (const uint8_t *)vtx.ptr + i * (vtx.stride ? vtx.stride : vtx.size * es) + c * es;
    switch (vtx.type) {
    case GL_FLOAT: return *(const float *)p;
    case GL_FIXED: return *(const int32_t *)p * (1.0f / 65536.0f);
    case GL_SHORT: return *(const int16_t *)p;
    default: return *(const int8_t *)p;
    }
}

static int hud_spot(float cx, float cy) {
    for (unsigned s = 0; s < NSPOTS; s++) {
        float dx = cx - hud_spots[s].x, dy = cy - hud_spots[s].y;
        if (dx * dx + dy * dy <= hud_spots[s].r * hud_spots[s].r)
            return s;
    }
    return -1;
}

// Returns the vertex array to draw with (hidden quads collapsed) or NULL to draw the game's own.
static const void *hud_filter(GLenum mode, int n, int count) {
    if (!gles_hud_hidden || !wide_target() || !vtx.ptr || vtx.size < 2 || n < 4 || (n & 3) || n > 4096)
        return NULL;
    // Quad batches only: indexed triangles (6 indices per 4 vertices) or one 4-vertex strip/fan sprite.
    if (!(mode == GL_TRIANGLES && count == n / 4 * 6) && !((mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN) && n == 4))
        return NULL;
    GLint ab = 0, tex = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
    if (ab)
        return NULL;
    float pm[16], mv[16], m[16];
    glGetFloatv(GL_PROJECTION_MATRIX, pm);
    // 2D (orthographic) only: perspective has w row (0, 0, -1, 0). Run 53: the HUD projection has pm[11] = -1
    // (w = 1 - z, z = 0 for sprites), so only pm[15] tells them apart.
    if (pm[15] == 0)
        return NULL;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    int known = 0;
    for (unsigned i = 0; i < 4; i++)
        known |= tex && hud_tex[i] == (GLuint)tex;
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            m[c * 4 + r] = pm[r] * mv[c * 4] + pm[4 + r] * mv[c * 4 + 1] + pm[8 + r] * mv[c * 4 + 2] + pm[12 + r] * mv[c * 4 + 3];
    int hidden = 0;
    for (int q = 0; q < n; q += 4) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int i = q; i < q + 4; i++) {
            float x = vtx_get(i, 0), y = vtx_get(i, 1), z = vtx.size > 2 ? vtx_get(i, 2) : 0;
            float nx = m[0] * x + m[4] * y + m[8] * z + m[12], ny = m[1] * x + m[5] * y + m[9] * z + m[13];
            // NDC -> 480x320 landscape game space (inverse of the blit in gles_present).
            float gx = 240 * (1 - ny), gy = 160 * (1 - nx);
            x0 = gx < x0 ? gx : x0, x1 = gx > x1 ? gx : x1, y0 = gy < y0 ? gy : y0, y1 = gy > y1 ? gy : y1;
        }
        float w = x1 - x0, h = y1 - y0, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        if (w > 110 || h > 110)
            continue;
        int s = hud_spot(cx, cy);
        if (s < 0)
            continue;
        if (!known) {
            // Learn: a square control-sized sprite centered on a control.
            float dx = cx - hud_spots[s].x, dy = cy - hud_spots[s].y;
            if (!tex || w < 24 || h < 24 || w > 1.3f * h || h > 1.3f * w || dx * dx + dy * dy > 12 * 12)
                continue;
            for (unsigned i = 0; i < 4; i++)
                if (!hud_tex[i]) {
                    hud_tex[i] = tex;
                    break;
                }
            known = 1;
            l_info("[HUD] control texture %d learned from a %dx%d sprite at (%d,%d)", tex, (int)w, (int)h, (int)cx, (int)cy);
            q = -4; // rescan this batch now that the texture is known
            hidden = 0;
            continue;
        }
        if (!hidden) {
            if (n * vtx.size > hud_cap) {
                hud_cap = n * vtx.size;
                hud_buf = realloc(hud_buf, hud_cap * sizeof(float));
            }
            for (int i = 0; i < n; i++)
                for (int c = 0; c < vtx.size; c++)
                    hud_buf[i * vtx.size + c] = vtx_get(i, c);
        }
        hidden++;
        for (int i = q + 1; i < q + 4; i++)
            memcpy(&hud_buf[i * vtx.size], &hud_buf[q * vtx.size], vtx.size * sizeof(float));
    }
    if (!hidden)
        return NULL;
    glVertexPointer(vtx.size, GL_FLOAT, 0, hud_buf);
    return hud_buf;
}

static void hud_restore(const void *f) {
    if (f && vtx.type != GL_FIXED) // GL_FIXED arrays are re-converted by the next draw anyway
        apply_vertex(0);
}

static void g_glGetRenderbufferParameterivOES(GLenum target, GLenum pname, GLint *v) {
    *v = pname == GL_RENDERBUFFER_WIDTH_OES ? FB_W : pname == GL_RENDERBUFFER_HEIGHT_OES ? FB_H : 0;
}

// ---------------------------------------------------------------- init / present

void gles_init(void) {
    // Run 54: gameplay frames take a bit over 33 ms; with double buffering + vsync each one waited for the third
    // vblank (20 fps). A third display buffer (2 MB) lets the game start the next frame instead of waiting.
    vglUseTripleBuffering(GL_TRUE);
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

    // iOS launch image (portrait, Apple CgBI PNG), drawn rotated 90 degrees counter-clockwise to landscape.
    void *px = NULL, *file = NULL;
    int w = 0, h = 0, bpp, len = 0;
    FILE *f = fopen("ux0:data/heroofspartaii/game_app/Default@2x.png", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        file = malloc(len);
        if (file && fread(file, 1, len, f) == (size_t)len)
            px = cgbi_decode(file, len, &w, &h, &bpp, malloc);
        fclose(f);
    }
    if (px) {
        glGenTextures(1, &splash_tex);
        glBindTexture(GL_TEXTURE_2D, splash_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // Drawn here directly: gles_present touches guest memory, which is not loaded yet (run 52 crash).
        static const float xy[] = {0, 0, SCR_W, 0, 0, SCR_H, SCR_W, SCR_H}, uv[] = {1, 0, 1, 1, 0, 0, 0, 1};
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, SCR_W, SCR_H);
        glEnable(GL_TEXTURE_2D);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrthof(0, SCR_W, SCR_H, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glVertexPointer(2, GL_FLOAT, 0, xy);
        glTexCoordPointer(2, GL_FLOAT, 0, uv);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        vglSwapBuffers(GL_FALSE); // stays on screen until the next swap
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, FB_W, FB_H);
    }
    free(file);
    free(px);
    if (!splash_tex)
        l_warn("splash: cannot load game_app/Default@2x.png");
    l_info("vitaGL: FBO %ux%u -> screen %ux%u at x=%d", FB_W, FB_H, DST_W, DST_H, DST_X);
}

void gles_present(void) {
    static uint32_t s_presents;
    objc_pump();
    s_presents++;
    static uint64_t s_swap_us, s_last;
    if (s_presents == 1 || (s_presents % 120) == 0) {
        uint64_t now = sceKernelGetProcessTimeWide();
        l_info("[GLES] Frame %u presented (%u draw calls this frame), %.1f fps, swap wait %.1f ms/frame, draws %.1f ms/frame",
               s_presents, gles_draw_calls, s_last ? 120e6 / (double)(now - s_last) : 0.0, s_swap_us / 120e3,
               draw_us / 120e3);
        s_last = now, s_swap_us = 0, draw_us = 0;
    }
    // The first frame with draws ends the splash; until then show it instead of the empty FBO.
    if (splash_tex && gles_draw_calls) {
        glDeleteTextures(1, &splash_tex);
        splash_tex = 0;
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

    glBindTexture(GL_TEXTURE_2D, splash_tex ? splash_tex : fbo_tex);
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
    static const float fbo_uv[] = {1, 1, 1, 0, 0, 1, 0, 0}, splash_uv[] = {1, 0, 1, 1, 0, 0, 0, 1};
    const float *uv = splash_tex ? splash_uv : fbo_uv;
    const float xy[] = {DST_X, 0, DST_X + DST_W, 0, DST_X, SCR_H, DST_X + DST_W, SCR_H};
    glVertexPointer(2, GL_FLOAT, 0, xy);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    uint64_t t0 = sceKernelGetProcessTimeWide();
    vglSwapBuffers(ime_active()); // GL_TRUE while system dialog is active
    s_swap_us += sceKernelGetProcessTimeWide() - t0;

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
    D(glDeleteShader), F(glDeleteTextures, g_glDeleteTextures),
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
