/**
 * @file darwin.c
 * @brief iOS Darwin libc, libgcc, and pthread emulation layer over Vita newlib.
 * @details Implements ARMv6 softfp Darwin system calls, POSIX threads with SceUID safety,
 *          virtual filesystem path translation to ux0:data/, high-resolution timers,
 *          and mathematical builtins.
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "darwin_printf.h"
#include "hle.h"
#include "imports.h"
#include "log.h"
#include "macho.h"
#include "runetype.h"

// ---------------------------------------------------------------- paths

// Emulated working directory (Vita processes have none). HOS2 chdir()s to the bundle and
// opens assets relative to it; the bundle is also the default.
static char cwd[512] = BUNDLE_PATH;

static int is_device_path(const char *p) {
    return !strncmp(p, "ux0:", 4) || !strncmp(p, "ur0:", 4) || !strncmp(p, "app0:", 5);
}

// Paths created by the game are mapped to the Vita: device paths stay, iOS absolute paths
// ("/var/...") go under DATA_PATH and relative ones under the working directory.
// "dir/../" is collapsed (sceIo does not resolve it): the game opens "<bundle>/../Documents/x",
// and the iOS sandbox Documents/ folder is DOCS_PATH.
const char *darwin_path(const char *in, char *out, size_t outlen) {
    char tmp[512];
    if (is_device_path(in)) {
        snprintf(tmp, sizeof(tmp), "%s", in);
    } else if (*in == '/') {
        while (*in == '/')
            in++;
        snprintf(tmp, sizeof(tmp), DATA_PATH "%s", in);
    } else {
        while (in[0] == '.' && in[1] == '/')
            in += 2;
        snprintf(tmp, sizeof(tmp), "%s%s", cwd, in);
    }
    for (char *p; (p = strstr(tmp, "/../"));) {
        char *q = p;
        while (q > tmp && q[-1] != '/' && q[-1] != ':')
            q--;
        memmove(q, p + 4, strlen(p + 4) + 1);
    }
    static const char docs[] = DATA_PATH "Documents";
    size_t dl = sizeof(docs) - 1;
    if (!strncmp(tmp, docs, dl) && (tmp[dl] == '/' || !tmp[dl]))
        snprintf(out, outlen, DOCS_PATH "%s", tmp[dl] ? tmp + dl + 1 : "");
    else
        snprintf(out, outlen, "%s", tmp);
    return out;
}

static FILE *d_fopen(const char *path, const char *mode) {
    char p[512];
    darwin_path(path, p, sizeof(p));
    FILE *f = fopen(p, mode);
    if (!f && path && path[0] == '/') {
        const char *rel = path;
        while (*rel == '/')
            rel++;
        char p2[512];
        snprintf(p2, sizeof(p2), BUNDLE_PATH "%s", rel);
        f = fopen(p2, mode);
        if (f) {
            l_info("fopen(%s, %s) -> fallback to %s (%p)", path, mode, p2, f);
            return f;
        }
    }
    if (!f) {
        l_warn("fopen(%s -> %s, %s) -> FAILED (errno %d)", path, p, mode, errno);
    } else {
        l_info("fopen(%s -> %s, %s) -> %p", path, p, mode, f);
    }
    return f;
}

static int d_mkdir(const char *path, int mode) {
    char p[512];
    return mkdir(darwin_path(path, p, sizeof(p)), mode);
}

static int d_chdir(const char *path) {
    char p[sizeof(cwd) - 1];
    darwin_path(path, p, sizeof(p));
    SceIoStat st;
    if (sceIoGetstat(p, &st) < 0 || !SCE_S_ISDIR(st.st_mode)) {
        l_warn("chdir(%s): not a directory", p);
        errno = ENOENT;
        return -1;
    }
    size_t n = strlen(p);
    snprintf(cwd, sizeof(cwd), "%s%s", p, n && p[n - 1] == '/' ? "" : "/");
    l_info("chdir(%s) -> %s", path, cwd);
    return 0;
}

static char *d_getcwd(char *buf, size_t size) {
    snprintf(buf, size, "%s", cwd);
    size_t n = strlen(buf);
    if (n > 1 && buf[n - 1] == '/' && buf[n - 2] != ':')
        buf[n - 1] = 0;
    return buf;
}

// ---------------------------------------------------------------- stdio / errno / _chk

static FILE *d_stderrp, *d_stdinp;

static int *d___error(void) { return &errno; }

static void *d___memcpy_chk(void *d, const void *s, size_t n, size_t dn) { return memcpy(d, s, n); }
static char *d___strcpy_chk(char *d, const char *s, size_t dn) { return strcpy(d, s); }

// ---------------------------------------------------------------- printf with Darwin ABI (darwin_printf.c)

static int d_sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int r = darwin_vformat(buf, INT_MAX, fmt, &w);
    va_end(ap);
    return r;
}
static int d___sprintf_chk(char *buf, int flag, size_t len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int r = darwin_vformat(buf, len, fmt, &w);
    va_end(ap);
    return r;
}
static int d_vsprintf(char *buf, const char *fmt, const uint32_t *ap) {
    words w = {NULL, ap};
    return darwin_vformat(buf, INT_MAX, fmt, &w);
}
static int d_vsnprintf(char *buf, size_t size, const char *fmt, const uint32_t *ap) {
    words w = {NULL, ap};
    return darwin_vformat(buf, size, fmt, &w);
}

static void d___assert_rtn(const char *func, const char *file, int line, const char *expr) {
    l_error("guest assertion failed: %s (%s:%d %s)", expr, file, line, func);
}

// NSLog(@"format", ...): format is a CFString (C string pointer at +8).
static void d_NSLog(void *cfstr, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, cfstr);
    words w = {&ap, NULL};
    darwin_vformat(buf, sizeof(buf), cfstr ? *(const char **)((uint8_t *)cfstr + 8) : "(nil)", &w);
    va_end(ap);
    l_info("NSLog: %s", buf);
}

static int qsort_guest_cmp;
static int qsort_tramp(const void *a, const void *b) {
    return (int)call_guest((void *)qsort_guest_cmp, (uint32_t)a, (uint32_t)b, 0, 0);
}
static void d_qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    int prev = qsort_guest_cmp; // nested qsort: restored upon return
    qsort_guest_cmp = (int)cmp;
    qsort(base, n, sz, qsort_tramp);
    qsort_guest_cmp = prev;
}

// ---------------------------------------------------------------- time / system

static uint64_t d_mach_absolute_time(void) { return sceKernelGetProcessTimeWide(); }

typedef struct { uint32_t numer, denom; } mach_timebase_info_t;
static int d_mach_timebase_info(mach_timebase_info_t *info) {
    info->numer = 1000; // mach_absolute_time is in microseconds
    info->denom = 1;
    return 0;
}

static int d_usleep(unsigned us) {
    sceKernelDelayThread(us);
    return 0;
}

static int d_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    if (!strcmp(name, "hw.machine")) {
        const char *model = "iPhone2,1"; // iPhone 3GS: game selects capable GLES1 texture profile
        size_t len = strlen(model) + 1;
        if (oldp && *oldlenp >= len)
            memcpy(oldp, model, len);
        *oldlenp = len;
        return 0;
    }
    errno = ENOENT;
    return -1;
}

static void d_exit(int code) {
    l_info("guest invoked exit(%d)", code);
    sceKernelExitProcess(0);
}

// ---------------------------------------------------------------- networking (offline stub)

static int d_net_fail(void) {
    errno = ENETDOWN;
    return -1;
}
static void *d_gethostbyname(const char *n) { return NULL; }
static int d_select(int n, void *r, void *w, void *e, void *t) { return 0; }

// ---------------------------------------------------------------- pthreads

// Darwin: pthread_mutex_t = {long sig; char opaque[40]}. Vita mutex stored in opaque.
// Darwin PTHREAD_MUTEX_INITIALIZER signature: 0x32AAABA7 (lazy initialized on first use).
#define DARWIN_MUTEX_SIG_INIT 0x32AAABA7
#define ML_MUTEX_SIG 0x4D4C5458
#define ML_MUTEXATTR_SIG 0x4D4C5441
typedef struct { uint32_t sig; pthread_mutex_t m; } d_mutex;
typedef struct { uint32_t sig; int type; } d_mutexattr;

static int d_pthread_mutexattr_init(d_mutexattr *a) {
    a->sig = ML_MUTEXATTR_SIG;
    a->type = 0;
    return 0;
}
static int d_pthread_mutexattr_settype(d_mutexattr *a, int type) {
    a->type = type; // Darwin: 0 normal, 1 errorcheck, 2 recursive
    return 0;
}
static int d_pthread_mutex_init(d_mutex *m, const d_mutexattr *a) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if (a && a->sig == ML_MUTEXATTR_SIG)
        pthread_mutexattr_settype(&attr, a->type == 2 ? PTHREAD_MUTEX_RECURSIVE
                                         : a->type == 1 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
    int r = pthread_mutex_init(&m->m, &attr);
    pthread_mutexattr_destroy(&attr);
    m->sig = ML_MUTEX_SIG;
    return r;
}
static void mutex_lazy(d_mutex *m) {
    if (m->sig != ML_MUTEX_SIG) {
        if (m->sig != DARWIN_MUTEX_SIG_INIT)
            l_error("uninitialized mutex (sig 0x%08X) at %p", (unsigned)m->sig, m);
        d_pthread_mutex_init(m, NULL);
    }
}
static int d_pthread_mutex_lock(d_mutex *m) {
    mutex_lazy(m);
    return pthread_mutex_lock(&m->m);
}
static int d_pthread_mutex_unlock(d_mutex *m) {
    mutex_lazy(m);
    return pthread_mutex_unlock(&m->m);
}
static int d_pthread_mutex_destroy(d_mutex *m) {
    if (m->sig != ML_MUTEX_SIG)
        return 0;
    m->sig = 0;
    return pthread_mutex_destroy(&m->m);
}

typedef struct { void *fn, *arg; } guest_thread;
static void *thread_tramp(void *p) {
    guest_thread t = *(guest_thread *)p;
    free(p);
    l_info("[THREAD] Game thread starting: fn=%p (offset 0x%08X) arg=%p",
           t.fn, (unsigned)((uintptr_t)t.fn - MACHO_SLIDE), t.arg);
    void *ret = (void *)call_guest(t.fn, (uint32_t)t.arg, 0, 0, 0);
    l_info("[THREAD] Game thread exited with %p", ret);
    return ret;
}
static int d_pthread_create(pthread_t *thr, const void *darwin_attr, void *fn, void *arg) {
    guest_thread *t = malloc(sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024); // iOS default 512 KB secondary thread stack
    int r = pthread_create(thr, &attr, thread_tramp, t);
    pthread_attr_destroy(&attr);
    l_debug("pthread_create(0x%08X) -> %d", (unsigned)fn - MACHO_SLIDE, r);
    return r;
}
static int d_pthread_detach(pthread_t t) {
    if (!t)
        return 0;
    // If handle is a Sony Vita SceUID (0x4001xxxx)
    if (((uintptr_t)t & 0xFFFF0000) == 0x40010000) {
        l_info("pthread_detach(SceUID 0x%08X) -> ok", (unsigned)(uintptr_t)t);
        return 0;
    }
    // If handle is a pointer to pte_thread_t in user memory
    uintptr_t addr = (uintptr_t)t;
    if (addr >= 0x81000000 && addr < 0x90000000 && (addr & 3) == 0) {
        uint32_t first = *(uint32_t *)addr;
        if ((first & 0xFFFF0000) == 0x40010000) {
            return pthread_detach(t);
        }
    }
    l_info("pthread_detach(%p) safely ignored", (void *)t);
    return 0;
}

static pthread_t d_pthread_self(void) {
    pthread_t p = pthread_self();
    if (p)
        return p;
    return (pthread_t)sceKernelGetThreadId();
}

static int d_pthread_setschedparam(pthread_t t, int policy, const void *param) { return 0; }

// Named semaphores (FMOD): Darwin sem_t* -> Vita SceUID.
typedef struct { SceUID id; } d_sem;
static d_sem *d_sem_open(const char *name, int oflag, ...) {
    va_list ap;
    va_start(ap, oflag);
    va_arg(ap, int); // mode
    unsigned value = va_arg(ap, unsigned);
    va_end(ap);
    d_sem *s = malloc(sizeof(*s));
    s->id = sceKernelCreateSema("ml_sem", 0, value, 0x7FFFFFFF, NULL);
    if (s->id < 0) {
        free(s);
        return (d_sem *)-1; // SEM_FAILED
    }
    return s;
}
static int d_sem_close(d_sem *s) {
    if (!s || s == (d_sem *)-1)
        return -1;
    sceKernelDeleteSema(s->id);
    free(s);
    return 0;
}
static int d_sem_unlink(const char *name) { return 0; }
static int d_sem_wait(d_sem *s) {
    if (!s || s == (d_sem *)-1)
        return -1;
    return sceKernelWaitSema(s->id, 1, NULL) < 0 ? -1 : 0;
}
static int d_sem_post(d_sem *s) {
    if (!s || s == (d_sem *)-1)
        return -1;
    return sceKernelSignalSema(s->id, 1) < 0 ? -1 : 0;
}

// ---------------------------------------------------------------- C++ / blocks / exceptions

static int d___cxa_atexit(void *f, void *a, void *d) { return 0; } // games never terminate cleanly
static int d___cxa_guard_acquire(uint8_t *g) { return *g == 0; }
static void d___cxa_guard_release(uint8_t *g) { *g = 1; }
static void d___cxa_pure_virtual(void) { fatal_error("__cxa_pure_virtual"); }
static void d_sjlj_noop(void *ctx) {}
static void d_sjlj_resume(void *ctx) { fatal_error("C++/ObjC exception thrown (_Unwind_SjLj_Resume)"); }

static uint8_t d_NSConcreteStackBlock[32];
static void d_Block_object_assign(void **dst, void *src, int flags) { *dst = src; }
static void d_Block_object_dispose(void *obj, int flags) {}

static uint8_t d_dispatch_main_q[64];
static void *d_dispatch_get_global_queue(long prio, unsigned long flags) { return d_dispatch_main_q; }
static void d_dispatch_async(void *q, void *block) { l_debug("dispatch_async ignored"); }

// ---------------------------------------------------------------- libgcc softfp (Apple *vfp)

SOFTFP static float f_add(float a, float b) { return a + b; }
SOFTFP static float f_sub(float a, float b) { return a - b; }
SOFTFP static float f_mul(float a, float b) { return a * b; }
SOFTFP static float f_div(float a, float b) { return a / b; }
SOFTFP static double d_add(double a, double b) { return a + b; }
SOFTFP static double d_sub(double a, double b) { return a - b; }
SOFTFP static double d_mul(double a, double b) { return a * b; }
// compiler-rt *vfp comparisons: return 1 if condition is true (unlike libgcc -1/0/1).
SOFTFP static int f_eq(float a, float b) { return a == b; }
SOFTFP static int f_ne(float a, float b) { return !(a == b); }
SOFTFP static int f_gt(float a, float b) { return a > b; }
SOFTFP static int f_ge(float a, float b) { return a >= b; }
SOFTFP static int f_lt(float a, float b) { return a < b; }
SOFTFP static int f_le(float a, float b) { return a <= b; }
SOFTFP static int dd_gt(double a, double b) { return a > b; }
SOFTFP static int dd_ge(double a, double b) { return a >= b; }
SOFTFP static int dd_lt(double a, double b) { return a < b; }
SOFTFP static double f_ext(float a) { return a; }
SOFTFP static float d_trunc(double a) { return (float)a; }
SOFTFP static int f_toi(float a) { return (int)a; }
SOFTFP static int d_toi(double a) { return (int)a; }
SOFTFP static long long f_toll(float a) { return (long long)a; }
SOFTFP static long long d_toll(double a) { return (long long)a; }
SOFTFP static unsigned long long f_toull(float a) { return (unsigned long long)a; }
SOFTFP static float i_tof(int a) { return (float)a; }
SOFTFP static float u_tof(unsigned a) { return (float)a; }
SOFTFP static double i_tod(int a) { return (double)a; }
SOFTFP static double ll_tod(long long a) { return (double)a; }
SOFTFP static float ll_tof(long long a) { return (float)a; }

static int i_div(int a, int b) { return a / b; }
static int i_mod(int a, int b) { return a % b; }
static unsigned u_div(unsigned a, unsigned b) { return a / b; }
static unsigned u_mod(unsigned a, unsigned b) { return a % b; }
static long long ll_div(long long a, long long b) { return a / b; }
static long long ll_mod(long long a, long long b) { return a % b; }
static long long ll_mul(long long a, long long b) { return a * b; }
static unsigned long long ull_div(unsigned long long a, unsigned long long b) { return a / b; }
static unsigned long long ull_mod(unsigned long long a, unsigned long long b) { return a % b; }
static int ll_cmp(long long a, long long b) { return a < b ? 0 : a == b ? 1 : 2; }

static FILE *d_stdoutp;

static int d_printf(const char *fmt, ...) {
    char out[1024];
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int ret = darwin_vformat(out, sizeof(out), fmt, &w);
    va_end(ap);
    fputs(out, stdout);
    return ret;
}

static int d_fprintf(FILE *stream, const char *fmt, ...) {
    char out[1024];
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int ret = darwin_vformat(out, sizeof(out), fmt, &w);
    va_end(ap);
    fputs(out, stream);
    return ret;
}

static int d_snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int ret = darwin_vformat(str, size, fmt, &w);
    va_end(ap);
    return ret;
}

static int d___tolower(int c) { return tolower(c); }
static int d___toupper(int c) { return toupper(c); }

static int d___maskrune(int c, unsigned long f) {
    if (c < 0 || c > 255)
        return 0;
    return (d_DefaultRuneLocale.runetype[(uint8_t)c] & f) != 0;
}

static char *d___strncpy_chk(char *dst, const char *src, size_t len, size_t dstlen) {
    (void)dstlen;
    return strncpy(dst, src, len);
}

static void *d_SCNetworkReachabilityCreateWithName(void *allocator, const char *nodename) {
    (void)allocator; (void)nodename;
    return (void *)1;
}

static int d_SCNetworkReachabilityGetFlags(void *target, uint32_t *flags) {
    (void)target;
    if (flags) *flags = 2; // kSCNetworkReachabilityFlagsReachable
    return 1;
}

static size_t d_strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size) {
        size_t n = len >= size ? size - 1 : len;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

static int d_ioctl(int fd, int req, ...) {
    (void)fd; (void)req;
    return -1;
}

static int d_sigaction(int signum, const void *act, void *oldact) {
    (void)signum; (void)act; (void)oldact;
    return 0;
}

static int d_recvfrom(int s, void *buf, size_t len, int flags, void *from, void *fromlen) {
    (void)s; (void)buf; (void)len; (void)flags; (void)from; (void)fromlen;
    return -1;
}

static int d_sendto(int s, const void *msg, size_t len, int flags, const void *to, int tolen) {
    (void)s; (void)msg; (void)len; (void)flags; (void)to; (void)tolen;
    return -1;
}

static char *d_inet_ntoa(uint32_t in) {
    static char buf[32];
    unsigned char *b = (unsigned char *)&in;
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

static void *d_getprotobyname(const char *name) {
    (void)name;
    return NULL;
}

static int d_getsockname(int s, void *name, void *namelen) {
    (void)s; (void)name; (void)namelen;
    return -1;
}

static int d_getsockopt(int s, int level, int optname, void *optval, void *optlen) {
    (void)s; (void)level; (void)optname; (void)optval; (void)optlen;
    return -1;
}

SOFTFP static double d___floatundidf(uint64_t a) { return (double)a; }
static int d___gxx_personality_sj0(void) { return 0; }
static void *d___cxa_begin_catch(void *e) { return e; }
static void d___cxa_end_catch(void) {}
static void d___cxa_rethrow(void) { fatal_error("Unhandled C++ exception rethrow"); }

extern void *__dynamic_cast(const void *, const void *, const void *, ptrdiff_t);

// External C++ ABI functions from libstdc++
extern void _ZNKSs4findEPKcj(void);
extern void _ZNKSs4findEPKcjj(void);
extern void _ZNKSs4findEcj(void);
extern void _ZNKSs5rfindEcj(void);
extern void _ZNKSs6substrEjj(void);
extern void _ZNKSs7compareEPKc(void);
extern void _ZNKSs7compareERKSs(void);
extern void _ZNKSt15basic_stringbufIcSt11char_traitsIcESaIcEE3strEv(void);
extern void _ZNKSt9basic_iosIcSt11char_traitsIcEE5widenEc(void);
extern void _ZNSi3getEv(void);
extern void _ZNSi4peekEv(void);
extern void _ZNSo9_M_insertIlEERSoT_(void);
extern void _ZNSolsEi(void);
extern void _ZNSs12_M_leak_hardEv(void);
extern void _ZNSs4_Rep10_M_destroyERKSaIcE(void);
extern void *_ZNSs4_Rep20_S_empty_rep_storageE;
extern void _ZNSs4_Rep9_S_createEjjRKSaIcE(void);
extern void _ZNSs6appendEPKcj(void);
extern void _ZNSs6appendERKSs(void);
extern void _ZNSs6appendEjc(void);
extern void _ZNSs6assignEPKcj(void);
extern void _ZNSs6assignERKSs(void);
extern void _ZNSs6insertEjRKSsjj(void);
extern void _ZNSs6resizeEjc(void);
extern void _ZNSs7reserveEj(void);
extern void _ZNSs9push_backEc(void);
extern void _ZNSsC1EPKcRKSaIcE(void);
extern void _ZNSsC1EPKcjRKSaIcE(void);
extern void _ZNSsC1ERKSs(void);
extern void _ZNSsC1ERKSsjj(void);
extern void _ZNSsD2Ev(void);
extern void _ZNSsixEj(void);
extern void _ZNSt15_List_node_base4hookEPS_(void);
extern void _ZNSt15_List_node_base6unhookEv(void);
extern void _ZNSt15_List_node_base8transferEPS_S0_(void);
extern void _ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode(void);
extern void _ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev(void);
extern void _ZNSt6localeC1Ev(void);
extern void _ZNSt6localeD1Ev(void);
extern void _ZNSt8ios_base4InitC1Ev(void);
extern void _ZNSt8ios_base4InitD1Ev(void);
extern void _ZNSt8ios_baseC2Ev(void);
extern void _ZNSt8ios_baseD2Ev(void);
extern void _ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E(void);
extern void _ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate(void);
extern void _ZSt16__ostream_insertIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_PKS3_i(void);
extern void _ZSt17__throw_bad_allocv(void);
extern void _ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base(void);
extern void _ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base(void);
extern void _ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base(void);
extern void _ZSt19__throw_logic_errorPKc(void);
extern void _ZSt20__throw_length_errorPKc(void);
extern void _ZSt20__throw_out_of_rangePKc(void);
extern void _ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_(void);
extern void _ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_(void);
extern void _ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_(void);
extern void *_ZTTSt18basic_stringstreamIcSt11char_traitsIcESaIcEE;
extern void *_ZTVN10__cxxabiv117__class_type_infoE;
extern void *_ZTVN10__cxxabiv120__si_class_type_infoE;
extern void *_ZTVN10__cxxabiv121__vmi_class_type_infoE;
extern void *_ZTVSt15basic_streambufIcSt11char_traitsIcEE;
extern void *_ZTVSt15basic_stringbufIcSt11char_traitsIcESaIcEE;
extern void *_ZTVSt18basic_stringstreamIcSt11char_traitsIcESaIcEE;
extern void *_ZTVSt9basic_iosIcSt11char_traitsIcEE;

SOFTFP static double m_acos(double x) { return acos(x); }
SOFTFP static double m_asin(double x) { return asin(x); }
SOFTFP static double m_atan(double x) { return atan(x); }
SOFTFP static float m_atan2f(float y, float x) { return atan2f(y, x); }
SOFTFP static float m_atanf(float x) { return atanf(x); }
SOFTFP static double m_atof(const char *s) { return atof(s); }
SOFTFP static double m_ceil(double x) { return ceil(x); }
SOFTFP static double m_cosh(double x) { return cosh(x); }
SOFTFP static double m_difftime(time_t t1, time_t t0) { return difftime(t1, t0); }
SOFTFP static double m_exp(double x) { return exp(x); }
SOFTFP static double m_frexp(double x, int *e) { return frexp(x, e); }
SOFTFP static float m_frexpf(float x, int *e) { return frexpf(x, e); }
SOFTFP static float m_ldexpf(float x, int e) { return ldexpf(x, e); }
SOFTFP static double m_log(double x) { return log(x); }
SOFTFP static double m_modf(double x, double *iptr) { return modf(x, iptr); }
SOFTFP static float m_roundf(float x) { return roundf(x); }
SOFTFP static double m_sinh(double x) { return sinh(x); }
SOFTFP static double m_tan(double x) { return tan(x); }
SOFTFP static double m_tanh(double x) { return tanh(x); }

SOFTFP static float m_acosf(float x) { return acosf(x); }
SOFTFP static float m_cosf(float x) { return cosf(x); }
SOFTFP static float m_sinf(float x) { return sinf(x); }
SOFTFP static float m_tanf(float x) { return tanf(x); }
SOFTFP static float m_expf(float x) { return expf(x); }
SOFTFP static float m_logf(float x) { return logf(x); }
SOFTFP static float m_log10f(float x) { return log10f(x); }
SOFTFP static float m_floorf(float x) { return floorf(x); }
SOFTFP static long m_lrintf(float x) { return lrintf(x); }
SOFTFP static double m_cos(double x) { return cos(x); }
SOFTFP static double m_sin(double x) { return sin(x); }
SOFTFP static double m_floor(double x) { return floor(x); }
SOFTFP static double m_log10(double x) { return log10(x); }
SOFTFP static double m_atan2(double y, double x) { return atan2(y, x); }
SOFTFP static double m_fmod(double x, double y) { return fmod(x, y); }
SOFTFP static double m_pow(double x, double y) { return pow(x, y); }
SOFTFP static double m_ldexp(double x, int e) { return ldexp(x, e); }

// ---------------------------------------------------------------- import table

const import_entry darwin_imports[] = {
    // memory / strings
    D(malloc), D(calloc), D(realloc), D(free), D(memcpy), D(memmove), D(memset), D(memcmp),
    D(memchr), D(bzero), D(strlen), D(strcmp), D(strcpy), D(strncpy), D(strstr), D(strtok),
    D(strcat), D(strchr), D(strcoll), D(strcspn), D(strdup), D(strerror), D(strftime),
    F(strlcpy, d_strlcpy), D(strncasecmp), D(strncat), D(strncmp), D(strpbrk), D(strrchr),
    D(strtod), D(strtoul), D(strcasecmp),
    D(atoi), D(rand), D(srand), F(sprintf, d_sprintf), F(vsprintf, d_vsprintf),
    F(vsnprintf, d_vsnprintf), F(snprintf, d_snprintf), F(printf, d_printf),
    F(fprintf, d_fprintf), D(sscanf), D(puts), D(putchar),
    F(qsort, d_qsort), F(_ZdlPv, free),
    F(__memcpy_chk, d___memcpy_chk), F(__strcpy_chk, d___strcpy_chk),
    F(__strncpy_chk, d___strncpy_chk),
    F(__sprintf_chk, d___sprintf_chk), F(__assert_rtn, d___assert_rtn), F(__error, d___error),
    F(NSLog, d_NSLog),
    F(__maskrune, d___maskrune), F(__tolower, d___tolower), F(__toupper, d___toupper),
    V(_DefaultRuneLocale, &d_DefaultRuneLocale),
    // stdio
    F(fopen, d_fopen), D(fclose), D(fread), D(fwrite), D(fseek), D(ftell), D(feof), D(ferror),
    D(fflush), D(fputs), D(fgets), D(fputc), D(freopen), D(fscanf), D(getc), D(ungetc),
    D(setvbuf), D(tmpfile), D(tmpnam), F(mkdir, d_mkdir), D(close),
    V(__stderrp, &d_stderrp), V(__stdinp, &d_stdinp), V(__stdoutp, &d_stdoutp),
    // time / system
    D(gettimeofday), D(time), D(clock), D(gmtime), D(localtime), D(mktime),
    F(usleep, d_usleep), F(mach_absolute_time, d_mach_absolute_time),
    F(mach_timebase_info, d_mach_timebase_info), F(sysctlbyname, d_sysctlbyname),
    F(exit, d_exit), D(atexit), F(chdir, d_chdir), F(getcwd, d_getcwd), D(clearerr), D(getenv), D(gethostname),
    D(perror), D(remove), D(rename), D(system), D(localeconv), D(setlocale),
    F(ioctl, d_ioctl), F(sigaction, d_sigaction), D(setjmp), D(longjmp),
    // network: offline stubs
    F(socket, d_net_fail), F(connect, d_net_fail), F(bind, d_net_fail), F(listen, d_net_fail),
    F(accept, d_net_fail), F(send, d_net_fail), F(recv, d_net_fail), F(setsockopt, d_net_fail),
    F(fcntl, d_net_fail), F(inet_addr, d_net_fail), F(gethostbyname, d_gethostbyname),
    F(select, d_select), F(recvfrom, d_recvfrom), F(sendto, d_sendto),
    F(inet_ntoa, d_inet_ntoa), F(getprotobyname, d_getprotobyname),
    F(getsockname, d_getsockname), F(getsockopt, d_getsockopt),
    F(SCNetworkReachabilityCreateWithName, d_SCNetworkReachabilityCreateWithName),
    F(SCNetworkReachabilityGetFlags, d_SCNetworkReachabilityGetFlags),
    // pthreads / semaphores
    F(pthread_create, d_pthread_create), F(pthread_detach, d_pthread_detach), F(pthread_self, d_pthread_self),
    F(pthread_setschedparam, d_pthread_setschedparam), F(pthread_mutex_init, d_pthread_mutex_init),
    F(pthread_mutex_lock, d_pthread_mutex_lock), F(pthread_mutex_unlock, d_pthread_mutex_unlock),
    F(pthread_mutex_destroy, d_pthread_mutex_destroy), F(pthread_mutexattr_init, d_pthread_mutexattr_init),
    F(pthread_mutexattr_settype, d_pthread_mutexattr_settype), F(sem_open, d_sem_open),
    F(sem_close, d_sem_close), F(sem_unlink, d_sem_unlink), F(sem_wait, d_sem_wait),
    F(sem_post, d_sem_post), D(pthread_cancel), D(pthread_mutex_trylock),
    D(pthread_setcanceltype), D(pthread_testcancel), D(sched_yield),
    // C++ / blocks / exceptions / GCD
    F(__cxa_atexit, d___cxa_atexit), F(__cxa_guard_acquire, d___cxa_guard_acquire),
    F(__cxa_guard_release, d___cxa_guard_release), F(__cxa_pure_virtual, d___cxa_pure_virtual),
    F(__cxa_begin_catch, d___cxa_begin_catch), F(__cxa_end_catch, d___cxa_end_catch),
    F(__cxa_rethrow, d___cxa_rethrow), F(__dynamic_cast, __dynamic_cast),
    F(__gxx_personality_sj0, d___gxx_personality_sj0),
    F(_Unwind_SjLj_Register, d_sjlj_noop), F(_Unwind_SjLj_Unregister, d_sjlj_noop),
    F(_Unwind_SjLj_Resume, d_sjlj_resume), V(_NSConcreteStackBlock, d_NSConcreteStackBlock),
    F(_Block_object_assign, d_Block_object_assign), F(_Block_object_dispose, d_Block_object_dispose),
    V(_dispatch_main_q, d_dispatch_main_q), F(dispatch_get_global_queue, d_dispatch_get_global_queue),
    F(dispatch_async, d_dispatch_async),
    // C++ stdlib ABI
    F_RAW("__ZNKSs4findEPKcm", _ZNKSs4findEPKcj),
    F_RAW("__ZNKSs4findEPKcmm", _ZNKSs4findEPKcjj),
    F_RAW("__ZNKSs4findEcm", _ZNKSs4findEcj),
    F_RAW("__ZNKSs5rfindEcm", _ZNKSs5rfindEcj),
    F_RAW("__ZNKSs6substrEmm", _ZNKSs6substrEjj),
    F_RAW("__ZNKSs7compareEPKc", _ZNKSs7compareEPKc),
    F_RAW("__ZNKSs7compareERKSs", _ZNKSs7compareERKSs),
    F_RAW("__ZNKSt15basic_stringbufIcSt11char_traitsIcESaIcEE3strEv", _ZNKSt15basic_stringbufIcSt11char_traitsIcESaIcEE3strEv),
    F_RAW("__ZNKSt9basic_iosIcSt11char_traitsIcEE5widenEc", _ZNKSt9basic_iosIcSt11char_traitsIcEE5widenEc),
    F_RAW("__ZNSi3getEv", _ZNSi3getEv),
    F_RAW("__ZNSi4peekEv", _ZNSi4peekEv),
    F_RAW("__ZNSo9_M_insertIlEERSoT_", _ZNSo9_M_insertIlEERSoT_),
    F_RAW("__ZNSolsEi", _ZNSolsEi),
    F_RAW("__ZNSs12_M_leak_hardEv", _ZNSs12_M_leak_hardEv),
    F_RAW("__ZNSs4_Rep10_M_destroyERKSaIcE", _ZNSs4_Rep10_M_destroyERKSaIcE),
    V_RAW("__ZNSs4_Rep20_S_empty_rep_storageE", &_ZNSs4_Rep20_S_empty_rep_storageE),
    F_RAW("__ZNSs4_Rep9_S_createEmmRKSaIcE", _ZNSs4_Rep9_S_createEjjRKSaIcE),
    F_RAW("__ZNSs6appendEPKcm", _ZNSs6appendEPKcj),
    F_RAW("__ZNSs6appendERKSs", _ZNSs6appendERKSs),
    F_RAW("__ZNSs6appendEmc", _ZNSs6appendEjc),
    F_RAW("__ZNSs6assignEPKcm", _ZNSs6assignEPKcj),
    F_RAW("__ZNSs6assignERKSs", _ZNSs6assignERKSs),
    F_RAW("__ZNSs6insertEmRKSsmm", _ZNSs6insertEjRKSsjj),
    F_RAW("__ZNSs6resizeEmc", _ZNSs6resizeEjc),
    F_RAW("__ZNSs7reserveEm", _ZNSs7reserveEj),
    F_RAW("__ZNSs9push_backEc", _ZNSs9push_backEc),
    F_RAW("__ZNSsC1EPKcRKSaIcE", _ZNSsC1EPKcRKSaIcE),
    F_RAW("__ZNSsC1EPKcmRKSaIcE", _ZNSsC1EPKcjRKSaIcE),
    F_RAW("__ZNSsC1ERKSs", _ZNSsC1ERKSs),
    F_RAW("__ZNSsC1ERKSsmm", _ZNSsC1ERKSsjj),
    F_RAW("__ZNSsD2Ev", _ZNSsD2Ev),
    F_RAW("__ZNSsixEm", _ZNSsixEj),
    F_RAW("__ZNSt15_List_node_base4hookEPS_", _ZNSt15_List_node_base4hookEPS_),
    F_RAW("__ZNSt15_List_node_base6unhookEv", _ZNSt15_List_node_base6unhookEv),
    F_RAW("__ZNSt15_List_node_base8transferEPS_S0_", _ZNSt15_List_node_base8transferEPS_S0_),
    F_RAW("__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode", _ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode),
    F_RAW("__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev", _ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev),
    F_RAW("__ZNSt6localeC1Ev", _ZNSt6localeC1Ev),
    F_RAW("__ZNSt6localeD1Ev", _ZNSt6localeD1Ev),
    F_RAW("__ZNSt8ios_base4InitC1Ev", _ZNSt8ios_base4InitC1Ev),
    F_RAW("__ZNSt8ios_base4InitD1Ev", _ZNSt8ios_base4InitD1Ev),
    F_RAW("__ZNSt8ios_baseC2Ev", _ZNSt8ios_baseC2Ev),
    F_RAW("__ZNSt8ios_baseD2Ev", _ZNSt8ios_baseD2Ev),
    F_RAW("__ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E", _ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E),
    F_RAW("__ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate", _ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate),
    F_RAW("__ZSt16__ostream_insertIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_PKS3_i", _ZSt16__ostream_insertIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_PKS3_i),
    F_RAW("__ZSt17__throw_bad_allocv", _ZSt17__throw_bad_allocv),
    F_RAW("__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base", _ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base),
    F_RAW("__ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base", _ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base),
    F_RAW("__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base", _ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base),
    F_RAW("__ZSt19__throw_logic_errorPKc", _ZSt19__throw_logic_errorPKc),
    F_RAW("__ZSt20__throw_length_errorPKc", _ZSt20__throw_length_errorPKc),
    F_RAW("__ZSt20__throw_out_of_rangePKc", _ZSt20__throw_out_of_rangePKc),
    F_RAW("__ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_", _ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_),
    F_RAW("__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_", _ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_),
    F_RAW("__ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_", _ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_),
    V_RAW("__ZTTSt18basic_stringstreamIcSt11char_traitsIcESaIcEE", &_ZTTSt18basic_stringstreamIcSt11char_traitsIcESaIcEE),
    V_RAW("__ZTVN10__cxxabiv117__class_type_infoE", &_ZTVN10__cxxabiv117__class_type_infoE),
    V_RAW("__ZTVN10__cxxabiv120__si_class_type_infoE", &_ZTVN10__cxxabiv120__si_class_type_infoE),
    V_RAW("__ZTVN10__cxxabiv121__vmi_class_type_infoE", &_ZTVN10__cxxabiv121__vmi_class_type_infoE),
    V_RAW("__ZTVSt15basic_streambufIcSt11char_traitsIcEE", &_ZTVSt15basic_streambufIcSt11char_traitsIcEE),
    V_RAW("__ZTVSt15basic_stringbufIcSt11char_traitsIcESaIcEE", &_ZTVSt15basic_stringbufIcSt11char_traitsIcESaIcEE),
    V_RAW("__ZTVSt18basic_stringstreamIcSt11char_traitsIcESaIcEE", &_ZTVSt18basic_stringstreamIcSt11char_traitsIcESaIcEE),
    V_RAW("__ZTVSt9basic_iosIcSt11char_traitsIcEE", &_ZTVSt9basic_iosIcSt11char_traitsIcEE),
    // softfp helpers
    F(__addsf3vfp, f_add), F(__subsf3vfp, f_sub), F(__mulsf3vfp, f_mul), F(__divsf3vfp, f_div),
    F(__adddf3vfp, d_add), F(__subdf3vfp, d_sub), F(__muldf3vfp, d_mul),
    F(__eqsf2vfp, f_eq), F(__nesf2vfp, f_ne), F(__gtsf2vfp, f_gt), F(__gesf2vfp, f_ge),
    F(__ltsf2vfp, f_lt), F(__lesf2vfp, f_le), F(__gtdf2vfp, dd_gt), F(__gedf2vfp, dd_ge),
    F(__ltdf2vfp, dd_lt), F(__extendsfdf2vfp, f_ext), F(__truncdfsf2vfp, d_trunc),
    F(__fixsfsivfp, f_toi), F(__fixdfsivfp, d_toi), F(__fixsfdi, f_toll), F(__fixdfdi, d_toll),
    F(__fixunssfdi, f_toull), F(__floatsisfvfp, i_tof), F(__floatunssisfvfp, u_tof),
    F(__floatsidfvfp, i_tod), F(__floatdidf, ll_tod), F(__floatdisf, ll_tof),
    F(__floatundidf, d___floatundidf),
    F(__divsi3, i_div), F(__modsi3, i_mod), F(__udivsi3, u_div), F(__umodsi3, u_mod),
    F(__divdi3, ll_div), F(__moddi3, ll_mod), F(__muldi3, ll_mul), F(__udivdi3, ull_div),
    F(__umoddi3, ull_mod), F(__cmpdi2, ll_cmp),
    // math softfp
    F(acos, m_acos), F(acosf, m_acosf), F(asin, m_asin), F(atan, m_atan),
    F(atan2, m_atan2), F(atan2f, m_atan2f), F(atanf, m_atanf), F(atof, m_atof),
    F(ceil, m_ceil), F(cos, m_cos), F(cosf, m_cosf), F(cosh, m_cosh),
    F(difftime, m_difftime), F(exp, m_exp), F(expf, m_expf), F(floor, m_floor),
    F(floorf, m_floorf), F(fmod, m_fmod), F(frexp, m_frexp), F(frexpf, m_frexpf),
    F(ldexp, m_ldexp), F(ldexpf, m_ldexpf), F(log, m_log), F(log10, m_log10),
    F(log10f, m_log10f), F(logf, m_logf), F(lrintf, m_lrintf), F(modf, m_modf),
    F(pow, m_pow), F(roundf, m_roundf), F(sin, m_sin), F(sinf, m_sinf),
    F(sinh, m_sinh), F(tan, m_tan), F(tanf, m_tanf), F(tanh, m_tanh),
};
const int darwin_imports_count = sizeof(darwin_imports) / sizeof(darwin_imports[0]);

void darwin_init(void) {
    d_stderrp = stderr;
    d_stdinp = stdin;
    d_stdoutp = stdout;
}
