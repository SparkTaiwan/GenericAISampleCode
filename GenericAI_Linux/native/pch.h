#ifndef PCH_H
#define PCH_H

// Linux build: no precompiled header and no <windows.h>. Every .cpp still
// starts with #include "pch.h" (kept identical to the Windows sources), so
// this header only carries the portability macros the ABI needs.
#include "gai_platform.h"

#endif // PCH_H
