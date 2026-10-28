/**
 * @file objc.c
 * @brief Lightweight Objective-C and Foundation/UIKit runtime emulation for Hero of Sparta II.
 */
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "darwin_printf.h"
#include "gles.h"
#include "hle.h"
#include "imports.h"
#include "log.h"
#include "macho.h"
#include "objc.h"

// ---------------------------------------------------------------- NSString & CFString

typedef struct {
    void *isa;
    uint32_t flags;
    char *str;
    uint32_t len;
} ns_string;

static ns_string *ns_new(const char *s) {
    static ns_string ring[512];
    static int next = 0;
    ns_string *o = &ring[next++ & 511];
    free(o->str);
    o->str = strdup(s ? s : "");
    o->len = strlen(o->str);
    o->flags = 0x7C8;
    return o;
}

static const char *ns_cstr(const void *o) {
    if (!o) return "";
    uintptr_t val = (uintptr_t)o;
    if (val < 0x10000) return "";
    // If val is a pointer to a pointer to cf_str:
    if ((val & 3) == 0) {
        uintptr_t deref = *(uintptr_t *)val;
        if (deref >= 0x10000 && (deref & 3) == 0) {
            uint32_t f = *(uint32_t *)(deref + 4);
            if ((f & 0xFF) == 0xC8 || f == 0x7C8) {
                const char *cs = *(const char **)(deref + 8);
                return cs ? cs : "";
            }
        }
    }
    // Check if it is a compile-time CFConstantString or dynamic ns_string (cstr at offset +8)
    const ns_string *ns = (const ns_string *)o;
    return ns->str ? ns->str : "";
}

static uint32_t utf8_next(const unsigned char **p) {
    const unsigned char *s = *p;
    uint32_t c = *s++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c &= extra ? 0x3F >> extra : 0xFF;
    while (extra-- && (*s & 0xC0) == 0x80)
        c = (c << 6) | (*s++ & 0x3F);
    *p = s;
    return c;
}

static int utf16_len(const char *s) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;)
        n += utf8_next(&p) > 0xFFFF ? 2 : 1;
    return n;
}

static ns_string *ns_format(const char *fmt, va_list ap) {
    char out[1024];
    va_list cp;
    va_copy(cp, ap);
    words w = {&cp, NULL};
    darwin_vformat(out, sizeof(out), fmt, &w);
    va_end(cp);
    return ns_new(out);
}

// ---------------------------------------------------------------- Typography (FreeType)

typedef struct {
    uint32_t key;
    int16_t left, top, adv;
    uint16_t w, h;
    uint8_t *bmp;
} glyph;

#define GLYPH_CACHE 4096
static glyph cache[GLYPH_CACHE];
static FT_Library ft;
static FT_Face faces[4];
static int nfaces = -1;

static void fonts_init(void) {
    static const char *paths[] = {
        DATA_PATH "font.ttf",
        "sa0:data/font/pvf/ltn0.pvf",
        "sa0:data/font/pvf/kr0.pvf",
        "sa0:data/font/pvf/jpn0.pvf"
    };
    nfaces = 0;
    if (FT_Init_FreeType(&ft))
        return l_error("FreeType failed to initialize");
    for (int i = 0; i < 4; i++) {
        if (!FT_New_Face(ft, paths[i], 0, &faces[nfaces])) {
            l_info("Loaded font: %s", paths[i]);
            nfaces++;
        }
    }
    if (!nfaces)
        l_warn("No system TTF fonts loaded: text metrics will use approximations");
}

static const glyph *get_glyph(int size, uint32_t cp) {
    static glyph tmp;
    uint32_t key = (uint32_t)size << 24 | (cp & 0xFFFFFF);
    uint32_t h = (key * 2654435761u) % GLYPH_CACHE;
    glyph *g = NULL;
    for (int i = 0; i < 16; i++) {
        glyph *c = &cache[(h + i) % GLYPH_CACHE];
        if (c->key == key) return c;
        if (!c->key) { g = c; break; }
    }
    if (!g) {
        free(tmp.bmp);
        g = &tmp;
    }
    memset(g, 0, sizeof(*g));
    g->key = key;
    if (nfaces < 0) fonts_init();
    FT_Face f = NULL;
    for (int i = 0; i < nfaces && !f; i++) {
        if (FT_Get_Char_Index(faces[i], cp))
            f = faces[i];
    }
    if (!f) {
        g->adv = cp == ' ' ? size / 3 : size / 2;
        return g;
    }
    FT_Set_Pixel_Sizes(f, 0, size);
    if (FT_Load_Char(f, cp, FT_LOAD_RENDER))
        return g;
    FT_GlyphSlot s = f->glyph;
    g->left = s->bitmap_left;
    g->top = s->bitmap_top;
    g->adv = s->advance.x >> 6;
    g->w = s->bitmap.width;
    g->h = s->bitmap.rows;
    g->bmp = malloc(g->w * g->h + 1);
    for (int y = 0; y < g->h; y++)
        memcpy(g->bmp + y * g->w, s->bitmap.buffer + y * s->bitmap.pitch, g->w);
    return g;
}

static int line_height(int size) { return size == 14 ? 18 : size == 13 ? 16 : size == 11 ? 14 : (size * 5 + 2) / 4; }
static int baseline(int size) { return (line_height(size) - size) / 2 + (size * 4 + 2) / 5; }

static int text_width(int size, const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;)
        w += get_glyph(size, utf8_next(&p))->adv;
    return w;
}

// ---------------------------------------------------------------- CoreGraphics

typedef struct {
    uint8_t *data;
    int w, h, bpr;
    float gray;
} cg_ctx;

static cg_ctx *cur_ctx;

static cg_ctx *cg_BitmapContextCreate(void *data, int w, int h, int bpc, int bpr, void *cs, int info) {
    cg_ctx *c = calloc(1, sizeof(*c));
    *c = (cg_ctx){data, w, h, bpr, 1.0f};
    return c;
}

static void cg_ContextRelease(cg_ctx *c) {
    if (cur_ctx == c) cur_ctx = NULL;
    free(c);
}

SOFTFP static void cg_SetGrayFillColor(cg_ctx *c, float gray, float alpha) {
    if (c) c->gray = gray * alpha;
}

static void *cg_ColorSpaceCreate(void) { return (void *)1; }
static void cg_noop(void) {}
static void ui_PushContext(cg_ctx *c) { cur_ctx = c; }
static void ui_PopContext(void) { cur_ctx = NULL; }

typedef struct {
    uint32_t tag;
    int size;
} ui_font;

static ui_font *font_of_size(float pt) {
    static ui_font fonts[32];
    int size = (int)pt;
    for (int i = 0; i < 32; i++) {
        if (!fonts[i].tag || fonts[i].size == size) {
            fonts[i] = (ui_font){0x464F4E54, size};
            return &fonts[i];
        }
    }
    return &fonts[31];
}

