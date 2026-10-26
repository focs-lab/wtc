/* rc_hooks.h: the thread census's reference-count hooks for a program built
 * by an ordinary compiler, without wtc-cc --census.
 *
 * Include it before any standard header (clang++ -include rc_hooks.h). It
 * defines libstdc++'s race-detector macros, which std::_Sp_counted_base
 * invokes on every release of a shared_ptr (on the use count) and before
 * destroying a control block (on the weak count), to call into the census
 * library (threadcensus.c, wtc_rc_before and wtc_rc_after). The other
 * reference counts that use the same macros (locale facets, the
 * copy-on-write string, ios_base's callbacks) are filtered out at compile
 * time by the name of the enclosing function. Copies are not seen this
 * way; the library's comment says what that does to the numbers.
 *
 * The program must run with the census library preloaded: the hooks are
 * weak, so a run without it calls nothing.
 */
#ifndef WTC_CENSUS_RC_HOOKS_H
#define WTC_CENSUS_RC_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif
__attribute__((weak)) void wtc_rc_before(const void* ctrl, long offset);
__attribute__((weak)) void wtc_rc_after(const void* ctrl, long offset);
#ifdef __cplusplus
}

namespace wtc_census {
// Whether a function's pretty name is one of _Sp_counted_base's.
constexpr bool in_counted_base(const char* s) {
    constexpr const char* needle = "_Sp_counted_base<";
    for (; *s; ++s) {
        const char* a = s;
        const char* b = needle;
        while (*b && *a == *b) ++a, ++b;
        if (!*b) return true;
    }
    return false;
}
} // namespace wtc_census

#define WTC_CENSUS_RC_CALL(fn, A)                                                                \
    do {                                                                                         \
        if constexpr (::wtc_census::in_counted_base(__PRETTY_FUNCTION__))                        \
            if (fn)                                                                              \
                fn(this, (long)((const char*)(A) - (const char*)this));                          \
    } while (0)

#define _GLIBCXX_SYNCHRONIZATION_HAPPENS_BEFORE(A) WTC_CENSUS_RC_CALL(wtc_rc_before, A)
#define _GLIBCXX_SYNCHRONIZATION_HAPPENS_AFTER(A) WTC_CENSUS_RC_CALL(wtc_rc_after, A)
#endif

#endif
