// wv2_shims.cpp — CRT/STL shims for linking the MSVC-built
// WebView2LoaderStatic.lib into a mingw-w64 static CRT build.
//
// The loader objects were compiled with MSVC /GS and the MSVC STL, so they
// reference symbols the mingw static CRT does not export. Each shim below is
// a faithful, minimal stand-in for what the loader uses.
//
//   __security_cookie / __security_check_cookie
//     MSVC /GS stack-cookie. The loader stores the cookie in a local at
//     function entry and passes it back at exit; we compare against the
//     global. Mismatch => abort (same behavior as the real MSVC handler).
//   ?__libcpp_verbose_abort@__Cr@std@@YAXPEBDZZ
//     MSVC STL internal called on fatal string errors (e.g. out_of_range
//     in operator+=). No recovery possible: report and terminate.
//   ?nothrow@std@@3Unothrow_t@1@B
//     MSVC STL: std::nothrow is a 1-byte global object.
//   _Init_thread_header / _Init_thread_footer / _Init_thread_epoch
//     MSVC CRT one-time initialization helpers used by the loader's
//     internal std::call_once-style statics. Trivial header/footer + epoch
//     0 is behaviorally equivalent for one-time static setup.
//
// HEAP STRATEGY — everything on the MSVC heap (msvcrt.dll):
//   The loader mixes C heap funcs (malloc/free/realloc) with C++ operators
//   (operator new/delete, nothrow-new, sized-delete). If those land on two
//   different CRT heaps, an allocation from one freed by the other is a
//   cross-CRT free that corrupts memory seconds into the run.
//
//   Empirically msvcrt.dll exports the plain operator new (??_U), operator
//   delete (??_V) and C malloc/free/realloc, but NOT the two C++14/C++11
//   variants the loader references:
//     ??2@YAPEAX_KAEBUnothrow_t@std@@@Z   operator new(size_t, nothrow_t&)
//     ??3@YAXPEAX_K@Z                     operator delete(void*, size_t)
//
//   So C + plain C++ allocation all auto-import from msvcrt.dll (MSVC heap,
//   self-consistent), and we provide ONLY the two missing variants, each
//   delegating to msvcrt's own plain operator new/delete. One heap, no
//   recursion, no cross-CRT free.
//
// MSVC-mangled names carry a `?` that gas reads as a comment, so they only
// ever appear inside quoted asm strings. We reference msvcrt's allocators
// through plain-C aliases (wv2_ms_new / wv2_ms_del) so the `call` operands
// never contain a `?`.

#include <cstdlib>

extern "C" {

unsigned char __security_cookie[8] = { 0x47, 0x41, 0x47, 0x45, 0x2D, 0x4D, 0x50, 0x01 };

void __security_check_cookie(unsigned long long cookie) {
    unsigned long long stored = 0;
    for (int i = 0; i < 8; ++i) stored = (stored << 8) | __security_cookie[i];
    if (cookie != stored) std::abort();
}

// MSVC uses a leading underscore on these names (x64 CRT C linkage).
// _Thrd_header is an 8-byte counter (init 0 -> caller sets 1 after the
// one-time block -> footer validates), so operate on the full 64 bits.
unsigned long long _Init_thread_epoch = 0;
void _Init_thread_header(unsigned long long* out) { if (out) *out = 0; }
void _Init_thread_footer(unsigned long long* flag) {
    // Tolerate 0 (block skipped) and 1 (block ran); real MSVC would abort.
    if (flag && *flag == 0) *flag = 1;
}

// MSVC STL: std::nothrow is a 1-byte global object.
unsigned char wv2_nothrow_obj = 0;

// msvcrt.dll's plain operator new/delete, reached through plain-C aliases
// (defined by the asm block below). These land on the MSVC heap.
extern "C" void* wv2_ms_new(unsigned long long size);
extern "C" void  wv2_ms_delete(void* p);

// operator new(size_t, const std::nothrow_t&) — delegate to the MSVC heap.
// Under the loader's usage a null-return on OOM is the contract; MSVC's
// plain new is a faithful, same-heap stand-in for its one-shot allocs.
extern "C" void* wv2_new_nothrow(unsigned long long size) {
    return wv2_ms_new(size);
}

// operator delete(void*, size_t) — delegate to the MSVC heap.
extern "C" void wv2_sized_delete(void* p, unsigned long long size) {
    (void)size;
    wv2_ms_delete(p);
}

}  // extern "C"

// MSVC-mangled: std::__Cr::__libcpp_verbose_abort(const char*, ...)
extern "C" [[noreturn]] void wv2_libcpp_verbose_abort(const char*, ...) {
    std::abort();
}

// Emit the exact MSVC-mangled names the loader references. Quoted gas
// symbols survive the `?`. wv2_ms_new / wv2_ms_del alias to msvcrt's own
// plain operator new/delete (auto-imported from msvcrt.dll), so the two
// missing C++14/C++11 variants route back onto the same MSVC heap.
asm(
    // plain operator new/delete (msvcrt) -> plain-C names for safe `call`s
    ".globl wv2_ms_new\n\t"
    ".set wv2_ms_new, \"??_U@YAPEAX_K@Z\"\n\t"
    ".globl wv2_ms_delete\n\t"
    ".set wv2_ms_delete, \"??_V@YAXPEAX@Z\"\n\t"
    // the non-heap shims
    ".globl \"?__libcpp_verbose_abort@__Cr@std@@YAXPEBDZZ\"\n\t"
    ".set \"?__libcpp_verbose_abort@__Cr@std@@YAXPEBDZZ\", wv2_libcpp_verbose_abort\n\t"
    ".globl \"?nothrow@std@@3Unothrow_t@1@B\"\n\t"
    ".set \"?nothrow@std@@3Unothrow_t@1@B\", wv2_nothrow_obj\n\t"
    // the two C++14/C++11 allocators msvcrt lacks, routed to the MSVC heap
    ".globl \"??2@YAPEAX_KAEBUnothrow_t@std@@@Z\"\n\t"
    ".set \"??2@YAPEAX_KAEBUnothrow_t@std@@@Z\", wv2_new_nothrow\n\t"
    ".globl \"??3@YAXPEAX_K@Z\"\n\t"
    ".set \"??3@YAXPEAX_K@Z\", wv2_sized_delete\n\t"
);