static void draw_text(cg_ctx *c, const char *s, int x, int y, int size) {
    int pen = x, base = baseline(size);
    uint8_t ink = (uint8_t)(c->gray * 255.0f);
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        const glyph *g = get_glyph(size, utf8_next(&p));
        for (int gy = 0; gy < g->h; gy++) {
            int row = c->h - 1 - (y + base - g->top + gy);
            if (row < 0 || row >= c->h) continue;
            for (int gx = 0; gx < g->w; gx++) {
                int col = pen + g->left + gx;
                if (col < 0 || col >= c->w) continue;
                uint8_t v = g->bmp[gy * g->w + gx] * ink / 255;
                uint8_t *d = &c->data[row * c->bpr + col];
                if (v > *d) *d = v;
            }
        }
        pen += g->adv;
    }
}

// ---------------------------------------------------------------- CoreGraphics Functions

typedef struct { float a, b, c, d, tx, ty; } CGAffineTransform;
typedef struct { float x, y; } CGPoint;
typedef struct { float width, height; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;

SOFTFP static CGAffineTransform d_CGAffineTransformMakeRotation(float angle) {
    float ca = cosf(angle), sa = sinf(angle);
    return (CGAffineTransform){ca, sa, -sa, ca, 0.0f, 0.0f};
}

SOFTFP static CGAffineTransform d_CGAffineTransformRotate(CGAffineTransform t, float angle) {
    float ca = cosf(angle), sa = sinf(angle);
    return (CGAffineTransform){
        t.a * ca + t.c * sa,
        t.b * ca + t.d * sa,
        t.a * -sa + t.c * ca,
        t.b * -sa + t.d * ca,
        t.tx, t.ty
    };
}

static void d_CGContextDrawImage(void *ctx, CGRect rect, void *img) { (void)ctx; (void)rect; (void)img; }
static void d_CGContextFillRect(void *ctx, CGRect rect) { (void)ctx; (void)rect; }
SOFTFP static void d_CGContextScaleCTM(void *ctx, float sx, float sy) { (void)ctx; (void)sx; (void)sy; }
SOFTFP static void d_CGContextSetRGBFillColor(void *ctx, float r, float g, float b, float a) { (void)ctx; (void)r; (void)g; (void)b; (void)a; }
SOFTFP static void d_CGContextTranslateCTM(void *ctx, float tx, float ty) { (void)ctx; (void)tx; (void)ty; }
static void *d_CGImageGetColorSpace(void *img) { (void)img; return (void *)1; }
static size_t d_CGImageGetHeight(void *img) { (void)img; return 480; }
static size_t d_CGImageGetWidth(void *img) { (void)img; return 320; }

// ---------------------------------------------------------------- CoreFoundation

static void *d_CFAllocatorGetDefault(void) { return (void *)1; }
static void *d_CFBundleGetMainBundle(void) { return (void *)2; }
static void *d_CFBundleCopyBundleURL(void *bundle) { (void)bundle; return (void *)3; }
static void *d_CFBundleCopyResourcesDirectoryURL(void *bundle) { (void)bundle; return (void *)3; }

static void *d_CFBundleGetValueForInfoDictionaryKey(void *bundle, const void *key) {
    (void)bundle; (void)key;
    return ns_new("1.0.2");
}

static void d_CFRelease(void *cf) { (void)cf; }

static int d_CFRunLoopRunInMode(const void *mode, double seconds, int returnAfterSourceHandled) {
    (void)mode; (void)seconds; (void)returnAfterSourceHandled;
    sceKernelDelayThread(1000);
    return 0;
}

static int d_CFStringGetCString(const void *str, char *buf, int maxlen, uint32_t enc) {
    (void)enc;
    const char *s = ns_cstr(str);
    if (buf && maxlen > 0) {
        snprintf(buf, maxlen, "%s", s);
        return 1;
    }
    return 0;
}

static void *d_CFURLCopyFileSystemPath(void *url, int style) {
    (void)url; (void)style;
    return ns_new(BUNDLE_PATH);
}

static int d_CFURLGetFileSystemRepresentation(void *url, int resolve, char *buf, int maxlen) {
    (void)url; (void)resolve;
    if (buf && maxlen > 0) {
        snprintf(buf, maxlen, "%s", BUNDLE_PATH);
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '/') buf[len - 1] = '\0';
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- IME Dialog

static int ime_type = -1;
static char ime_utf8[128];
static uint16_t ime_buf[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1], ime_title[16], ime_initial[1];

static void ime_open(int type) {
    static const char *titles[] = {"Name", "Number"};
    const char *t = titles[type == 1];
    for (int i = 0; (ime_title[i] = t[i]); i++) {}
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.type = type == 1 ? SCE_IME_TYPE_NUMBER : SCE_IME_TYPE_DEFAULT;
    p.title = ime_title;
    p.maxTextLength = type == 1 ? 4 : 12;
    p.initialText = ime_initial;
    p.inputTextBuffer = ime_buf;
    ime_utf8[0] = 0;
    int r = sceImeDialogInit(&p);
    if (r < 0) l_error("sceImeDialogInit = 0x%08X", r);
    ime_type = type;
}

int ime_active(void) { return ime_type >= 0; }

void ime_poll(void) {
    if (ime_type < 0) return;
    SceCommonDialogStatus st = sceImeDialogGetStatus();
    if (st == SCE_COMMON_DIALOG_STATUS_RUNNING) return;
    if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
        SceImeDialogResult res;
        memset(&res, 0, sizeof(res));
        sceImeDialogGetResult(&res);
        if (res.button == SCE_IME_DIALOG_BUTTON_ENTER) {
            size_t n = 0;
            for (int i = 0; ime_buf[i] && n < sizeof(ime_utf8) - 4; i++) {
                uint16_t c = ime_buf[i];
                if (c < 0x80) ime_utf8[n++] = c;
                else if (c < 0x800) n += sprintf(ime_utf8 + n, "%c%c", 0xC0 | c >> 6, 0x80 | (c & 0x3F));
                else n += sprintf(ime_utf8 + n, "%c%c%c", 0xE0 | c >> 12, 0x80 | ((c >> 6) & 0x3F), 0x80 | (c & 0x3F));
            }
            ime_utf8[n] = 0;
        }
        sceImeDialogTerm();
    }
    int type = ime_type;
    ime_type = -1;
    l_info("Text input entered (%d): \"%s\"", type, ime_utf8);
    uintptr_t text_close_fn = macho_sym("_Ex_HandleEventTextClose");
    if (text_close_fn) call_guest((void *)text_close_fn, type, 0, 0, 0);
}

// ---------------------------------------------------------------- Guest Classes Registry

typedef struct guest_method {
    const char *name;
    uintptr_t imp;
} guest_method;

typedef struct guest_class_info {
    const char *name;
    uint32_t cls_ptr;
    uint32_t instance_size;
    guest_method *methods;
    int method_count;
} guest_class_info;

static guest_class_info registered_classes[32];
static int registered_class_count = 0;

void *objc_find_class(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < registered_class_count; i++) {
        if (!strcmp(registered_classes[i].name, name))
            return (void *)registered_classes[i].cls_ptr;
    }
    return NULL;
}

