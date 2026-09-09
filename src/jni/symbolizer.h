#pragma once
#include <stdint.h>
#include <stddef.h>

// Символизация адресов внутри libDamnWrapper32.so прямо на устройстве:
// адрес -> "src/jni/main.cpp:1303 (CrashHandler)". Читает DWARF (.debug_line)
// и .symtab из файла самой библиотеки, без malloc и без внешних утилит.

bool SymInit();
const char* SymStatus();
bool SymReady();
bool SymOwnsAddress(uintptr_t runtimeAddr);

// Пакетное разрешение: адреса сначала складываются, потом один проход по DWARF.
void SymBatchReset();
void SymBatchAdd(uintptr_t runtimeAddr, bool isReturnAddress);
void SymBatchResolve();

// "" если адрес не наш или строка не найдена, иначе " >>> src/jni/main.cpp:1303 (Func)"
const char* SymSuffix(uintptr_t runtimeAddr, bool isReturnAddress);
