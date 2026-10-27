#pragma once

#include <input_next_core.h>

struct input_next_effects_api_v1;

#ifdef __cplusplus
extern "C" {
#endif

input_next_status inputnext_example_effect(const struct input_next_effects_api_v1* api,
    int with_pcm);

#ifdef __cplusplus
}
#endif