uintptr_t objc_get_imp(void *cls, const char *sel_name) {
    if (!cls || !sel_name) return 0;
    for (int i = 0; i < registered_class_count; i++) {
        if (registered_classes[i].cls_ptr == (uint32_t)cls) {
            for (int m = 0; m < registered_classes[i].method_count; m++) {
                if (!strcmp(registered_classes[i].methods[m].name, sel_name))
                    return registered_classes[i].methods[m].imp;
            }
            break;
        }
    }
    return 0;
}

int objc_has_method(void *cls, const char *sel_name) {
    return objc_get_imp(cls, sel_name) != 0;
}

void *objc_class_alloc(void *cls) {
    if (!cls) return NULL;
    uint32_t size = 128;
    for (int i = 0; i < registered_class_count; i++) {
        if (registered_classes[i].cls_ptr == (uint32_t)cls) {
            if (registered_classes[i].instance_size > 0)
                size = registered_classes[i].instance_size;
            break;
        }
    }
    void **obj = calloc(1, size + 16);
    *obj = cls; // obj->isa = cls
    return obj;
}

static inline uintptr_t to_guest_mem(uint32_t va) {
    if (!va) return 0;
    return (va < MACHO_SLIDE) ? (MACHO_SLIDE + va) : va;
}

void objc_init(void) {
    l_info("[OBJC] Registering guest Mach-O classes from __objc_classlist...");
    // In HOS2, __objc_classlist is at offset 0x349f34
    uint32_t *cl_ptrs = (uint32_t *)(MACHO_SLIDE + 0x349f34);
    int nclasses = 12; // 48 bytes / 4
    for (int i = 0; i < nclasses; i++) {
        uint32_t cls_va = cl_ptrs[i];
        if (!cls_va) continue;
        uint32_t cls_mem = to_guest_mem(cls_va);
        uint32_t ro_va = *(uint32_t *)(cls_mem + 16);
        uint32_t ro_mem = to_guest_mem(ro_va);
        uint32_t name_va = *(uint32_t *)(ro_mem + 16);
        const char *cname = (const char *)to_guest_mem(name_va);
        uint32_t isize = *(uint32_t *)(ro_mem + 8);
        uint32_t methods_va = *(uint32_t *)(ro_mem + 20);

        guest_class_info *ci = &registered_classes[registered_class_count++];
        ci->name = cname;
        ci->cls_ptr = cls_mem;
        ci->instance_size = isize;
        ci->methods = NULL;
        ci->method_count = 0;

        if (methods_va) {
            uint32_t methods_mem = to_guest_mem(methods_va);
            uint32_t mcount = *(uint32_t *)(methods_mem + 4);
            ci->method_count = mcount;
            ci->methods = calloc(mcount, sizeof(guest_method));
            for (uint32_t m = 0; m < mcount; m++) {
                uint32_t m_entry = methods_mem + 8 + m * 12;
                uint32_t m_name_va = *(uint32_t *)m_entry;
                uint32_t m_imp_va = *(uint32_t *)(m_entry + 8);
                ci->methods[m].name = (const char *)to_guest_mem(m_name_va);
                ci->methods[m].imp = to_guest_mem(m_imp_va);
            }
        }
        l_info("[OBJC] Registered guest class %s (size %u, %d methods)", cname, (unsigned)isize, ci->method_count);
    }
}

// ---------------------------------------------------------------- System Stub Classes

typedef struct objc_class_stub {
    struct objc_class_stub *isa;
    struct objc_class_stub *superclass;
    void *cache;
    void *vtable;
    uint32_t info;
    const char *name;
} objc_class_stub;

#define DEF_OBJC_CLASS(clsname) \
    static objc_class_stub OBJC_METACLASS_$_##clsname = { &OBJC_METACLASS_$_##clsname, NULL, NULL, NULL, 0, #clsname }; \
    static objc_class_stub OBJC_CLASS_$_##clsname = { &OBJC_METACLASS_$_##clsname, NULL, NULL, NULL, 0, #clsname }

DEF_OBJC_CLASS(CAEAGLLayer);
DEF_OBJC_CLASS(EAGLContext);
DEF_OBJC_CLASS(MPMediaItemCollection);
DEF_OBJC_CLASS(MPMediaLibrary);
DEF_OBJC_CLASS(MPMediaQuery);
DEF_OBJC_CLASS(MPMoviePlayerController);
DEF_OBJC_CLASS(MPMoviePlayerViewController);
DEF_OBJC_CLASS(MPMusicPlayerController);
DEF_OBJC_CLASS(NSArray);
DEF_OBJC_CLASS(NSAutoreleasePool);
DEF_OBJC_CLASS(NSBundle);
DEF_OBJC_CLASS(NSData);
DEF_OBJC_CLASS(NSDate);
DEF_OBJC_CLASS(NSDateFormatter);
DEF_OBJC_CLASS(NSDictionary);
DEF_OBJC_CLASS(NSError);
DEF_OBJC_CLASS(NSFileManager);
DEF_OBJC_CLASS(NSLocale);
DEF_OBJC_CLASS(NSMutableData);
DEF_OBJC_CLASS(NSMutableURLRequest);
DEF_OBJC_CLASS(NSNotificationCenter);
DEF_OBJC_CLASS(NSNumber);
DEF_OBJC_CLASS(NSObject);
DEF_OBJC_CLASS(NSString);
DEF_OBJC_CLASS(NSThread);
DEF_OBJC_CLASS(NSTimer);
DEF_OBJC_CLASS(NSURL);
DEF_OBJC_CLASS(NSURLConnection);
DEF_OBJC_CLASS(NSURLRequest);
DEF_OBJC_CLASS(NSUserDefaults);
DEF_OBJC_CLASS(UIAccelerometer);
DEF_OBJC_CLASS(UIActivityIndicatorView);
DEF_OBJC_CLASS(UIAlertView);
DEF_OBJC_CLASS(UIApplication);
DEF_OBJC_CLASS(UIButton);
DEF_OBJC_CLASS(UIColor);
DEF_OBJC_CLASS(UIDevice);
DEF_OBJC_CLASS(UIFont);
DEF_OBJC_CLASS(UIImage);
DEF_OBJC_CLASS(UIImagePickerController);
DEF_OBJC_CLASS(UIImageView);
DEF_OBJC_CLASS(UILabel);
DEF_OBJC_CLASS(UINavigationController);
DEF_OBJC_CLASS(UIScreen);
DEF_OBJC_CLASS(UITextField);
DEF_OBJC_CLASS(UITextView);
DEF_OBJC_CLASS(UIView);
DEF_OBJC_CLASS(UIViewController);
DEF_OBJC_CLASS(UIWebView);
DEF_OBJC_CLASS(UIWindow);

static void *d_CFConstantStringClassReference[8];
static void *d_OBJC_EHTYPE_id[8];
static void *d_objc_empty_cache[8];
static void *d_objc_empty_vtable[8];

