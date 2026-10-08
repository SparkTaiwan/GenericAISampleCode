#pragma once

// Portability shims for the GAI_* C ABI. The Windows build exports the ABI
// from GenericAI.Native.dll with __declspec(dllexport)/__cdecl and declares
// the callbacks __stdcall; on Linux x86-64 there is a single calling
// convention and the native code is linked straight into the GenericAI
// executable, so all three collapse to nothing / default visibility.
#if defined(_WIN32)
#define GAI_EXPORT __declspec(dllexport)
#define GAI_CDECL  __cdecl
#define GAI_STDCALL __stdcall
#else
#define GAI_EXPORT __attribute__((visibility("default")))
#define GAI_CDECL
#define GAI_STDCALL
#endif
