#pragma once

namespace inputnext {

// The registry owns one source object. Destruction runs during a lifecycle operation;
// derived destructors must not reenter that registry's lifecycle API.
class source_lifetime {
public:
    virtual ~source_lifetime() noexcept = default;

    source_lifetime(const source_lifetime&) = delete;
    source_lifetime& operator=(const source_lifetime&) = delete;
    source_lifetime(source_lifetime&&) = delete;
    source_lifetime& operator=(source_lifetime&&) = delete;

protected:
    source_lifetime() = default;
};

} // namespace inputnext
