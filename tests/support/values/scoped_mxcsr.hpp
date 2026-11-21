#pragma once

#include <xmmintrin.h>

namespace inputnext::test {

class scoped_mxcsr final {
public:
    explicit scoped_mxcsr(unsigned int control) noexcept : previous_(_mm_getcsr()) {
        _mm_setcsr(control);
    }

    ~scoped_mxcsr() { _mm_setcsr(previous_); }

    scoped_mxcsr(const scoped_mxcsr&) = delete;
    scoped_mxcsr& operator=(const scoped_mxcsr&) = delete;
    scoped_mxcsr(scoped_mxcsr&&) = delete;
    scoped_mxcsr& operator=(scoped_mxcsr&&) = delete;

private:
    unsigned int previous_;
};

} // namespace inputnext::test
