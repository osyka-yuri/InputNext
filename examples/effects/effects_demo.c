#include "effects_demo.h"

#include <input_next_core.h>
#include <input_next_effects.h>

#include <Windows.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int same_context(const input_next_effects_context_v1* left,
    const input_next_effects_context_v1* right) {
    return left->struct_size == sizeof(*left) && left->reserved == 0U &&
           left->runtime_id[0] == right->runtime_id[0] &&
           left->runtime_id[1] == right->runtime_id[1] && left->id == right->id;
}

static input_next_status close_and_ack(const input_next_effects_api_v1* api,
    const input_next_effects_context_v1* context) {
    input_next_effects_close_v1 request = {0};
    request.struct_size = sizeof(request);
    request.context = *context;
    request.close_request_id = context->id;
    input_next_effects_close_receipt_v1 receipt = {0};
    receipt.struct_size = sizeof(receipt);
    receipt.context.struct_size = sizeof(receipt.context);
    receipt.target.struct_size = sizeof(receipt.target);
    input_next_status status = INPUT_NEXT_STATUS_UNAVAILABLE;
    for (unsigned int attempt = 0U; attempt < 3U; ++attempt) {
        status = api->close_context(&request, &receipt);
        if (status == INPUT_NEXT_STATUS_REVOKED) {
            break;
        }
        Sleep(1U);
    }
    if (status != INPUT_NEXT_STATUS_REVOKED) {
        return status;
    }
    if (receipt.struct_size != sizeof(receipt) || !same_context(&receipt.context, context) ||
        receipt.close_request_id != context->id || receipt.publication_revision == 0U ||
        (receipt.disposition != INPUT_NEXT_EFFECTS_CLOSE_NORMAL &&
            receipt.disposition != INPUT_NEXT_EFFECTS_CLOSE_CONNECTION_RETIRED)) {
        return INPUT_NEXT_STATUS_IO_FAILED;
    }
    for (unsigned int attempt = 0U; attempt < 3U; ++attempt) {
        status = api->ack_close(&receipt);
        if (status == INPUT_NEXT_STATUS_OK) {
            return status;
        }
        Sleep(1U);
    }
    return status;
}

static input_next_status submit_rumble(const input_next_effects_api_v1* api,
    const input_next_effects_context_v1* context,
    const input_next_effects_capabilities_v1* capabilities,
    uint64_t* publication) {
    if ((capabilities->supported_channels & INPUT_NEXT_EFFECTS_RUMBLE) == 0U) {
        return INPUT_NEXT_STATUS_UNSUPPORTED_CHANNEL;
    }
    input_next_effects_request_v1 request = {0};
    request.struct_size = sizeof(request);
    request.channel_mask = INPUT_NEXT_EFFECTS_RUMBLE;
    request.ttl_ms = 500U;
    request.context = *context;
    request.mutation_sequence = 1U;
    request.rumble.low_frequency =
        (capabilities->feature_flags & INPUT_NEXT_EFFECTS_CAP_LOW_RUMBLE) != 0U ? 0.15F : 0.0F;
    request.rumble.high_frequency =
        (capabilities->feature_flags & INPUT_NEXT_EFFECTS_CAP_HIGH_RUMBLE) != 0U ? 0.15F : 0.0F;
    input_next_effects_receipt_v1 receipt = {0};
    receipt.struct_size = sizeof(receipt);
    receipt.context.struct_size = sizeof(receipt.context);
    const input_next_status status = api->submit(&request, &receipt);
    if (status == INPUT_NEXT_STATUS_OK) {
        if (!same_context(&receipt.context, context) || receipt.publication_revision == 0U) {
            return INPUT_NEXT_STATUS_IO_FAILED;
        }
        *publication = receipt.publication_revision;
    }
    return status;
}

