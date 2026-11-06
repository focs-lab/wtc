// For inline_init.cpp: an inline function with a local static, and an inline
// variable with a dynamic initialiser that counts its runs, used by two files.
#pragma once
#include <cstdio>
inline int &counter() { static int c = 0; return c; }
inline int runs = 0;
struct Noisy { int v; Noisy() : v(++runs) {} };
inline Noisy shared_noisy;
template <class T> struct Holder { static Noisy n; };
template <class T> Noisy Holder<T>::n;
int bump_b();
