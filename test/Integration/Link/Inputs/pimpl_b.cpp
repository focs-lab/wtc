// For pimpl.cpp: the implementation.
#include "pimpl.hpp"
class Impl { public: int v = 41; };
Widget::Widget() : impl_(new Impl) {}
int Widget::value() const { return id_ + impl_->v; }
