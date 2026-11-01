#include "gameinput_probe/input/history/report.hpp"

#include "gameinput_probe/input/history/evidence.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/history.hpp>

#include <ios>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] std::string_view error_name(history_error error) noexcept {
    switch (error) {
    case history_error::invalid_argument:
        return "invalid_argument";
    case history_error::invalid_reader:
        return "invalid_reader";
    case history_error::closed:
        return "closed";
    case history_error::disconnected:
        return "disconnected";
    case history_error::busy:
        return "busy";
    case history_error::capacity_exceeded:
        return "capacity_exceeded";
    case history_error::identity_exhausted:
        return "identity_exhausted";
    case history_error::sequence_exhausted:
        return "sequence_exhausted";
    case history_error::no_data:
        return "no_data";
    case history_error::history_lost:
        return "history_lost";
    case history_error::provider_failure:
        return "provider_failure";
    case history_error::clock_discontinuity:
        return "clock_discontinuity";
    }
    return "unknown";
}

[[nodiscard]] std::string_view loss_name(history_loss loss) noexcept {
    switch (loss) {
    case history_loss::none:
        return "none";
    case history_loss::known:
        return "known";
    case history_loss::unknown:
        return "unknown";
    }
    return "invalid";
}

[[nodiscard]] std::string_view reset_name(history_reset reset) noexcept {
    switch (reset) {
    case history_reset::none:
        return "none";
    case history_reset::initial_open:
        return "initial_open";
    case history_reset::history_gap:
        return "history_gap";
    }
    return "invalid";
}

[[nodiscard]] std::string_view origin_name(history_record_origin origin) noexcept {
    switch (origin) {
    case history_record_origin::baseline:
        return "baseline";
    case history_record_origin::native_pad:
        return "native_pad";
    case history_record_origin::callback_pad:
        return "callback_pad";
    case history_record_origin::callback_guide:
        return "callback_guide";
    }
    return "invalid";
}

void append_failure(std::ostream& output, history_failure failure) {
    output << " error=" << error_name(failure.code) << " native_code=0x" << std::hex
           << failure.native_code << std::dec;
}

} // namespace

std::string format_history_batch(connection_target target,
    const history_batch& batch,
    const history_evidence& evidence) {
    std::ostringstream output;
    output << std::boolalpha << "history key=" << target.device.value
           << " connection=" << target.connection.value << " stream=" << batch.domain.stream
           << " source="
           << (batch.source == history_source_kind::native_history ? "native_history"
                                                                   : "callback_capture")
           << " records=" << batch.count << " first=" << batch.first_sequence
           << " last=" << batch.last_sequence
           << " checkpoint_sequence=" << batch.checkpoint_sequence
           << " checkpoint_qpc=" << batch.checkpoint_qpc << " caught_up=" << batch.caught_up
           << " reset=" << reset_name(batch.reset) << " loss=" << loss_name(batch.loss)
           << " lost_count=" << batch.lost_count << " readings_total=" << evidence.readings
           << " native_readings=" << evidence.native_readings << " baselines=" << evidence.baselines
           << " resets=" << evidence.resets << " failures=" << evidence.failures
           << " native_history_observed=" << evidence.native_history_observed();
    if (batch.failure.has_value()) {
        append_failure(output, batch.failure.value_or(history_failure{}));
    }
    return output.str();
}

std::string format_history_record(history_domain domain, const history_record& record) {
    std::ostringstream output;
    output << std::boolalpha << "history_state key=" << domain.target.device.value
           << " connection=" << domain.target.connection.value << " stream=" << domain.stream
           << " sequence=" << record.sequence << " baseline=" << record.baseline
           << " source_us=" << record.sample.source_timestamp_us
           << " qpc=" << record.sample.effective_qpc << " buttons=0x" << std::hex
           << record.sample.state.buttons << std::dec
           << " guide_known=" << record.sample.guide_known
           << " origin=" << origin_name(record.origin);
    return output.str();
}

std::string format_history_operation(std::string_view operation,
    connection_target target,
    const history_result<void>& result) {
    std::ostringstream output;
    output << "history_" << operation << " key=" << target.device.value
           << " connection=" << target.connection.value << " result=" << (result ? "ok" : "failed");
    if (!result.has_value()) {
        append_failure(output, result.error());
    }
    return output.str();
}

} // namespace inputnext::gameinput_detail
