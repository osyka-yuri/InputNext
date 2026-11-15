#pragma once

namespace inputnext::dinput {
class buffered_input;

namespace detail {

// Hooks run outside pool gates. Their context must outlive admitted calls and workers.
struct buffered_input_test_hooks final {
    void (*before_allocation)(void*){};
    void (*before_close)(void*) noexcept {};
    void* context{};
};

struct buffered_input_test_access final {
    static void set_hooks(buffered_input& input, buffered_input_test_hooks hooks) noexcept;
};

} // namespace detail
} // namespace inputnext::dinput
