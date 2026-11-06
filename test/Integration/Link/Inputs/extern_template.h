// For extern_template.cpp.
#pragma once
template <class T> struct Box { T v; __attribute__((noinline)) T get() const { return v + 1; } };