typedef struct { void *isa; uint32_t flags; const char *str; uint32_t len; } cf_str_objc;
#define DEF_CFSTR_OBJC(name, str_val) \
    static const cf_str_objc cf_##name = { NULL, 0x7C8, str_val, sizeof(str_val) - 1 }; \
    static const void *ptr_##name = &cf_##name

DEF_CFSTR_OBJC(kCFBundleVersionKey, "CFBundleVersion");
DEF_CFSTR_OBJC(kCFRunLoopDefaultMode, "kCFRunLoopDefaultMode");
DEF_CFSTR_OBJC(NSErrorFailingURLStringKey, "NSErrorFailingURLStringKey");
DEF_CFSTR_OBJC(NSLocaleCountryCode, "en_US");
DEF_CFSTR_OBJC(UIKeyboardWillHideNotification, "UIKeyboardWillHideNotification");
DEF_CFSTR_OBJC(UIKeyboardWillShowNotification, "UIKeyboardWillShowNotification");

// ---------------------------------------------------------------- ObjC Runtime Functions

static int d___objc_personality_v0(void) { return 0; }
static void *d_objc_begin_catch(void *e) { return e; }
static void d_objc_end_catch(void) {}
static void d_objc_enumerationMutation(void *col) { (void)col; }

static uint32_t dummy_obj[16];
static uint32_t ret_obj(void) { return (uint32_t)dummy_obj; }

static void log_unknown(void *self, const char *sel) {
    static const char *seen[512];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == sel) return;
    if (nseen < 512) seen[nseen++] = sel;
    l_warn("[OBJC] unhandled -[%p %s]", self, sel);
}

static int sel_is(const char *sel, const char *name) { return !strcmp(sel, name); }

