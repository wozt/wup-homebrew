/* C++ on the GamePad: the runtime pieces a program needs that libc++'s headers do not have (they live in libc++ and
 * libc++abi, which the pad does not carry): operator new and delete over malloc, guarded static locals, a pure virtual
 * call, libc++'s abort hook, and std::string's members (libc++ declares them compiled in its library: they are compiled
 * here instead). No exceptions and no RTTI (tools/wupbuild.py: -fno-exceptions -fno-rtti): a failed new aborts. */
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <new>
#include <string>

extern "C" void wup_log(const char *);

namespace std { const nothrow_t nothrow{}; }           /* (new (std::nothrow): the object libc++abi would hold) */
static void *alloc(std::size_t n) {
    void *p = std::malloc(n ? n : 1);
    if (!p) { wup_log("c++: out of memory (new)\n"); std::abort(); }
    return p;
}
void *operator new(std::size_t n) { return alloc(n); }
void *operator new[](std::size_t n) { return alloc(n); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept { return std::malloc(n ? n : 1); }
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { return std::malloc(n ? n : 1); }
void *operator new(std::size_t n, std::align_val_t a) { void *p = aligned_alloc((std::size_t)a, n ? n : 1); if (!p) std::abort(); return p; }
void *operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }

/* static locals: initialised once (the Itanium ABI's guard: its first byte, 1 once done). A program's own threads
   might race for one; libwup's are cooperative enough that the simple form serves */
extern "C" int __cxa_guard_acquire(long long *g) { return *(volatile char *)g == 0; }
extern "C" void __cxa_guard_release(long long *g) { *(volatile char *)g = 1; }
extern "C" void __cxa_guard_abort(long long *) {}
extern "C" void __cxa_pure_virtual() { wup_log("c++: pure virtual call\n"); std::abort(); }

namespace std { inline namespace __1 {
[[noreturn]] void __libcpp_verbose_abort(const char *fmt, ...) noexcept {
    char b[160]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    wup_log("c++: "); wup_log(b); wup_log("\n");
    abort();
}
template class basic_string<char>;

/* compiled in libc++'s library too: std::sort on the built-in types, and to_string */
template <class _Comp, class _RandomAccessIterator>   /* (libc++ declares it; its library defines it: heap sort here,
                                                         O(n log n) always, from the headers' own templates) */
void __sort(_RandomAccessIterator f, _RandomAccessIterator l, _Comp) {   /* (only __less instantiations: a < b) */
    auto lt = [](const auto &a, const auto &b) { return a < b; };
    std::make_heap(f, l, lt); std::sort_heap(f, l, lt);
}
#define SORT_(T) template void __sort<__less<T>&, T*>(T*, T*, __less<T>&);
SORT_(char) SORT_(signed char) SORT_(unsigned char) SORT_(short) SORT_(unsigned short) SORT_(int) SORT_(unsigned)
SORT_(long) SORT_(unsigned long) SORT_(long long) SORT_(unsigned long long) SORT_(float) SORT_(double) SORT_(long double)
#undef SORT_
static string fmt_(const char *f, ...) { char b[48]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); return string(b); }
string to_string(int v) { return fmt_("%d", v); }
string to_string(unsigned v) { return fmt_("%u", v); }
string to_string(long v) { return fmt_("%ld", v); }
string to_string(unsigned long v) { return fmt_("%lu", v); }
string to_string(long long v) { return fmt_("%lld", v); }
string to_string(unsigned long long v) { return fmt_("%llu", v); }
string to_string(float v) { return fmt_("%f", (double)v); }
string to_string(double v) { return fmt_("%f", v); }
string to_string(long double v) { return fmt_("%Lf", v); }                    /* (libc++ declares these "compiled in the library") */
template string operator+ <char, char_traits<char>, allocator<char> >(const char *, const string &);
} }
