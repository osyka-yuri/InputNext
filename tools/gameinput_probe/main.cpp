#include "gameinput_probe/input/worker.hpp"
#include "gameinput_probe/options.hpp"
#include "gameinput_probe/output/dualsense_usb/sequence.hpp"
#include "gameinput_probe/output/dualsense_usb/worker.hpp"
#include "gameinput_probe/output/hd_audio/worker.hpp"
#include "gameinput_probe/output/rumble/worker.hpp"

#include "gameinput_probe/process/supervisor.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string_view>

namespace {

[[nodiscard]] bool parse_positive(std::wstring_view text, std::uint32_t& value) noexcept {
    if (text.empty()) {
        return false;
    }

    std::uint32_t parsed{};
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            return false;
        }
        const auto digit = static_cast<std::uint32_t>(character - L'0');
        if (parsed > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        parsed = (parsed * 10U) + digit;
    }

    if (parsed == 0U) {
        return false;
    }
    value = parsed;
    return true;
}

[[nodiscard]] int usage() {
    std::wcerr << L"Usage: inputnext_gameinput_probe [--duration-ms N] [--require-device] "
                  L"[--require-reading] [--history] [--require-history] [--xinput] "
                  L"[--xinput-events] [--rumble | --dualsense-usb | --dualsense-hd] "
                  L"[--start-step K]\n"
                  L"--rumble opens a foreground diagnostic window; output starts only after its "
                  L"Start click. Confirm each step manually.\n"
                  L"--dualsense-usb opens a manual DualSense USB output sequence after Start.\n"
                  L"--dualsense-hd opens a manual USB HD audio output sequence after Start.\n"
                  L"Output diagnostics have no overall time limit unless --duration-ms is set.\n"
                  L"--start-step selects the first DualSense USB step (1 through 20). Skipped "
                  L"steps are not passed.\n"
                  L"N must be between 1 and 300000 milliseconds.\n";
    return 2;
}

[[nodiscard]] bool finalize_options(inputnext::gameinput_detail::probe_options& options,
    const bool duration_was_set,
    const bool start_step_was_set) {
    if (start_step_was_set &&
        (!options.dualsense_usb ||
            options.first_step > inputnext::gameinput_detail::dualsense_usb_steps().size())) {
        return false;
    }
    const bool other_probe_mode = options.require_device || options.require_reading ||
                                  options.history || options.require_history || options.xinput ||
                                  options.xinput_events || options.rumble;
    if (!duration_was_set && (options.rumble || options.dualsense_usb || options.dualsense_hd)) {
        options.duration_ms.reset();
    }
    if (options.dualsense_usb || options.dualsense_hd) {
        return !other_probe_mode && !(options.dualsense_usb && options.dualsense_hd);
    }
    return !options.rumble ||
           !(options.require_device || options.require_reading || options.history ||
               options.require_history || options.xinput || options.xinput_events);
}

[[nodiscard]] bool parse_options(int argc,
    wchar_t** argv,
    int first_argument,
    inputnext::gameinput_detail::probe_options& options) {
    bool duration_was_set = false;
    bool start_step_was_set = false;
    for (int index = first_argument; index < argc; ++index) {
        const std::wstring_view argument{argv[index]};
        if (argument == L"--require-device") {
            options.require_device = true;
        } else if (argument == L"--require-reading") {
            options.require_reading = true;
        } else if (argument == L"--history") {
            options.history = true;
        } else if (argument == L"--require-history") {
            options.history = true;
            options.require_history = true;
        } else if (argument == L"--xinput") {
            options.xinput = true;
        } else if (argument == L"--xinput-events") {
            options.xinput = true;
            options.xinput_events = true;
        } else if (argument == L"--rumble") {
            options.rumble = true;
        } else if (argument == L"--dualsense-usb") {
            options.dualsense_usb = true;
        } else if (argument == L"--dualsense-hd") {
            options.dualsense_hd = true;
        } else if (argument == L"--duration-ms" && index + 1 < argc) {
            std::uint32_t value{};
            if (!parse_positive(argv[++index], value) || value > 300'000U) {
                return false;
            }
            options.duration_ms = value;
            duration_was_set = true;
        } else if (argument == L"--start-step" && index + 1 < argc) {
            if (!parse_positive(argv[++index], options.first_step)) {
                return false;
            }
            start_step_was_set = true;
        } else {
            return false;
        }
    }
    return finalize_options(options, duration_was_set, start_step_was_set);
}

} // namespace

int wmain(int argc, wchar_t** argv) try {
    const bool worker = argc >= 2 && std::wstring_view{argv[1]} == L"--inputnext-worker";
    inputnext::gameinput_detail::probe_options options{};
    if (!parse_options(argc, argv, worker ? 2 : 1, options)) {
        return usage();
    }
    if (worker) {
        if (options.rumble) {
            return inputnext::gameinput_detail::run_rumble_probe_worker(options, std::cout);
        }
        if (options.dualsense_usb) {
            return inputnext::gameinput_detail::run_dualsense_usb_probe_worker(options, std::cout);
        }
        if (options.dualsense_hd) {
            return inputnext::gameinput_detail::run_hd_audio_probe_worker(options, std::cout);
        }
        return inputnext::gameinput_detail::run_probe_worker(options, std::cout);
    }

    const auto result = inputnext::gameinput_detail::run_supervised_probe(options,
        {.output = std::cout, .errors = std::cerr});
    return result.exit_code;
} catch (const std::exception& error) {
    static_cast<void>(std::fprintf(stderr, "gameinput_probe_error=%s\n", error.what()));
    return 1;
} catch (...) {
    static_cast<void>(std::fputs("gameinput_probe_error=unknown exception\n", stderr));
    return 1;
}