static uint32_t objc_msgSend_system_impl(void *self, const char *sel, va_list ap) {
    if (!self || !sel) return 0;

    va_list ap_copy;
    va_copy(ap_copy, ap);
    uint32_t a = va_arg(ap_copy, uint32_t);
    uint32_t r = 0;
    const struct tm *now;
    time_t t;

    // Allocation & Instantiation
    if (sel_is(sel, "alloc") || sel_is(sel, "allocWithZone:") || sel_is(sel, "new")) {
        r = (uint32_t)objc_class_alloc(self);
        if (!r) r = ret_obj();
    }
    // Initializers: MUST return self!
    else if (sel_is(sel, "initWithAPI:")) {
        uint32_t api = va_arg(ap, uint32_t);
        l_info("EAGLContext initWithAPI:%u", (unsigned)api);
        r = (api == 1) ? (uint32_t)self : 0;
    } else if (sel_is(sel, "init") ||
             sel_is(sel, "initWithFrame:") ||
             sel_is(sel, "initWithFrame:pixelFormat:") ||
             sel_is(sel, "initWithFrame:pixelFormat:depthFormat:preserveBackbuffer:") ||
             sel_is(sel, "initWithActivityIndicatorStyle:") ||
             sel_is(sel, "initWithArray:") ||
             sel_is(sel, "initWithBytes:length:") ||
             sel_is(sel, "initWithContentURL:") ||
             sel_is(sel, "initWithContentsOfFile:") ||
             sel_is(sel, "initWithContentsOfURL:") ||
             sel_is(sel, "initWithData:encoding:") ||
             sel_is(sel, "initWithFile:andFlipped:") ||
             sel_is(sel, "initWithFormat:") ||
             sel_is(sel, "initWithRequest:delegate:") ||
             sel_is(sel, "initWithString:") ||
             sel_is(sel, "initWithTitle:message:delegate:cancelButtonTitle:otherButtonTitles:") ||
             sel_is(sel, "initWithUTF8String:")) {
        r = (uint32_t)self;
    }
    // Memory management & class introspection
    else if (sel_is(sel, "retain") || sel_is(sel, "autorelease") || sel_is(sel, "class")) {
        r = (uint32_t)self;
    } else if (sel_is(sel, "release") || sel_is(sel, "dealloc")) {
        r = 0;
    }
    // Singletons / Shared Instances
    else if (sel_is(sel, "sharedInstance") || sel_is(sel, "sharedApplication") ||
             sel_is(sel, "sharedAccelerometer") || sel_is(sel, "standardUserDefaults") ||
             sel_is(sel, "defaultCenter") || sel_is(sel, "defaultManager") ||
             sel_is(sel, "defaultMediaLibrary") || sel_is(sel, "currentDevice") ||
             sel_is(sel, "currentLocale") || sel_is(sel, "mainScreen") ||
             sel_is(sel, "mainBundle") || sel_is(sel, "currentCalendar") ||
             sel_is(sel, "currentContext") || sel_is(sel, "date") ||
             sel_is(sel, "layer") || sel_is(sel, "context") ||
             sel_is(sel, "iPodMusicPlayer") || sel_is(sel, "keyWindow") ||
             sel_is(sel, "subviews")) {
        r = ret_obj();
    }
    // Colors
    else if (sel_is(sel, "blackColor") || sel_is(sel, "whiteColor") ||
             sel_is(sel, "clearColor") || sel_is(sel, "grayColor") ||
             sel_is(sel, "orangeColor") || sel_is(sel, "colorWithRed:green:blue:alpha:")) {
        r = ret_obj();
    }
    // Layer class
    else if (sel_is(sel, "layerClass")) {
        r = (uint32_t)&OBJC_CLASS_$_CAEAGLLayer;
    }
    // String creation & formatting
    else if (sel_is(sel, "stringWithUTF8String:") || sel_is(sel, "stringWithCString:encoding:")) {
        r = (uint32_t)ns_new((const char *)a);
    } else if (sel_is(sel, "stringWithFormat:")) {
        r = (uint32_t)ns_format(ns_cstr((void *)a), ap_copy);
    } else if (sel_is(sel, "stringByAppendingString:") || sel_is(sel, "stringByAppendingPathComponent:") ||
               sel_is(sel, "stringByAppendingFormat:")) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s/%s", ns_cstr(self), ns_cstr((void *)a));
        r = (uint32_t)ns_new(buf);
    } else if (sel_is(sel, "stringWithContentsOfFile:") || sel_is(sel, "stringWithContentsOfFile:encoding:error:")) {
        r = (uint32_t)ns_new("");
    } else if (sel_is(sel, "length")) {
        r = utf16_len(ns_cstr(self));
    } else if (sel_is(sel, "UTF8String") || sel_is(sel, "cStringUsingEncoding:")) {
        r = (uint32_t)ns_cstr(self);
    } else if (sel_is(sel, "getCString:maxLength:encoding:")) {
        uint32_t max = va_arg(ap_copy, uint32_t);
        snprintf((char *)a, max, "%s", ns_cstr(self));
        r = 1;
    } else if (sel_is(sel, "isEqualToString:")) {
        r = !strcmp(ns_cstr(self), ns_cstr((void *)a));
    } else if (sel_is(sel, "hasPrefix:")) {
        const char *s = ns_cstr(self), *prefix = ns_cstr((void *)a);
        r = strncmp(s, prefix, strlen(prefix)) == 0;
    } else if (sel_is(sel, "hasSuffix:")) {
        const char *s = ns_cstr(self), *suffix = ns_cstr((void *)a);
        size_t slen = strlen(s), suflen = strlen(suffix);
        r = (slen >= suflen && !strcmp(s + slen - suflen, suffix));
    }
    // Fonts
    else if (sel_is(sel, "systemFontOfSize:") || sel_is(sel, "boldSystemFontOfSize:")) {
        union { uint32_t u; float f; } v = {a};
        r = (uint32_t)font_of_size(v.f);
    }
    // Bundle resource path & info dictionary
    else if (sel_is(sel, "resourcePath")) {
        char p[256];
        snprintf(p, sizeof(p), "%s", BUNDLE_PATH);
        size_t len = strlen(p);
        if (len > 0 && p[len - 1] == '/') p[len - 1] = 0;
        r = (uint32_t)ns_new(p);
    } else if (sel_is(sel, "objectForInfoDictionaryKey:")) {
        const char *k = ns_cstr((void *)a);
        r = !strcmp(k, "CFBundleVersion") || !strcmp(k, "CFBundleShortVersionString") ? (uint32_t)ns_new("1.0.2") : 0;
    } else if (sel_is(sel, "year") || sel_is(sel, "month") || sel_is(sel, "day")) {
        t = time(NULL);
        now = localtime(&t);
        r = sel[0] == 'y' ? now->tm_year + 1900 : sel[0] == 'm' ? now->tm_mon + 1 : now->tm_mday;
    }
    // GLES / EAGL
    else if (sel_is(sel, "renderbufferStorage:fromDrawable:") ||
             sel_is(sel, "renderbufferStorage:fromDrawable")) {
        gles_bind_drawable();
        r = 1;
    } else if (sel_is(sel, "presentRenderbuffer:")) {
        gles_present();
        r = 1;
    } else if (sel_is(sel, "setCurrentContext:") || sel_is(sel, "setCurrentContext")) {
        r = 1;
    } else if (sel_is(sel, "isCurrentContext")) {
        r = 1;
    }
    // Device properties
    else if (sel_is(sel, "systemVersion")) {
        r = (uint32_t)ns_new("3.2");
    } else if (sel_is(sel, "model")) {
        r = (uint32_t)ns_new("iPhone");
    } else if (sel_is(sel, "uniqueIdentifier")) {
        r = (uint32_t)ns_new("0000000000000000000000000000000000000000");
    } else if (sel_is(sel, "userInterfaceIdiom")) {
        r = 0; // UIUserInterfaceIdiomPhone
    } else if (sel_is(sel, "orientation") || sel_is(sel, "statusBarOrientation")) {
        r = 1; // UIInterfaceOrientationPortrait
    } else if (sel_is(sel, "contentScaleFactor") || sel_is(sel, "scale") || sel_is(sel, "alpha")) {
        union { float f; uint32_t u; } flt = { 1.0f };
        r = flt.u;
    }
    // Data, arrays, dictionaries
    else if (sel_is(sel, "count") || sel_is(sel, "intValue") || sel_is(sel, "isMultitaskingSupported")) {
        r = 0;
    } else if (sel_is(sel, "data") || sel_is(sel, "bytes")) {
        r = (uint32_t)dummy_obj;
    } else if (sel_is(sel, "dictionaryWithObjectsAndKeys:") ||
               sel_is(sel, "componentsSeparatedByString:") ||
               sel_is(sel, "numberWithBool:") ||
               sel_is(sel, "imageWithContentsOfFile:") ||
               sel_is(sel, "imageWithData:") ||
               sel_is(sel, "fileURLWithPath:") ||
               sel_is(sel, "URLWithString:") ||
               sel_is(sel, "requestWithURL:") ||
               sel_is(sel, "requestWithURL:cachePolicy:timeoutInterval:")) {
        r = ret_obj();
    } else if (sel_is(sel, "absoluteString")) {
        r = (uint32_t)ns_new("");
    } else if (sel_is(sel, "writeToFile:atomically:") ||
               sel_is(sel, "writeToFile:atomically:encoding:error:") ||
               sel_is(sel, "copyItemAtPath:toPath:error:") ||
               sel_is(sel, "createDirectoryAtPath:attributes:") ||
               sel_is(sel, "removeItemAtPath:error:") ||
               sel_is(sel, "fileExistsAtPath:")) {
        r = 1;
    }
    // IME
    else if (sel_is(sel, "textInputOpen:")) {
        ime_open(a);
    } else if (sel_is(sel, "textInputGetUTF8")) {
        r = (uint32_t)ime_utf8;
    } else if (sel_is(sel, "textInputGetLength")) {
        r = utf16_len(ime_utf8);
    }
    // Introspection
    else if (sel_is(sel, "respondsToSelector:")) {
        r = 0;
    } else if (sel_is(sel, "isMemberOfClass:")) {
        r = 1;
    }
    // No-op setters / helpers that return 0
    else if (sel_is(sel, "setIdleTimerDisabled:") ||
             sel_is(sel, "setStatusBarHidden:") ||
             sel_is(sel, "setStatusBarHidden:animated:") ||
             sel_is(sel, "setStatusBarOrientation:") ||
             sel_is(sel, "setStatusBarOrientation:animated:") ||
             sel_is(sel, "setStatusBar:") ||
             sel_is(sel, "setBackgroundColor:") ||
             sel_is(sel, "setMultipleTouchEnabled:") ||
             sel_is(sel, "setExclusiveTouch:") ||
             sel_is(sel, "addSubview:") ||
             sel_is(sel, "makeKeyAndVisible") ||
             sel_is(sel, "removeFromSuperview") ||
             sel_is(sel, "removeAllWidgetsFromSuperview") ||
             sel_is(sel, "beginGeneratingDeviceOrientationNotifications") ||
             sel_is(sel, "endGeneratingDeviceOrientationNotifications") ||
             sel_is(sel, "beginGeneratingLibraryChangeNotifications") ||
             sel_is(sel, "beginGeneratingPlaybackNotifications") ||
             sel_is(sel, "endGeneratingPlaybackNotifications") ||
             sel_is(sel, "addObserver:selector:name:object:") ||
             sel_is(sel, "removeObserver:name:object:") ||
             sel_is(sel, "removeObserver:") ||
             sel_is(sel, "setDrawableProperties:") ||
             sel_is(sel, "setOpaque:") ||
             sel_is(sel, "setFrame:") ||
             sel_is(sel, "setBounds:") ||
             sel_is(sel, "setCenter:") ||
             sel_is(sel, "setTransform:") ||
             sel_is(sel, "setUserInteractionEnabled:") ||
             sel_is(sel, "setClearsContextBeforeDrawing:") ||
             sel_is(sel, "setContentScaleFactor:") ||
             sel_is(sel, "setNeedsDisplay") ||
             sel_is(sel, "setNeedsLayout") ||
             sel_is(sel, "setHidden:") ||
             sel_is(sel, "setAlpha:") ||
             sel_is(sel, "setEnabled:") ||
             sel_is(sel, "setTag:") ||
             sel_is(sel, "setText:") ||
             sel_is(sel, "setTextColor:") ||
             sel_is(sel, "setFont:") ||
             sel_is(sel, "setTextAlignment:") ||
             sel_is(sel, "setNumberOfLines:") ||
             sel_is(sel, "setLineBreakMode:") ||
             sel_is(sel, "setAdjustsFontSizeToFitWidth:") ||
             sel_is(sel, "setBorderStyle:") ||
             sel_is(sel, "setKeyboardType:") ||
             sel_is(sel, "setSecureTextEntry:") ||
             sel_is(sel, "setContentMode:") ||
             sel_is(sel, "setScalingMode:") ||
             sel_is(sel, "setFullscreen:") ||
             sel_is(sel, "setControlStyle:") ||
             sel_is(sel, "setMovieControlMode:") ||
             sel_is(sel, "setUseApplicationAudioSession:") ||
             sel_is(sel, "setUpdateInterval:") ||
             sel_is(sel, "setDelegate:") ||
             sel_is(sel, "setImage:") ||
             sel_is(sel, "setTitle:forState:") ||
             sel_is(sel, "setBackgroundImage:forState:") ||
             sel_is(sel, "addTarget:action:forControlEvents:") ||
             sel_is(sel, "sendActionsForControlEvents:") ||
             sel_is(sel, "show") ||
             sel_is(sel, "startAnimating") ||
             sel_is(sel, "stopAnimating") ||
             sel_is(sel, "becomeFirstResponder") ||
             sel_is(sel, "resignFirstResponder") ||
             sel_is(sel, "openURL:") ||
             sel_is(sel, "loadHTMLString:baseURL:") ||
             sel_is(sel, "loadRequest:") ||
             sel_is(sel, "stop") ||
             sel_is(sel, "pause") ||
             sel_is(sel, "play") ||
             sel_is(sel, "invalidate") ||
             sel_is(sel, "detachNewThreadSelector:toTarget:withObject:") ||
             sel_is(sel, "performSelectorOnMainThread:withObject:waitUntilDone:")) {
        r = 0;
    } else {
        log_unknown(self, sel);
    }

    va_end(ap_copy);
    return r;
}

