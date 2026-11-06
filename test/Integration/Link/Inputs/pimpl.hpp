// For pimpl.cpp: the C++ pimpl idiom.
#pragma once
class Impl;
class Widget {
public:
    Widget();
    int value() const;
private:
    int id_ = 1;
    Impl *impl_;
};
