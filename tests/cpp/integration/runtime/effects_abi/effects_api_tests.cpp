#include "effects_api_test_support.hpp"
#include "runtime/abi/runtime_api.hpp"
#include "runtime_api_test_composition.hpp"
#include "values/required_value.hpp"

#include <catch2/catch_test_macros.hpp>

#include <input_next_core.h>
#include <input_next_effects.h>

#include <algorithm>

namespace {

using namespace inputnext::effects_abi_test_support;
using inputnext::runtime_detail::runtime_api_test_fixture;
using inputnext::test::required_value;

[[nodiscard]] bool valid_effects_table(const input_next_effects_api_v1& api) noexcept {
    return api.struct_size == sizeof(api) && api.abi_version == INPUT_NEXT_EFFECTS_ABI_VERSION &&
           api.resolve_target != nullptr && api.open_context != nullptr &&
           api.query_context != nullptr && api.submit != nullptr && api.clear != nullptr &&
           api.submit_body != nullptr && api.clear_body != nullptr &&
           api.close_context != nullptr && api.ack_close != nullptr &&
           api.query_progress != nullptr;
}

[[nodiscard]] input_next_effects_close_receipt_v1 valid_close_receipt_for_test() {
    input_next_effects_close_receipt_v1 receipt{};
    receipt.struct_size = sizeof(receipt);
    receipt.disposition = INPUT_NEXT_EFFECTS_CLOSE_NORMAL;
    receipt.context = test_context();
    receipt.target = test_target();
    receipt.close_request_id = receipt.context.id;
    receipt.publication_revision = 1U;
    return receipt;
}

TEST_CASE("runtime.effects.query_is_lazy_shared_and_exposes_all_ten_callbacks",
    "[integration][runtime][effects][abi][bootstrap]") {
    runtime_api_test_fixture fixture;

    CHECK(input_next_runtime_query_effects_api(0U) == nullptr);
    CHECK(input_next_runtime_query_effects_api(2U) == nullptr);
    CHECK(fixture.pin_calls() == 0U);
    CHECK(fixture.owner_creations() == 0U);

    const auto* const effects_api =
        input_next_runtime_query_effects_api(INPUT_NEXT_EFFECTS_ABI_VERSION);
    REQUIRE(effects_api != nullptr);
    CHECK(input_next_runtime_query_effects_api(INPUT_NEXT_EFFECTS_ABI_VERSION) == effects_api);
    CHECK(valid_effects_table(*effects_api));

    const auto* const core_api = input_next_runtime_query_core_api(INPUT_NEXT_CORE_ABI_VERSION);
    const auto* const xinput_api = input_next_runtime_query_api(1U);
    CHECK(core_api != nullptr);
    CHECK(xinput_api != nullptr);
    CHECK(fixture.pin_calls() == 1U);
    CHECK(fixture.owner_creations() == 1U);
    CHECK(fixture.focus_observations() == 0U);
    CHECK(fixture.prepare_attempts() == 0U);
    CHECK_FALSE(fixture.native_initialized());
}

TEST_CASE("runtime.effects.invalid_and_retained_queries_preserve_outputs_while_cold",
    "[integration][runtime][effects][abi][validation]") {
    runtime_api_test_fixture fixture;
    const auto* const api = input_next_runtime_query_effects_api(INPUT_NEXT_EFFECTS_ABI_VERSION);
    REQUIRE(api != nullptr);

    auto selector = selector_for(test_target());
    selector.struct_size = 0U;
    auto target = test_target({.device_key = 0xAAU, .connection_id = 0xBBU});
    input_next_effects_capabilities_v1 capabilities{};
    initialize_capabilities_output(capabilities);
    capabilities.feature_flags = 0xA5A5U;
    CHECK(api->resolve_target(&selector, &target, &capabilities) ==
          INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(target.device_key == 0xAAU);
    CHECK(target.connection_id == 0xBBU);
    CHECK(capabilities.feature_flags == 0xA5A5U);

    input_next_effects_open_v1 open{};
    open.struct_size = sizeof(open);
    open.flags = 1U;
    open.target = test_target();
    input_next_effects_context_v1 opened{};
    initialize_context_output(opened);
    opened.id = 0x1234U;
    CHECK(api->open_context(&open, &opened) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(opened.id == 0x1234U);

    auto request = rumble_request(test_context());
    request.ttl_ms = 0U;
    input_next_effects_receipt_v1 receipt{};
    initialize_receipt_output(receipt);
    receipt.accepted_mask = 0xCAFEBABEU;
    receipt.publication_revision = 0x1122334455667788ULL;
    CHECK(api->submit(&request, &receipt) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(receipt.accepted_mask == 0xCAFEBABEU);
    CHECK(receipt.publication_revision == 0x1122334455667788ULL);

    auto body = body_keep_request(test_context());
    body.pcm.struct_size = 0U;
    input_next_effects_body_receipt_v1 body_receipt{};
    initialize_body_receipt_output(body_receipt);
    body_receipt.lease_id = 0x12345678U;
    CHECK(api->submit_body(&body, &body_receipt) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(body_receipt.lease_id == 0x12345678U);

    const auto context = test_context();
    input_next_effects_context_info_v1 info{};
    initialize_context_info_output(info);
    info.state = 0xA5U;
    CHECK(api->query_context(&context, &info) == INPUT_NEXT_STATUS_UNAVAILABLE);
    CHECK(info.state == 0xA5U);

    request = rumble_request(context);
    initialize_receipt_output(receipt);
    receipt.accepted_mask = 0x13579BDFU;
    CHECK(api->submit(&request, &receipt) == INPUT_NEXT_STATUS_UNAVAILABLE);
    CHECK(receipt.accepted_mask == 0x13579BDFU);

    input_next_effects_clear_v1 clear{};
    clear.struct_size = sizeof(clear);
    clear.channel_mask = INPUT_NEXT_EFFECTS_RUMBLE;
    clear.context = context;
    clear.mutation_sequence = 1U;
    CHECK(api->clear(&clear) == INPUT_NEXT_STATUS_UNAVAILABLE);

    body = body_keep_request(context);
    initialize_body_receipt_output(body_receipt);
    body_receipt.lease_id = 0x87654321U;
    CHECK(api->submit_body(&body, &body_receipt) == INPUT_NEXT_STATUS_UNAVAILABLE);
    CHECK(body_receipt.lease_id == 0x87654321U);

    input_next_effects_body_clear_v1 clear_body{};
    clear_body.struct_size = sizeof(clear_body);
    clear_body.context = context;
    clear_body.lease_id = 1U;
    clear_body.sequence = 1U;
    CHECK(api->clear_body(&clear_body) == INPUT_NEXT_STATUS_UNAVAILABLE);

    input_next_effects_close_v1 close{};
    close.struct_size = sizeof(close);
    close.context = context;
    close.close_request_id = context.id;
    input_next_effects_close_receipt_v1 close_receipt{};
    initialize_close_receipt_output(close_receipt);
    close_receipt.publication_revision = 0x99U;
    CHECK(api->close_context(&close, &close_receipt) == INPUT_NEXT_STATUS_UNAVAILABLE);
    CHECK(close_receipt.publication_revision == 0x99U);

    const auto forged_receipt = valid_close_receipt_for_test();
    CHECK(api->ack_close(&forged_receipt) == INPUT_NEXT_STATUS_UNAVAILABLE);

    input_next_effects_progress_query_v1 query{};
    query.struct_size = sizeof(query);
    query.target = test_target();
    query.publication_revision = 1U;
    input_next_effects_progress_v1 progress{};
    initialize_progress_output(progress);
    progress.stage = 0xA5U;
    CHECK(api->query_progress(&query, &progress) == INPUT_NEXT_STATUS_UNAVAILABLE);
    CHECK(progress.stage == 0xA5U);

    CHECK(fixture.prepare_attempts() == 0U);
    CHECK_FALSE(fixture.native_initialized());
}

TEST_CASE("runtime.effects.wrong_outer_sizes_are_rejected_before_input_copy_or_bootstrap",
    "[integration][runtime][effects][abi][validation]") {
    runtime_api_test_fixture fixture;
    const auto* const api = input_next_runtime_query_effects_api(INPUT_NEXT_EFFECTS_ABI_VERSION);
    REQUIRE(api != nullptr);

    auto selector = selector_for(test_target());
    selector.struct_size = 0U;
    auto target = test_target({.device_key = 0xAAU, .connection_id = 0xBBU});
    input_next_effects_capabilities_v1 capabilities{};
    initialize_capabilities_output(capabilities);
    capabilities.feature_flags = 0xA5A5U;
    CHECK(api->resolve_target(&selector, &target, &capabilities) ==
          INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(target.device_key == 0xAAU);
    CHECK(capabilities.feature_flags == 0xA5A5U);

    input_next_effects_open_v1 open{};
    open.struct_size = 0U;
    input_next_effects_context_v1 opened{};
    initialize_context_output(opened);
    opened.id = 0x1234U;
    CHECK(api->open_context(&open, &opened) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(opened.id == 0x1234U);

    auto context = test_context();
    context.struct_size = 0U;
    input_next_effects_context_info_v1 info{};
    initialize_context_info_output(info);
    info.state = 0xA5U;
    CHECK(api->query_context(&context, &info) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(info.state == 0xA5U);

    auto request = rumble_request(test_context());
    request.struct_size = 0U;
    input_next_effects_receipt_v1 receipt{};
    initialize_receipt_output(receipt);
    receipt.accepted_mask = 0xCAFEBABEU;
    CHECK(api->submit(&request, &receipt) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(receipt.accepted_mask == 0xCAFEBABEU);

    input_next_effects_clear_v1 clear{};
    clear.struct_size = 0U;
    CHECK(api->clear(&clear) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);

    auto body = body_keep_request(test_context());
    body.struct_size = 0U;
    input_next_effects_body_receipt_v1 body_receipt{};
    initialize_body_receipt_output(body_receipt);
    body_receipt.lease_id = 0x12345678U;
    CHECK(api->submit_body(&body, &body_receipt) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(body_receipt.lease_id == 0x12345678U);

    input_next_effects_body_clear_v1 body_clear{};
    body_clear.struct_size = 0U;
    CHECK(api->clear_body(&body_clear) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);

    input_next_effects_close_v1 close{};
    close.struct_size = 0U;
    input_next_effects_close_receipt_v1 close_receipt{};
    initialize_close_receipt_output(close_receipt);
    close_receipt.publication_revision = 0x99U;
    CHECK(api->close_context(&close, &close_receipt) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(close_receipt.publication_revision == 0x99U);

    auto ack = valid_close_receipt_for_test();
    ack.struct_size = 0U;
    CHECK(api->ack_close(&ack) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);

    input_next_effects_progress_query_v1 query{};
    query.struct_size = 0U;
    input_next_effects_progress_v1 progress{};
    initialize_progress_output(progress);
    progress.stage = 0xA5U;
    CHECK(api->query_progress(&query, &progress) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(progress.stage == 0xA5U);

    CHECK(fixture.prepare_attempts() == 0U);
    CHECK_FALSE(fixture.native_initialized());
}

TEST_CASE("runtime.effects.actual_table_shares_core_and_commits_rumble_body_close_and_ack",
    "[integration][runtime][effects][abi]") {
    runtime_api_test_fixture fixture;
    const auto* const api = input_next_runtime_query_effects_api(INPUT_NEXT_EFFECTS_ABI_VERSION);
    const auto* const core = input_next_runtime_query_core_api(INPUT_NEXT_CORE_ABI_VERSION);
    REQUIRE(api != nullptr);
    REQUIRE(core != nullptr);

    input_next_runtime_info_v1 info{};
    info.struct_size = sizeof(info);
    REQUIRE(core->get_runtime_info(&info) == INPUT_NEXT_STATUS_OK);
    REQUIRE(fixture.owner_is_ready());
    CHECK(info.max_effect_contexts == 32U);
    fixture.connect_native_device();
    const auto discovered_target = required_value(wait_for_device(*core));

    const auto selector = selector_for(discovered_target);
    auto target = discovered_target;
    input_next_effects_capabilities_v1 capabilities{};
    initialize_capabilities_output(capabilities);
    REQUIRE(api->resolve_target(&selector, &target, &capabilities) == INPUT_NEXT_STATUS_OK);
    CHECK(target.device_key == discovered_target.device_key);
    CHECK((capabilities.supported_channels & INPUT_NEXT_EFFECTS_RUMBLE) != 0U);
    CHECK(capabilities.capabilities_revision != 0U);

    input_next_effects_open_v1 open{};
    open.struct_size = sizeof(open);
    open.target = target;
    input_next_effects_context_v1 context{};
    initialize_context_output(context);
    REQUIRE(api->open_context(&open, &context) == INPUT_NEXT_STATUS_OK);
    REQUIRE(context.id != 0U);

    input_next_effects_context_info_v1 context_info{};
    initialize_context_info_output(context_info);
    REQUIRE(api->query_context(&context, &context_info) == INPUT_NEXT_STATUS_OK);
    CHECK(context_info.state == INPUT_NEXT_EFFECTS_CONTEXT_OPEN);

    auto foreign = context;
    foreign.runtime_id[0U] ^= 0x40U;
    initialize_context_info_output(context_info);
    context_info.state = 0xA5U;
    CHECK(api->query_context(&foreign, &context_info) == INPUT_NEXT_STATUS_STALE_CONTEXT);
    CHECK(context_info.state == 0xA5U);

    const auto writes_before = fixture.rumble_writes();
    const auto request = rumble_request(context);
    input_next_effects_receipt_v1 receipt{};
    initialize_receipt_output(receipt);
    REQUIRE(api->submit(&request, &receipt) == INPUT_NEXT_STATUS_OK);
    CHECK(receipt.accepted_mask == INPUT_NEXT_EFFECTS_RUMBLE);
    CHECK(receipt.mutation_sequence == request.mutation_sequence);
    CHECK(receipt.publication_revision != 0U);
    REQUIRE(wait_for_rumble_value(fixture, writes_before, 0.25F, 0.5F));

    input_next_effects_progress_query_v1 progress_query{};
    progress_query.struct_size = sizeof(progress_query);
    progress_query.target = target;
    progress_query.publication_revision = receipt.publication_revision;
    input_next_effects_progress_v1 progress{};
    initialize_progress_output(progress);
    REQUIRE(api->query_progress(&progress_query, &progress) == INPUT_NEXT_STATUS_OK);
    CHECK(progress.requested_publication_revision == receipt.publication_revision);
    CHECK(progress.target.device_key == target.device_key);

    input_next_effects_clear_v1 clear{};
    clear.struct_size = sizeof(clear);
    clear.channel_mask = INPUT_NEXT_EFFECTS_RUMBLE;
    clear.context = context;
    clear.mutation_sequence = 2U;
    std::copy_n(receipt.channel_revisions, 4U, clear.channel_revisions);
    CHECK(api->clear(&clear) == INPUT_NEXT_STATUS_OK);

    const auto body = body_keep_request(context);
    input_next_effects_body_receipt_v1 body_receipt{};
    initialize_body_receipt_output(body_receipt);
    REQUIRE(api->submit_body(&body, &body_receipt) == INPUT_NEXT_STATUS_OK);
    CHECK(body_receipt.lease_id != 0U);
    CHECK(body_receipt.sequence == body.sequence);

    input_next_effects_body_clear_v1 body_clear{};
    body_clear.struct_size = sizeof(body_clear);
    body_clear.context = context;
    body_clear.lease_id = body_receipt.lease_id;
    body_clear.audio_instance_id = body_receipt.audio_instance_id;
    body_clear.sequence = 2U;
    CHECK(api->clear_body(&body_clear) == INPUT_NEXT_STATUS_OK);

    input_next_effects_close_v1 close{};
    close.struct_size = sizeof(close);
    close.context = context;
    close.close_request_id = context.id;
    input_next_effects_close_receipt_v1 close_receipt{};
    initialize_close_receipt_output(close_receipt);
    CHECK(api->close_context(&close, &close_receipt) == INPUT_NEXT_STATUS_REVOKED);
    CHECK(close_receipt.close_request_id == context.id);
    CHECK(close_receipt.publication_revision != 0U);

    initialize_context_info_output(context_info);
    CHECK(api->query_context(&context, &context_info) == INPUT_NEXT_STATUS_REVOKED);
    CHECK(context_info.state == INPUT_NEXT_EFFECTS_CONTEXT_REVOKED);

    auto forged_close_receipt = close_receipt;
    ++forged_close_receipt.target.device_key;
    CHECK(api->ack_close(&forged_close_receipt) == INPUT_NEXT_STATUS_STALE_CONTEXT);
    CHECK(api->ack_close(&close_receipt) == INPUT_NEXT_STATUS_OK);
    CHECK(api->ack_close(&close_receipt) == INPUT_NEXT_STATUS_OK);

    CHECK(fixture.prepare_attempts() == 1U);
    CHECK(fixture.owner_creations() == 1U);
}

TEST_CASE("runtime.effects.retained_degraded_progress_does_not_restart_native_runtime",
    "[integration][runtime][effects][abi][degraded]") {
    runtime_api_test_fixture fixture{true};
    const auto* const api = input_next_runtime_query_effects_api(INPUT_NEXT_EFFECTS_ABI_VERSION);
    REQUIRE(api != nullptr);

    input_next_effects_open_v1 open{};
    open.struct_size = sizeof(open);
    open.target = test_target();
    input_next_effects_context_v1 context{};
    initialize_context_output(context);
    CHECK(api->open_context(&open, &context) == INPUT_NEXT_STATUS_UNAVAILABLE);
    REQUIRE(fixture.owner_is_degraded());
    CHECK(fixture.prepare_attempts() == 1U);
    CHECK(fixture.native_initialized());

    const auto* const core = input_next_runtime_query_core_api(INPUT_NEXT_CORE_ABI_VERSION);
    REQUIRE(core != nullptr);
    input_next_runtime_info_v1 info{};
    info.struct_size = sizeof(info);
    REQUIRE(core->get_runtime_info(&info) == INPUT_NEXT_STATUS_OK);

    input_next_effects_progress_query_v1 query{};
    query.struct_size = sizeof(query);
    query.target = test_target();
    query.target.runtime_id[0U] = info.runtime_id[0U];
    query.target.runtime_id[1U] = info.runtime_id[1U];
    query.publication_revision = 1U;
    input_next_effects_progress_v1 progress{};
    initialize_progress_output(progress);
    CHECK(api->query_progress(&query, &progress) == INPUT_NEXT_STATUS_OK);
    CHECK(progress.stage == INPUT_NEXT_EFFECTS_PROGRESS_UNKNOWN);
    CHECK(fixture.prepare_attempts() == 1U);
    CHECK(fixture.native_initialized());
}

} // namespace