typedef struct { void *receiver; void *super_class; } objc_super;

static uint32_t d_objc_msgSendSuper2(objc_super *super, const char *sel, ...) {
    if (!super || !super->receiver || !sel) return 0;
    va_list ap;
    va_start(ap, sel);
    // In HOS2 all guest classes have external superclasses (UIView, UIViewController, NSObject),
    // so super calls must dispatch directly to system handling to avoid infinite recursion into self.
    uint32_t r = objc_msgSend_system_impl(super->receiver, sel, ap);
    va_end(ap);
    return r;
}

static void d_objc_setProperty(void *self, const char *sel, ptrdiff_t offset, void *val, int atomic, int copy) {
    (void)sel; (void)atomic; (void)copy;
    if (self) *(void **)((char *)self + offset) = val;
}

// ---------------------------------------------------------------- UIKit Shims

static void *d_NSClassFromString(const void *name) {
    return objc_find_class(ns_cstr(name));
}

static void *d_NSHomeDirectory(void) { return ns_new(DOCS_PATH); }

static void *d_NSSearchPathForDirectoriesInDomains(int dir, int mask, int expand) {
    (void)dir; (void)mask; (void)expand;
    static void *docs_arr[4];
    docs_arr[0] = ns_new(DOCS_PATH);
    return docs_arr;
}

SOFTFP static void d_UIGraphicsBeginImageContext(CGSize size) { (void)size; }
static void d_UIGraphicsEndImageContext(void) {}
static void *d_UIGraphicsGetCurrentContext(void) { return (void *)1; }
static void *d_UIGraphicsGetImageFromCurrentImageContext(void) { return (void *)1; }
SOFTFP static void *d_UIImageJPEGRepresentation(void *img, float comp) { (void)img; (void)comp; return ns_new(""); }
static void *d_UIImagePNGRepresentation(void *img) { (void)img; return ns_new(""); }

// ---------------------------------------------------------------- UIApplicationMain

static int d_UIApplicationMain(int argc, char *argv[], void *principal, void *delegateName) {
    (void)argc; (void)argv; (void)principal;
    const char *del_name = ns_cstr(delegateName);
    l_info("[BOOT] UIApplicationMain starting with delegate: %s", del_name ? del_name : "(null)");

    void *del_cls = objc_find_class(del_name);
    if (!del_cls) del_cls = objc_find_class("AppController");
    if (!del_cls) {
        l_warn("[BOOT] AppController class not yet resolved; initializing registry now...");
        objc_init();
        del_cls = objc_find_class("AppController");
    }

    if (!del_cls) {
        fatal_error("Failed to find delegate class: %s", del_name ? del_name : "AppController");
    }

    void *delegate = objc_class_alloc(del_cls);
    if (objc_has_method(del_cls, "init")) {
        delegate = (void *)call_guest((void *)objc_get_imp(del_cls, "init"), (uintptr_t)delegate, (uintptr_t)"init", 0, 0);
    }

    uintptr_t launch_imp = objc_get_imp(del_cls, "application:didFinishLaunchingWithOptions:");
    if (!launch_imp) launch_imp = objc_get_imp(del_cls, "applicationDidFinishLaunching:");
    if (launch_imp) {
        l_info("[BOOT] Calling delegate didFinishLaunching at 0x%08X...", (unsigned)launch_imp);
        call_guest((void *)launch_imp, (uintptr_t)delegate, (uintptr_t)"application:didFinishLaunchingWithOptions:", (uintptr_t)ns_new(""), 0);
        l_info("[BOOT] delegate ok");
    } else {
        l_error("[BOOT] No didFinishLaunching found on delegate!");
    }

    // Unpause / activate application: in HOS2, guest worker thread at 0x18fe60 loops waiting for *0x34a7a4 to become 0
    uint8_t *pause_flag = (uint8_t *)(MACHO_SLIDE + 0x0034a7a4);
    *pause_flag = 0;

    uintptr_t active_imp = objc_get_imp(del_cls, "applicationDidBecomeActive:");
    if (active_imp) {
        l_info("[BOOT] Calling delegate applicationDidBecomeActive at 0x%08X...", (unsigned)active_imp);
        call_guest((void *)active_imp, (uintptr_t)delegate, (uintptr_t)"applicationDidBecomeActive:", (uintptr_t)ret_obj(), 0);
        l_info("[BOOT] applicationDidBecomeActive ok");
    }
    *pause_flag = 0; // ensure cleared even if delegate did anything

    return 0;
}

// ---------------------------------------------------------------- objc_msgSend Core

