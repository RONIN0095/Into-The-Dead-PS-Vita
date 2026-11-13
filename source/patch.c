/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Hooks into libunity.so/libil2cpp.so.
 *
 * Neither library uses the ARM kuser helpers (checked: no 0xFFFF0FC0 /
 * 0xFFFF0FA0 literals), so unlike older ports nothing needs patching for
 * atomics. Game-specific hooks (input through CrossPlatformInputManager,
 * see docs/NOTES.md) go here.
 */

#include <so_util/so_util.h>

#include "na_modules.h"
#include "utils/logger.h"

void so_patch(void) {
    // Example:
    // uintptr_t f = na_module_symbol(NA_LIB_IL2CPP, "il2cpp_gc_disable");
    // hook_addr(f, (uintptr_t)&my_function);
    for (int i = 0; i < NA_LIB_COUNT; i++)
        if (na_libs[i].loaded) so_flush_caches(&na_libs[i].mod);
}
