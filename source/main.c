/**
 * @file main.c
 * @brief Application entry point and runtime bootstrap for the PS Vita iOS loader.
 */
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/system_param.h>
#include <stdio.h>
#include <string.h>

#include "darwin.h"
#include "gles.h"
#include "hle.h"
#include "input.h"
#include "log.h"
#include "macho.h"
#include "objc.h"
#include "patch.h"

int _newlib_heap_size_user = 256 * 1024 * 1024;
// Guest main thread requires substantial stack headroom (4 MB)
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;

// Rotating log file manager (heroofspartaii_001.log .. heroofspartaii_999.log)
static int next_log_number(void) {
    sceIoMkdir(DATA_PATH, 0777);
    sceIoMkdir(LOGS_PATH, 0777);
    int last = 0, n;
    uint64_t newest = 0;
    SceUID d = sceIoDopen(LOGS_PATH);
    SceIoDirent e;
    while (d >= 0 && sceIoDread(d, &e) > 0) {
        const SceDateTime *t = &e.d_stat.st_mtime;
        uint64_t key = (uint64_t)t->year << 48 | (uint64_t)t->month << 40 | (uint64_t)t->day << 32 |
                       (uint64_t)t->hour << 24 | t->minute << 16 | t->second << 8 | t->microsecond / 10000;
        if (sscanf(e.d_name, "heroofspartaii_%3d.log", &n) == 1 && n >= 1 && n <= 999 && key >= newest) {
            newest = key;
            last = n;
        }
    }
    if (d >= 0)
        sceIoDclose(d);
    return (last % 999) + 1;
}

int main(int argc, char *argv[]) {
    // Standard PS Vita overclock for optimal emulation frame pacing
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    // Prepare save data and logs directories
    sceIoMkdir(DATA_PATH, 0777);
    sceIoMkdir(DOCS_PATH, 0777);
    char log_path[128];
    snprintf(log_path, sizeof(log_path), LOGS_PATH "heroofspartaii_%03d.log", next_log_number());
    log_open(log_path);
    l_info("=== iOS Mach-O Loader for PS Vita Initializing ===");

    // Verify ShaRKBR33D runtime availability
    if (sceIoGetstat("ur0:/data/libshacccg.suprx", &(SceIoStat){0}) < 0 &&
        sceIoGetstat("ur0:/data/external/libshacccg.suprx", &(SceIoStat){0}) < 0) {
        fatal_error("Missing libshacccg.suprx (install it via ShaRKBR33D on console).");
    }

    // Initialize AppUtil and system common dialogs
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    sceAppUtilInit(&init, &boot);

    SceCommonDialogConfigParam dlg;
    sceCommonDialogConfigParamInit(&dlg);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&dlg.language);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&dlg.enterButtonAssign);
    sceCommonDialogSetConfigParam(&dlg);

    // Initialize subsystems
    gles_init();
    darwin_init();
    input_init();

    // Check game data existence
    if (sceIoGetstat(BUNDLE_PATH "HOS2", &(SceIoStat){0}) < 0) {
        fatal_error("Missing %s\nPlease place decrypted IPA .app contents in ux0:data/heroofspartaii/game_app/", BUNDLE_PATH "HOS2");
    }
    if (sceIoGetstat(DATA_PATH "rebase.bin", &(SceIoStat){0}) < 0) {
        fatal_error("Missing %s\nPlease copy rebase.bin to ux0:data/heroofspartaii/", DATA_PATH "rebase.bin");
    }

    // Load guest Mach-O executable
    l_info("Loading primary Mach-O executable...");
    int res = macho_load(BUNDLE_PATH "HOS2", DATA_PATH "rebase.bin");
    if (res < 0) {
        fatal_error("Failed to load Mach-O executable at %s", BUNDLE_PATH "HOS2");
    }

    // Initialize game-specific hooks and memory patches
    patch_init();

    // Register guest Objective-C classes from __objc_classlist
    objc_init();

    // Execute C/C++ static constructors (__mod_init_func)
    macho_run_inits();

    // Locate entry point
    uintptr_t entry = macho_sym("_main");
    if (!entry)
        entry = macho_sym("_SDL_main");
    if (!entry)
        entry = macho_sym("_startClet");
    if (!entry)
        entry = MACHO_SLIDE + 0x00190470; // HOS2 main() function

    if (entry) {
        l_info("Jumping to guest entry point at 0x%08X (Mach-O offset 0x%08X)",
               (unsigned)entry, (unsigned)(entry - MACHO_SLIDE));
        call_guest((void *)entry, 0, 0, 0, 0);
    } else {
        l_warn("No entry point found; entering default event loop...");
    }

    // Main fallback event loop (presentation is driven by the game render thread via presentRenderbuffer:)
    for (;;) {
        input_poll();
        ime_poll();
        patch_frame();
        sceKernelDelayThread(16000); // ~60 fps
    }

    return 0;
}