static input_next_status submit_body(const input_next_effects_api_v1* api,
    const input_next_effects_context_v1* context,
    const input_next_effects_capabilities_v1* capabilities,
    uint64_t* publication) {
    if ((capabilities->feature_flags & INPUT_NEXT_EFFECTS_CAP_BODY_LEASE) == 0U) {
        return INPUT_NEXT_STATUS_UNSUPPORTED_CHANNEL;
    }
    const uint32_t frame_count = 24000U;
    float* samples = malloc((size_t)frame_count * sizeof(*samples));
    if (samples == NULL) {
        return INPUT_NEXT_STATUS_CAPACITY_EXCEEDED;
    }
    for (uint32_t frame = 0U; frame < frame_count; ++frame) {
        samples[frame] = sinf(6.2831853F * 120.0F * (float)frame / 48000.0F);
    }
    input_next_effects_body_request_v1 request = {0};
    request.struct_size = sizeof(request);
    request.ttl_ms = 1000U;
    request.route_preference = INPUT_NEXT_EFFECTS_BODY_ROUTE_PREFER_HD;
    request.pcm_command = INPUT_NEXT_EFFECTS_BODY_PCM_REPLACE;
    request.context = *context;
    request.audio_instance_id = capabilities->audio_instance_id;
    request.sequence = 1U;
    request.fallback_rumble.low_frequency =
        (capabilities->feature_flags & INPUT_NEXT_EFFECTS_CAP_LOW_RUMBLE) != 0U ? 0.15F : 0.0F;
    request.fallback_rumble.high_frequency =
        (capabilities->feature_flags & INPUT_NEXT_EFFECTS_CAP_HIGH_RUMBLE) != 0U ? 0.15F : 0.0F;
    request.pcm.struct_size = sizeof(request.pcm);
    request.pcm.frame_count = frame_count;
    request.pcm.channels = 1U;
    request.pcm.sample_rate = 48000U;
    request.pcm.gain = 0.1F;
    request.pcm.samples = samples;
    input_next_effects_body_receipt_v1 receipt = {0};
    receipt.struct_size = sizeof(receipt);
    receipt.context.struct_size = sizeof(receipt.context);
    receipt.target.struct_size = sizeof(receipt.target);
    const input_next_status status = api->submit_body(&request, &receipt);
    free(samples);
    if (status == INPUT_NEXT_STATUS_OK) {
        if (!same_context(&receipt.context, context) || receipt.publication_revision == 0U) {
            return INPUT_NEXT_STATUS_IO_FAILED;
        }
        *publication = receipt.publication_revision;
        (void)printf_s("Body route: %u; fallback reason: %u\n",
            receipt.effective_route,
            receipt.fallback_reason);
    }
    return status;
}

input_next_status inputnext_example_effect(const input_next_effects_api_v1* api,
    const int with_pcm) {
    input_next_device_selector_v1 selector = {0};
    selector.struct_size = sizeof(selector);
    selector.kind = INPUT_NEXT_CORE_SELECTOR_XINPUT_SLOT;
    selector.target.struct_size = sizeof(selector.target);
    input_next_device_target_v1 target = {0};
    target.struct_size = sizeof(target);
    input_next_effects_capabilities_v1 capabilities = {0};
    capabilities.struct_size = sizeof(capabilities);
    input_next_status status = api->resolve_target(&selector, &target, &capabilities);
    if (status != INPUT_NEXT_STATUS_OK) {
        return status;
    }
    input_next_effects_open_v1 request = {0};
    request.struct_size = sizeof(request);
    request.target = target;
    input_next_effects_context_v1 context = {0};
    context.struct_size = sizeof(context);
    status = api->open_context(&request, &context);
    if (status != INPUT_NEXT_STATUS_OK) {
        return status;
    }
    uint64_t publication = 0U;
    status = with_pcm != 0 ? submit_body(api, &context, &capabilities, &publication)
                           : submit_rumble(api, &context, &capabilities, &publication);
    if (status == INPUT_NEXT_STATUS_OK) {
        Sleep(550U);
        input_next_effects_progress_query_v1 query = {0};
        query.struct_size = sizeof(query);
        query.target = target;
        query.publication_revision = publication;
        input_next_effects_progress_v1 progress = {0};
        progress.struct_size = sizeof(progress);
        progress.target.struct_size = sizeof(progress.target);
        if (api->query_progress(&query, &progress) == INPUT_NEXT_STATUS_OK) {
            (void)printf_s("Dispatch stage: %u; completion class: %u\n",
                progress.stage,
                progress.completion_class);
        }
    }
    const input_next_status cleanup = close_and_ack(api, &context);
    if (cleanup != INPUT_NEXT_STATUS_OK) {
        (void)fprintf_s(stderr, "Context cleanup is unresolved: %u\n", cleanup);
        return cleanup;
    }
    return status;
}