static uint32_t objc_msgSend_impl(void *self, const char *sel, va_list ap) {
    if (!self || !sel) return 0;

    // Check if self is a guest class instance or class object
    void *target_cls = NULL;
    uint32_t self_val = (uint32_t)self;
    for (int i = 0; i < registered_class_count; i++) {
        if (registered_classes[i].cls_ptr == self_val) {
            target_cls = (void *)self_val;
            break;
        }
    }
    if (!target_cls && self_val >= 0x10000 && (self_val & 3) == 0) {
        uint32_t isa_val = *(uint32_t *)self;
        for (int i = 0; i < registered_class_count; i++) {
            if (registered_classes[i].cls_ptr == isa_val) {
                target_cls = (void *)isa_val;
                break;
            }
        }
    }

    if (target_cls && objc_has_method(target_cls, sel)) {
        uintptr_t imp = objc_get_imp(target_cls, sel);
        uint32_t a0 = va_arg(ap, uint32_t);
        uint32_t a1 = va_arg(ap, uint32_t);
        uint32_t a2 = va_arg(ap, uint32_t);
        uint32_t a3 = va_arg(ap, uint32_t);
        return call_guest6((void *)imp, (uintptr_t)self, (uintptr_t)sel, a0, a1, a2, a3);
    }

    return objc_msgSend_system_impl(self, sel, ap);
}

static uint32_t objc_msgSend(void *self, const char *sel, ...) {
    va_list ap;
    va_start(ap, sel);
    uint32_t r = objc_msgSend_impl(self, sel, ap);
    va_end(ap);
    return r;
}

static void objc_msgSend_stret(float *ret, void *self, const char *sel, ...) {
    va_list ap;
    va_start(ap, sel);
    if (sel_is(sel, "sizeWithFont:")) {
        ui_font *f = va_arg(ap, ui_font *);
        int size = f ? f->size : 14;
        ret[0] = text_width(size, ns_cstr(self));
        ret[1] = line_height(size);
    } else if (sel_is(sel, "drawAtPoint:withFont:")) {
        union { uint32_t u; float f; } x = {va_arg(ap, uint32_t)}, y = {va_arg(ap, uint32_t)};
        ui_font *f = va_arg(ap, ui_font *);
        int size = f ? f->size : 14;
        if (cur_ctx) draw_text(cur_ctx, ns_cstr(self), (int)x.f, (int)y.f, size);
        ret[0] = text_width(size, ns_cstr(self));
        ret[1] = line_height(size);
    } else if (sel_is(sel, "bounds") || sel_is(sel, "frame")) {
        // Return 320x480 portrait screen bounds
        ret[0] = 0.0f; // x
        ret[1] = 0.0f; // y
        ret[2] = 320.0f; // width
        ret[3] = 480.0f; // height
    } else if (sel_is(sel, "surfaceSize") || sel_is(sel, "size")) {
        ret[0] = 320.0f;
        ret[1] = 480.0f;
    } else if (sel_is(sel, "transform")) {
        ret[0] = 1.0f; ret[1] = 0.0f; ret[2] = 0.0f;
        ret[3] = 1.0f; ret[4] = 0.0f; ret[5] = 0.0f;
    } else {
        log_unknown(self, sel);
        ret[0] = ret[1] = ret[2] = ret[3] = 0.0f;
    }
    va_end(ap);
}

// ---------------------------------------------------------------- Import Table

