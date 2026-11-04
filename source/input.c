/**
 * @file input.c
 * @brief Input subsystem mapping PS Vita front touch and physical buttons to iOS touch events.
 */
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "input.h"
#include "log.h"
#include "macho.h"
#include "video.h"

#define MAX_SYNTH_TOUCHES 16

touch_point touches[MAX_SYNTH_TOUCHES];
int touch_count = 0;
SceCtrlData pad;

static touch_point prev_touches[MAX_SYNTH_TOUCHES];
static int prev_count = 0;

typedef struct {
    int joy_center_x, joy_center_y, joy_radius;
    int cross_x, cross_y;
    int circle_x, circle_y;
    int square_x, square_y;
    int triangle_x, triangle_y;
    int l_trigger_x, l_trigger_y;
    int r_trigger_x, r_trigger_y;
    int start_x, start_y;
    int select_x, select_y;
    int deadzone;
} control_config;

static control_config cfg = {
    .joy_center_x = 75, .joy_center_y = 245, .joy_radius = 45,
    .cross_x = 420, .cross_y = 260,
    .circle_x = 370, .circle_y = 200,
    .square_x = 440, .square_y = 180,
    .triangle_x = 350, .triangle_y = 275,
    .l_trigger_x = 440, .l_trigger_y = 180,
    .r_trigger_x = 420, .r_trigger_y = 260,
    .start_x = 450, .start_y = 30,
    .select_x = 30, .select_y = 30,
    .deadzone = 40
};

static void load_controls_config(void) {
    FILE *f = fopen(DATA_PATH "controls.txt", "r");
    if (!f) {
        // Write default configuration file
        f = fopen(DATA_PATH "controls.txt", "w");
        if (f) {
            fprintf(f, "# Hero of Sparta II PS Vita Controls Configuration\n");
            fprintf(f, "JOY_CENTER_X=%d\nJOY_CENTER_Y=%d\nJOY_RADIUS=%d\n", cfg.joy_center_x, cfg.joy_center_y, cfg.joy_radius);
            fprintf(f, "CROSS_X=%d\nCROSS_Y=%d\n", cfg.cross_x, cfg.cross_y);
            fprintf(f, "CIRCLE_X=%d\nCIRCLE_Y=%d\n", cfg.circle_x, cfg.circle_y);
            fprintf(f, "SQUARE_X=%d\nSQUARE_Y=%d\n", cfg.square_x, cfg.square_y);
            fprintf(f, "TRIANGLE_X=%d\nTRIANGLE_Y=%d\n", cfg.triangle_x, cfg.triangle_y);
            fprintf(f, "L_X=%d\nL_Y=%d\n", cfg.l_trigger_x, cfg.l_trigger_y);
            fprintf(f, "R_X=%d\nR_Y=%d\n", cfg.r_trigger_x, cfg.r_trigger_y);
            fprintf(f, "START_X=%d\nSTART_Y=%d\n", cfg.start_x, cfg.start_y);
            fprintf(f, "DEADZONE=%d\n", cfg.deadzone);
            fclose(f);
        }
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "JOY_CENTER_X=%d", &v) == 1) cfg.joy_center_x = v;
        else if (sscanf(line, "JOY_CENTER_Y=%d", &v) == 1) cfg.joy_center_y = v;
        else if (sscanf(line, "JOY_RADIUS=%d", &v) == 1) cfg.joy_radius = v;
        else if (sscanf(line, "CROSS_X=%d", &v) == 1) cfg.cross_x = v;
        else if (sscanf(line, "CROSS_Y=%d", &v) == 1) cfg.cross_y = v;
        else if (sscanf(line, "CIRCLE_X=%d", &v) == 1) cfg.circle_x = v;
        else if (sscanf(line, "CIRCLE_Y=%d", &v) == 1) cfg.circle_y = v;
        else if (sscanf(line, "SQUARE_X=%d", &v) == 1) cfg.square_x = v;
        else if (sscanf(line, "SQUARE_Y=%d", &v) == 1) cfg.square_y = v;
        else if (sscanf(line, "TRIANGLE_X=%d", &v) == 1) cfg.triangle_x = v;
        else if (sscanf(line, "TRIANGLE_Y=%d", &v) == 1) cfg.triangle_y = v;
        else if (sscanf(line, "L_X=%d", &v) == 1) cfg.l_trigger_x = v;
        else if (sscanf(line, "L_Y=%d", &v) == 1) cfg.l_trigger_y = v;
        else if (sscanf(line, "R_X=%d", &v) == 1) cfg.r_trigger_x = v;
        else if (sscanf(line, "R_Y=%d", &v) == 1) cfg.r_trigger_y = v;
        else if (sscanf(line, "START_X=%d", &v) == 1) cfg.start_x = v;
        else if (sscanf(line, "DEADZONE=%d", &v) == 1) cfg.deadzone = v;
    }
    fclose(f);
    // Run 42: deadzone 12 < Vita stick drift -> permanent phantom touch on the joystick spot,
    // which held the menu highlight on "Más juegos" and opened IGP when it ended.
    if (cfg.deadzone < 40)
        cfg.deadzone = 40;
    l_info("Loaded custom controls from %s", DATA_PATH "controls.txt");
}

