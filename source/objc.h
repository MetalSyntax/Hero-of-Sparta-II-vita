/**
 * @file objc.h
 * @brief Objective-C selector dispatch runtime and on-screen keyboard IME bridge.
 */
#pragma once
#include <stdint.h>

void objc_init(void);
void *objc_find_class(const char *name);
void *objc_class_alloc(void *cls);
uintptr_t objc_get_imp(void *cls, const char *sel_name);
int objc_has_method(void *cls, const char *sel_name);
void ime_poll(void);
int ime_active(void);
