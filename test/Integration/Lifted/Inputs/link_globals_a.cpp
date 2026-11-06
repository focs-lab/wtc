// The other file of link_globals.cpp.
#include <string>

static int hits;
namespace {
struct K {
    int v;
};
K k{1};
}  // namespace
std::string name = "a";
inline int shared() { return 7; }
template <class T> T twice(T x) { return x + x; }
int a_hits() { return hits + shared() + twice(1) + k.v + static_cast<int>(name.size()); }