void input_init(void) {
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    memset(touches, 0, sizeof(touches));
    memset(prev_touches, 0, sizeof(prev_touches));
    touch_count = 0;
    prev_count = 0;
    load_controls_config();
    l_info("Input subsystem initialized (multitouch + physical buttons synthesized).");
}

static void add_touch(int id, int gx, int gy) {
    if (touch_count >= MAX_SYNTH_TOUCHES) return;
    touches[touch_count].id = id;
    touches[touch_count].gx = gx;
    touches[touch_count].gy = gy;
    // Map back to screen coords for consistency
    touches[touch_count].x = (gx * 816 / 480) + 72;
    touches[touch_count].y = gy * 544 / 320;
    touches[touch_count].down = 1;
    touch_count++;
}

void input_poll(void) {
    sceCtrlPeekBufferPositive(0, &pad, 1);

    SceTouchData t;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) < 1)
        t.reportNum = 0;

    memcpy(prev_touches, touches, sizeof(touches));
    prev_count = touch_count;
    touch_count = 0;

    // 1. Hardware front touch points
    int num_hw = (int)t.reportNum;
    if (num_hw > SCE_TOUCH_MAX_REPORT) num_hw = SCE_TOUCH_MAX_REPORT;
    for (int i = 0; i < num_hw; i++) {
        int x = t.report[i].x / 2;
        int y = t.report[i].y / 2;
        int gx = (x - 72) * 480 / 816;
        int gy = y * 320 / 544;
        gx = gx < 0 ? 0 : gx > 479 ? 479 : gx;
        gy = gy < 0 ? 0 : gy > 319 ? 319 : gy;
        add_touch(t.report[i].id, gx, gy);
    }

    // 2. Synthesize touches from physical buttons
    // Analog Stick & D-Pad: Virtual Joystick
    int lx = (int)pad.lx - 128;
    int ly = (int)pad.ly - 128;
    if (pad.buttons & SCE_CTRL_LEFT) lx = -120;
    if (pad.buttons & SCE_CTRL_RIGHT) lx = 120;
    if (pad.buttons & SCE_CTRL_UP) ly = -120;
    if (pad.buttons & SCE_CTRL_DOWN) ly = 120;

    if (abs(lx) > cfg.deadzone || abs(ly) > cfg.deadzone) {
        int jx = cfg.joy_center_x + (lx * cfg.joy_radius / 128);
        int jy = cfg.joy_center_y + (ly * cfg.joy_radius / 128);
        add_touch(100, jx, jy);
    }

    // Action buttons
    // Debug shortcut: L + R + TRIANGLE directly plays HoS_trailer.m4v
    static uint32_t s_last_btns = 0;
    uint32_t pressed = pad.buttons & ~s_last_btns;
    s_last_btns = pad.buttons;
    if ((pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) == (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER) &&
        (pressed & SCE_CTRL_TRIANGLE)) {
        l_info("[VIDEO] Debug shortcut L+R+TRIANGLE: playing HoS_trailer.m4v");
        video_play(BUNDLE_PATH "HoS_trailer.m4v");
    }

    if (pad.buttons & SCE_CTRL_CROSS) add_touch(101, cfg.cross_x, cfg.cross_y);
    if (pad.buttons & SCE_CTRL_CIRCLE) add_touch(102, cfg.circle_x, cfg.circle_y);
    if (pad.buttons & SCE_CTRL_SQUARE) add_touch(103, cfg.square_x, cfg.square_y);
    if (pad.buttons & SCE_CTRL_TRIANGLE) add_touch(104, cfg.triangle_x, cfg.triangle_y);
    if (pad.buttons & SCE_CTRL_LTRIGGER) add_touch(105, cfg.l_trigger_x, cfg.l_trigger_y);
    if (pad.buttons & SCE_CTRL_RTRIGGER) add_touch(106, cfg.r_trigger_x, cfg.r_trigger_y);
    if (pad.buttons & SCE_CTRL_START) add_touch(107, cfg.start_x, cfg.start_y);
    if (pad.buttons & SCE_CTRL_SELECT) add_touch(108, cfg.select_x, cfg.select_y);
}
