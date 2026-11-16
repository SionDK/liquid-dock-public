#pragma once
// A value that costs a call into Windows to read but is wanted every frame (a tooltip's text, say):
// read again at most every `period` seconds, at once after Invalidate, and at once should the clock
// go back. One thread only (the UI thread where it is used). No Windows headers:
// tests/refreshed_test.cpp, run by tests/run_portable.sh.
#include <utility>

template <class T>
class Refreshed {
public:
    template <class Read>
    const T& Get(double now, double period, Read&& read) {
        if (!valid_ || now - at_ >= period || now < at_) {
            value_ = std::forward<Read>(read)();
            at_ = now;
            valid_ = true;
        }
        return value_;
    }
    void Invalidate() { valid_ = false; }

private:
    T value_{};
    double at_ = 0;
    bool valid_ = false;
};
