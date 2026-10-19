#pragma once

#include "history_evidence.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/history.hpp>

#include <string>
#include <string_view>

namespace inputnext::gameinput_detail {

[[nodiscard]] std::string format_history_batch(connection_target target,
    const history_batch& batch,
    const history_evidence& evidence);
[[nodiscard]] std::string format_history_record(history_domain domain,
    const history_record& record);
[[nodiscard]] std::string format_history_operation(std::string_view operation,
    connection_target target,
    const history_result<void>& result);

} // namespace inputnext::gameinput_detail
