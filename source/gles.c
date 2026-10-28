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
static array_ptr vtx, tc[2];
static int client_unit; // glClientActiveTexture - GL_TEXTURE0
static GLint env_mode = GL_MODULATE;
static GLfixed color[4] = {0x10000, 0x10000, 0x10000, 0x10000};

// ---------------------------------------------------------------- GL_FIXED arrays

static float *conv_buf[3];
static int conv_cap[3];

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

uint32_t gles_get_draw_calls(void) {
    return gles_draw_calls;
}

static void g_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    gles_draw_calls++;
    int n = first + count;
    if (vtx.type == GL_FIXED)
        apply_vertex(n);
    for (int u = 0; u < 2; u++)
        if (tc[u].type == GL_FIXED)
            apply_texcoord(u, n);
    glDrawArrays(mode, first, count);
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
    vglInitExtended(0, SCR_W, SCR_H, 8 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
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

void gles_present(void) {
    static uint32_t s_presents;
    s_presents++;
    if (s_presents == 1 || (s_presents % 120) == 0) {
        l_info("[GLES] Frame %u presented (%u draw calls)", s_presents, gles_draw_calls);
    }

    static const GLenum caps[] = {GL_BLEND, GL_ALPHA_TEST, GL_SCISSOR_TEST, GL_DEPTH_TEST, GL_TEXTURE_2D};
    GLboolean on[5], tca, va, ca, na;
    GLint tex, mode, active, vp[4];
    for (int i = 0; i < 5; i++)
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
    for (int i = 0; i < 4; i++)
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
    for (int i = 0; i < 5; i++)
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
    glClientActiveTexture(GL_TEXTURE0 + client_unit);
    glActiveTexture(active);
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
    D(glBindAttribLocation), D(glBindBuffer),
    F(glBindFramebuffer, g_glBindFramebufferOES), F(glBindFramebufferOES, g_glBindFramebufferOES),
    F(glBindRenderbuffer, g_glBindRenderbufferOES), F(glBindRenderbufferOES, g_glBindRenderbufferOES),
    D(glBindTexture), D(glBlendFunc),
    D(glBufferData), D(glBufferSubData),
    F(glClear, g_glClear), D(glClearColor),
    D(glColorPointer), F(glColor4x, g_glColor4x),
    D(glCompileShader), D(glCompressedTexImage2D),
    D(glCopyTexImage2D), D(glCopyTexSubImage2D),
    D(glCreateProgram), D(glCreateShader),
    D(glCullFace),
    D(glDeleteBuffers), F(glDeleteFramebuffersOES, g_glDeleteFramebuffersOES),
    D(glDeleteProgram), F(glDeleteRenderbuffersOES, glDeleteRenderbuffers),
    D(glDeleteShader), D(glDeleteTextures),
    D(glDepthFunc), D(glDepthMask),
    D(glDisable), D(glDisableClientState), D(glDisableVertexAttribArray),
    F(glDrawArrays, g_glDrawArrays), D(glDrawElements),
    D(glEnable), D(glEnableClientState), D(glEnableVertexAttribArray),
    D(glFinish), D(glFogf), D(glFogfv),
    F(glFramebufferRenderbuffer, g_glFramebufferRenderbufferOES), F(glFramebufferRenderbufferOES, g_glFramebufferRenderbufferOES),
    D(glFramebufferTexture2D), D(glFrontFace),
    D(glGenBuffers), F(glGenFramebuffers, glGenFramebuffers), F(glGenFramebuffersOES, glGenFramebuffers),
    F(glGenRenderbuffers, glGenRenderbuffers), F(glGenRenderbuffersOES, glGenRenderbuffers),
    D(glGenTextures),
    D(glGetAttribLocation), D(glGetError), D(glGetIntegerv),
    D(glGetProgramInfoLog), D(glGetProgramiv),
    D(glGetShaderInfoLog), D(glGetShaderiv),
    D(glGetUniformLocation), D(glHint),
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
    D(glTexEnvf), D(glTexEnvfv), F(glTexEnvi, g_glTexEnvi), D(glTexEnvx), F(glTexEnvxv, g_glTexEnvxv),
    D(glTexImage2D), D(glTexParameteri), F(glTexParameterx, g_glTexParameterx), D(glTexSubImage2D),
    D(glTranslatef), D(glTranslatex),
    D(glUniform1f), D(glUniform1i), D(glUniform2fv), D(glUniform3fv), D(glUniform4fv), D(glUniformMatrix4fv),
    D(glUseProgram), F(glValidateProgram, g_glValidateProgram),
    F(glVertexPointer, g_glVertexPointer), D(glVertexAttribPointer),
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