const import_entry objc_imports[] = {
    // ObjC runtime
    F(objc_msgSend, objc_msgSend),
    F(objc_msgSend_stret, objc_msgSend_stret),
    F(objc_msgSendSuper2, d_objc_msgSendSuper2),
    F(objc_setProperty, d_objc_setProperty),
    F(objc_begin_catch, d_objc_begin_catch),
    F(objc_end_catch, d_objc_end_catch),
    F(objc_enumerationMutation, d_objc_enumerationMutation),
    F(__objc_personality_v0, d___objc_personality_v0),
    V(___CFConstantStringClassReference, d_CFConstantStringClassReference),
    V(OBJC_EHTYPE_id, d_OBJC_EHTYPE_id),
    V(_objc_empty_cache, d_objc_empty_cache),
    V(_objc_empty_vtable, d_objc_empty_vtable),
    // CoreGraphics
    F(CGBitmapContextCreate, cg_BitmapContextCreate),
    F(CGContextRelease, cg_ContextRelease),
    F(CGContextSetGrayFillColor, cg_SetGrayFillColor),
    F(CGContextSetTextDrawingMode, cg_noop),
    F(CGColorSpaceCreateDeviceGray, cg_ColorSpaceCreate),
    F(CGColorSpaceCreateDeviceRGB, cg_ColorSpaceCreate),
    F(CGColorSpaceRelease, cg_noop),
    F(UIGraphicsPushContext, ui_PushContext),
    F(UIGraphicsPopContext, ui_PopContext),
    F(CGAffineTransformMakeRotation, d_CGAffineTransformMakeRotation),
    F(CGAffineTransformRotate, d_CGAffineTransformRotate),
    F(CGContextDrawImage, d_CGContextDrawImage),
    F(CGContextFillRect, d_CGContextFillRect),
    F(CGContextScaleCTM, d_CGContextScaleCTM),
    F(CGContextSetRGBFillColor, d_CGContextSetRGBFillColor),
    F(CGContextTranslateCTM, d_CGContextTranslateCTM),
    F(CGImageGetColorSpace, d_CGImageGetColorSpace),
    F(CGImageGetHeight, d_CGImageGetHeight),
    F(CGImageGetWidth, d_CGImageGetWidth),
    // CoreFoundation
    F(CFAllocatorGetDefault, d_CFAllocatorGetDefault),
    F(CFBundleCopyBundleURL, d_CFBundleCopyBundleURL),
    F(CFBundleCopyResourcesDirectoryURL, d_CFBundleCopyResourcesDirectoryURL),
    F(CFBundleGetMainBundle, d_CFBundleGetMainBundle),
    F(CFBundleGetValueForInfoDictionaryKey, d_CFBundleGetValueForInfoDictionaryKey),
    F(CFRelease, d_CFRelease),
    F(CFRunLoopRunInMode, d_CFRunLoopRunInMode),
    F(CFStringGetCString, d_CFStringGetCString),
    F(CFURLCopyFileSystemPath, d_CFURLCopyFileSystemPath),
    F(CFURLGetFileSystemRepresentation, d_CFURLGetFileSystemRepresentation),
    V(kCFBundleVersionKey, &ptr_kCFBundleVersionKey),
    V(kCFRunLoopDefaultMode, &ptr_kCFRunLoopDefaultMode),
    // UIKit / Foundation
    F(UIApplicationMain, d_UIApplicationMain),
    F(NSClassFromString, d_NSClassFromString),
    F(NSHomeDirectory, d_NSHomeDirectory),
    F(NSSearchPathForDirectoriesInDomains, d_NSSearchPathForDirectoriesInDomains),
    V(NSErrorFailingURLStringKey, &ptr_NSErrorFailingURLStringKey),
    V(NSLocaleCountryCode, &ptr_NSLocaleCountryCode),
    V(UIKeyboardWillHideNotification, &ptr_UIKeyboardWillHideNotification),
    V(UIKeyboardWillShowNotification, &ptr_UIKeyboardWillShowNotification),
    F(UIGraphicsBeginImageContext, d_UIGraphicsBeginImageContext),
    F(UIGraphicsEndImageContext, d_UIGraphicsEndImageContext),
    F(UIGraphicsGetCurrentContext, d_UIGraphicsGetCurrentContext),
    F(UIGraphicsGetImageFromCurrentImageContext, d_UIGraphicsGetImageFromCurrentImageContext),
    F(UIImageJPEGRepresentation, d_UIImageJPEGRepresentation),
    F(UIImagePNGRepresentation, d_UIImagePNGRepresentation),
    // Classes
    V(OBJC_CLASS_$_CAEAGLLayer, &OBJC_CLASS_$_CAEAGLLayer),
    V(OBJC_CLASS_$_EAGLContext, &OBJC_CLASS_$_EAGLContext),
    V(OBJC_CLASS_$_MPMediaItemCollection, &OBJC_CLASS_$_MPMediaItemCollection),
    V(OBJC_CLASS_$_MPMediaLibrary, &OBJC_CLASS_$_MPMediaLibrary),
    V(OBJC_CLASS_$_MPMediaQuery, &OBJC_CLASS_$_MPMediaQuery),
    V(OBJC_CLASS_$_MPMoviePlayerController, &OBJC_CLASS_$_MPMoviePlayerController),
    V(OBJC_CLASS_$_MPMoviePlayerViewController, &OBJC_CLASS_$_MPMoviePlayerViewController),
    V(OBJC_METACLASS_$_MPMoviePlayerViewController, &OBJC_METACLASS_$_MPMoviePlayerViewController),
    V(OBJC_CLASS_$_MPMusicPlayerController, &OBJC_CLASS_$_MPMusicPlayerController),
    V(OBJC_CLASS_$_NSArray, &OBJC_CLASS_$_NSArray),
    V(OBJC_CLASS_$_NSAutoreleasePool, &OBJC_CLASS_$_NSAutoreleasePool),
    V(OBJC_CLASS_$_NSBundle, &OBJC_CLASS_$_NSBundle),
    V(OBJC_CLASS_$_NSData, &OBJC_CLASS_$_NSData),
    V(OBJC_CLASS_$_NSDate, &OBJC_CLASS_$_NSDate),
    V(OBJC_CLASS_$_NSDateFormatter, &OBJC_CLASS_$_NSDateFormatter),
    V(OBJC_CLASS_$_NSDictionary, &OBJC_CLASS_$_NSDictionary),
    V(OBJC_CLASS_$_NSError, &OBJC_CLASS_$_NSError),
    V(OBJC_CLASS_$_NSFileManager, &OBJC_CLASS_$_NSFileManager),
    V(OBJC_CLASS_$_NSLocale, &OBJC_CLASS_$_NSLocale),
    V(OBJC_CLASS_$_NSMutableData, &OBJC_CLASS_$_NSMutableData),
    V(OBJC_CLASS_$_NSMutableURLRequest, &OBJC_CLASS_$_NSMutableURLRequest),
    V(OBJC_CLASS_$_NSNotificationCenter, &OBJC_CLASS_$_NSNotificationCenter),
    V(OBJC_CLASS_$_NSNumber, &OBJC_CLASS_$_NSNumber),
    V(OBJC_CLASS_$_NSObject, &OBJC_CLASS_$_NSObject),
    V(OBJC_METACLASS_$_NSObject, &OBJC_METACLASS_$_NSObject),
    V(OBJC_CLASS_$_NSString, &OBJC_CLASS_$_NSString),
    V(OBJC_CLASS_$_NSThread, &OBJC_CLASS_$_NSThread),
    V(OBJC_CLASS_$_NSTimer, &OBJC_CLASS_$_NSTimer),
    V(OBJC_CLASS_$_NSURL, &OBJC_CLASS_$_NSURL),
    V(OBJC_CLASS_$_NSURLConnection, &OBJC_CLASS_$_NSURLConnection),
    V(OBJC_CLASS_$_NSURLRequest, &OBJC_CLASS_$_NSURLRequest),
    V(OBJC_CLASS_$_NSUserDefaults, &OBJC_CLASS_$_NSUserDefaults),
    V(OBJC_CLASS_$_UIAccelerometer, &OBJC_CLASS_$_UIAccelerometer),
    V(OBJC_CLASS_$_UIActivityIndicatorView, &OBJC_CLASS_$_UIActivityIndicatorView),
    V(OBJC_CLASS_$_UIAlertView, &OBJC_CLASS_$_UIAlertView),
    V(OBJC_CLASS_$_UIApplication, &OBJC_CLASS_$_UIApplication),
    V(OBJC_CLASS_$_UIButton, &OBJC_CLASS_$_UIButton),
    V(OBJC_CLASS_$_UIColor, &OBJC_CLASS_$_UIColor),
    V(OBJC_CLASS_$_UIDevice, &OBJC_CLASS_$_UIDevice),
    V(OBJC_CLASS_$_UIFont, &OBJC_CLASS_$_UIFont),
    V(OBJC_CLASS_$_UIImage, &OBJC_CLASS_$_UIImage),
    V(OBJC_CLASS_$_UIImagePickerController, &OBJC_CLASS_$_UIImagePickerController),
    V(OBJC_CLASS_$_UIImageView, &OBJC_CLASS_$_UIImageView),
    V(OBJC_CLASS_$_UILabel, &OBJC_CLASS_$_UILabel),
    V(OBJC_CLASS_$_UINavigationController, &OBJC_CLASS_$_UINavigationController),
    V(OBJC_METACLASS_$_UINavigationController, &OBJC_METACLASS_$_UINavigationController),
    V(OBJC_CLASS_$_UIScreen, &OBJC_CLASS_$_UIScreen),
    V(OBJC_CLASS_$_UITextField, &OBJC_CLASS_$_UITextField),
    V(OBJC_CLASS_$_UITextView, &OBJC_CLASS_$_UITextView),
    V(OBJC_CLASS_$_UIView, &OBJC_CLASS_$_UIView),
    V(OBJC_METACLASS_$_UIView, &OBJC_METACLASS_$_UIView),
    V(OBJC_CLASS_$_UIViewController, &OBJC_CLASS_$_UIViewController),
    V(OBJC_METACLASS_$_UIViewController, &OBJC_METACLASS_$_UIViewController),
    V(OBJC_CLASS_$_UIWebView, &OBJC_CLASS_$_UIWebView),
    V(OBJC_CLASS_$_UIWindow, &OBJC_CLASS_$_UIWindow),
};
const int objc_imports_count = sizeof(objc_imports) / sizeof(objc_imports[0]);
