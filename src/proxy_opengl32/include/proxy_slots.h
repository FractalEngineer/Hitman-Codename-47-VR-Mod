#pragma once

// The export thunk table. Defined in opengl32_exports.generated.cpp, which is
// produced by tools/gen_opengl32_proxy.py from the real import table.
//
// The tables use C linkage on purpose: MSVC's inline assembler resolves plain
// identifiers, not namespaced C++ symbols.

extern "C" {

// One slot per exported thunk, in the same order as h47vr_proxy_names.
extern void* h47vr_proxy_slots[];

// Export names, used to resolve the matching function in the system library.
extern const char* h47vr_proxy_names[];

// Number of entries in both tables.
extern int h47vr_proxy_count;

// Fills every slot from the system opengl32.dll. Idempotent and thread safe.
// Called by the thunks on first use, so it never runs under the loader lock.
extern void h47vr_proxy_resolve(void);

// Indirection so the generated thunks can call the resolver without depending
// on how the linker decorates function names.
extern void (*h47vr_proxy_resolve_fn)(void);

}  // extern "C"
