/**
 * @file objc.h
 * @brief Objective-C selector dispatch runtime and on-screen keyboard IME bridge.
 */
#pragma once
#include <stdint.h>

#include "input.h"

void objc_init(void);
void *objc_find_class(const char *name);
void *objc_class_alloc(void *cls);
uintptr_t objc_get_imp(void *cls, const char *sel_name);
int objc_has_method(void *cls, const char *sel_name);
const char *ns_cstr(const void *o);
void ime_poll(void);
void objc_pump(void);
int ime_active(void);
/** @brief Diff current touches against the previous call and send touchesBegan/Moved/Ended to EAGLView. */
void objc_deliver_touches(const touch_point *cur, int ncur);
