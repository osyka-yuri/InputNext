#include <GameInput.h>
#include <Windows.h>

#include "bootstrap.hpp"
#include "capture_report.hpp"
#include "gameinput_observation.hpp"
#include "input/touch_resolver.hpp"
#include "native_capture.hpp"
#include "report.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace gameinput = GameInput::v4;
namespace diagnostic = inputnext::input_qualification::diagnostic;
using diagnostic::device_report;
using diagnostic::max_callback_records;
using diagnostic::observation_kinds;
using inputnext::input_qualification::reading_sample;

constexpr std::uint32_t max_device_events = 32U;
constexpr std::uint16_t dualsense_vendor = 0x054CU;
constexpr std::uint16_t dualsense_product = 0x0CE6U;

struct options final {
    std::uint32_t seconds{};
    std::filesystem::path output;
    std::optional<std::uint32_t> bootstrap_click_index;
    bool dualsense_usb_raw{};
};

struct device_event final {
    gameinput::IGameInputDevice* device{};
    gameinput::GameInputDeviceEvent event{};
};

struct capture_state final {
    SRWLOCK lock = SRWLOCK_INIT;
    inputnext::input::touch_resolver raw_touch_resolver{};
    std::array<reading_sample, max_callback_records> callbacks{};
    std::array<gameinput::IGameInputReading*, max_callback_records> callback_readings{};
    std::uint32_t callback_count{};
    std::uint64_t callback_drops{};
    std::uint64_t callback_gamepad_missing{};
    std::uint64_t callback_button_short{};
    std::uint64_t callback_contact_truncated{};
    std::array<device_event, max_device_events> devices{};
    std::uint32_t device_count{};
    std::uint64_t device_event_drops{};
    // Installed before registering the reading callback and immutable through drain.
    std::optional<device_report> descriptor;

    capture_state() = default;
    capture_state(const capture_state&) = delete;
    capture_state& operator=(const capture_state&) = delete;
    capture_state(capture_state&&) = delete;
    capture_state& operator=(capture_state&&) = delete;

    ~capture_state() {
        for (auto* reading : callback_readings) {
            if (reading != nullptr) {
                reading->Release();
            }
        }
        for (const auto& event : devices) {
            if (event.device != nullptr) {
                event.device->Release();
            }
        }
    }
};

struct device_batch final {
    std::array<device_event, max_device_events> events{};
    std::uint32_t size{};
};

struct callback_registration final {
    gameinput::GameInputCallbackToken token{};
    bool active{};
};

enum class callback_kind : std::uint8_t { reading, device };

struct com_device_release final {
    void operator()(gameinput::IGameInputDevice* device) const noexcept {
        if (device != nullptr) {
            device->Release();
        }
    }
};

using device_owner = std::unique_ptr<gameinput::IGameInputDevice, com_device_release>;

class exclusive_lock final {
public:
    explicit exclusive_lock(SRWLOCK& lock) noexcept : lock_(lock) {
        AcquireSRWLockExclusive(&lock_);
    }

    ~exclusive_lock() noexcept { ReleaseSRWLockExclusive(&lock_); }

    exclusive_lock(const exclusive_lock&) = delete;
    exclusive_lock& operator=(const exclusive_lock&) = delete;
    exclusive_lock(exclusive_lock&&) = delete;
    exclusive_lock& operator=(exclusive_lock&&) = delete;

private:
    SRWLOCK& lock_;
};

class callback_lifecycle final {
public:
    explicit callback_lifecycle(gameinput::IGameInput* input) noexcept : input_(input) {}

    ~callback_lifecycle() noexcept { drain(); }

    callback_lifecycle(const callback_lifecycle&) = delete;
    callback_lifecycle& operator=(const callback_lifecycle&) = delete;
    callback_lifecycle(callback_lifecycle&&) = delete;
    callback_lifecycle& operator=(callback_lifecycle&&) = delete;

    void set_device_registration(HRESULT result, gameinput::GameInputCallbackToken token) noexcept {
        device_.token = token;
        device_.active = SUCCEEDED(result) || token != 0U;
    }

    void set_reading_registration(HRESULT result,
        gameinput::GameInputCallbackToken token) noexcept {
        reading_.token = token;
        reading_.active = SUCCEEDED(result) || token != 0U;
    }

    void drain() noexcept {
        drain_one(reading_, callback_kind::reading);
        drain_one(device_, callback_kind::device);
    }

private:
    void drain_one(callback_registration& registration, const callback_kind kind) noexcept {
        bool announced = false;
        while (registration.active) {
            if (input_->UnregisterCallback(registration.token)) {
                registration.active = false;
                registration.token = 0U;
                break;
            }
            if (!announced) {
                if (kind == callback_kind::reading) {
                    OutputDebugStringW(
                        L"Waiting for GameInput reading callback to unregister safely.\n");
                } else {
                    OutputDebugStringW(
                        L"Waiting for GameInput device callback to unregister safely.\n");
                }
                announced = true;
            }
            Sleep(10U);
        }
    }

    gameinput::IGameInput* input_{};
    callback_registration reading_{};
    callback_registration device_{};
};

struct runtime_api final {
    HMODULE module{};
    gameinput::IGameInput* input{};
    std::wstring path;
    std::wstring file_version;

    runtime_api() = default;
    runtime_api(const runtime_api&) = delete;
    runtime_api& operator=(const runtime_api&) = delete;
    runtime_api(runtime_api&&) = delete;
    runtime_api& operator=(runtime_api&&) = delete;

    ~runtime_api() {
        if (input != nullptr) {
            input->Release();
        }
        if (module != nullptr) {
            FreeLibrary(module);
        }
    }
};

struct capture_iteration_state final {
    capture_state& capture;
    callback_lifecycle& callbacks;
    device_owner& selected_device;
    diagnostic::native_capture& native_history;
    inputnext::input_qualification::bootstrap_measurement& bootstrap_measurements;
    std::string_view& bootstrap_status;
    gameinput::GameInputCallbackToken& reading_token;
    std::uint64_t& registration_errors;
    std::uint32_t& processed_device_count;
    std::uint32_t& processed_touch_callback_count;
    bool raw_usb_mode_requested{};
};

[[nodiscard]] bool parse_number(std::wstring_view text, std::uint32_t& result) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint32_t value{};
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            return false;
        }
        const auto digit = static_cast<std::uint32_t>(character - L'0');
        if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U) {
            return false;
        }
        value = (value * 10U) + digit;
    }
    result = value;
    return true;
}

[[nodiscard]] std::optional<options> parse_options(int argc, wchar_t** argv) {
    options parsed{};
    bool have_seconds = false;
    bool have_output = false;
    bool have_bootstrap_click_index = false;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument{argv[index]};
        if (argument == L"--dualsense-usb-raw" && !parsed.dualsense_usb_raw) {
            parsed.dualsense_usb_raw = true;
            continue;
        }
        if (index + 1 >= argc) {
            return std::nullopt;
        }
        if (argument == L"--seconds") {
            if (have_seconds || !parse_number(argv[++index], parsed.seconds) ||
                parsed.seconds == 0U || parsed.seconds > 60U) {
                return std::nullopt;
            }
            have_seconds = true;
        } else if (argument == L"--output") {
            if (have_output) {
                return std::nullopt;
            }
            parsed.output = std::filesystem::path{argv[++index]};
            have_output = true;
        } else if (argument == L"--bootstrap-click-index") {
            std::uint32_t click_index{};
            if (have_bootstrap_click_index || !parse_number(argv[++index], click_index) ||
                click_index >= 64U) {
                return std::nullopt;
            }
            parsed.bootstrap_click_index = click_index;
            have_bootstrap_click_index = true;
        } else {
            return std::nullopt;
        }
    }
    if (!have_seconds || !parsed.output.is_absolute()) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] std::wstring module_path(HMODULE module) {
    std::vector<wchar_t> buffer(32768U);
    const auto length =
        GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size()) {
        return {};
    }
    return std::wstring{buffer.data(), length};
}

[[nodiscard]] std::wstring file_version(const std::wstring& path) {
    DWORD ignored{};
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0U) {
        return {};
    }
    std::vector<std::byte> buffer(size);
    if (GetFileVersionInfoW(path.c_str(), 0U, size, buffer.data()) == FALSE) {
        return {};
    }
    VS_FIXEDFILEINFO* info{};
    UINT info_size{};
    if (VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) ==
            FALSE ||
        info == nullptr || info_size < sizeof(VS_FIXEDFILEINFO)) {
        return {};
    }
    std::wostringstream value;
    value << HIWORD(info->dwFileVersionMS) << L'.' << LOWORD(info->dwFileVersionMS) << L'.'
          << HIWORD(info->dwFileVersionLS) << L'.' << LOWORD(info->dwFileVersionLS);
    return value.str();
}

[[nodiscard]] bool initialize_runtime(runtime_api& runtime) {
    const auto executable = module_path(nullptr);
    if (executable.empty()) {
        return false;
    }
    runtime.path =
        (std::filesystem::path{executable}.parent_path() / L"GameInputRedist.dll").wstring();
    runtime.module = LoadLibraryExW(runtime.path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (runtime.module == nullptr) {
        return false;
    }
    using initialize_fn = HRESULT(WINAPI*)(REFIID, LPVOID*);
    const auto initialize =
        reinterpret_cast<initialize_fn>(GetProcAddress(runtime.module, "GameInputInitialize"));
    if (initialize == nullptr ||
        FAILED(initialize(gameinput::IID_IGameInput, reinterpret_cast<void**>(&runtime.input))) ||
        runtime.input == nullptr) {
        return false;
    }
    runtime.file_version = file_version(runtime.path);
    runtime.input->EnableBackgroundInput(true);
    return true;
}

[[nodiscard]] std::string narrow_ascii(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t character : text) {
        result.push_back(
            character >= 0x20 && character <= 0x7e ? static_cast<char>(character) : '?');
    }
    return result;
}

[[nodiscard]] std::string to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const auto needed = WideCharToMultiByte(CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (needed <= 0) {
        return narrow_ascii(text);
    }
    std::string result(static_cast<std::size_t>(needed), '\0');
    static_cast<void>(WideCharToMultiByte(CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        needed,
        nullptr,
        nullptr));
    return result;
}

[[nodiscard]] std::string_view computer_architecture() noexcept {
#ifdef _M_X64
    return "x64";
#elifdef _M_IX86
    return "x86";
#else
    return "unknown";
#endif
}

void CALLBACK on_reading(gameinput::GameInputCallbackToken token,
    void* context,
    gameinput::IGameInputReading* reading) noexcept {
    static_cast<void>(token);
    auto* state = static_cast<capture_state*>(context);
    if (state == nullptr || reading == nullptr) {
        return;
    }
    const exclusive_lock lock{state->lock};
    if (state->callback_count >= state->callbacks.size()) {
        ++state->callback_drops;
        return;
    }
    if (!state->descriptor.has_value()) {
        return;
    }
    const auto& descriptor = *state->descriptor;
    const auto sample =
        diagnostic::inspect_reading(*reading, descriptor, &state->raw_touch_resolver);
    if (!sample.gamepad_available) {
        ++state->callback_gamepad_missing;
    }
    if (sample.returned_button_count < descriptor.button_count) {
        ++state->callback_button_short;
    }
    if (sample.contact_count_truncated) {
        ++state->callback_contact_truncated;
    }
    reading->AddRef();
    state->callback_readings[state->callback_count] = reading;
    state->callbacks[state->callback_count] = sample;
    ++state->callback_count;
}

void CALLBACK on_device(gameinput::GameInputCallbackToken token,
    void* context,
    gameinput::GameInputDeviceEvent event,
    gameinput::IGameInputDevice* device) noexcept {
    static_cast<void>(token);
    auto* state = static_cast<capture_state*>(context);
    if (state == nullptr || device == nullptr || device->GetVendorId() != dualsense_vendor ||
        device->GetProductId() != dualsense_product) {
        return;
    }
    const exclusive_lock lock{state->lock};
    if (state->device_count >= state->devices.size()) {
        ++state->device_event_drops;
        return;
    }
    device->AddRef();
    state->devices[state->device_count++] = device_event{.device = device, .event = event};
}

void process_device_events(runtime_api& runtime, capture_iteration_state& state) {
    device_batch pending{};
    {
        const exclusive_lock lock{state.capture.lock};
        pending.size = state.capture.device_count - state.processed_device_count;
        std::copy_n(state.capture.devices.begin() +
                        static_cast<std::ptrdiff_t>(state.processed_device_count),
            static_cast<std::ptrdiff_t>(pending.size),
            pending.events.begin());
        state.processed_device_count += pending.size;
    }
    for (std::uint32_t index = 0U; index < pending.size; ++index) {
        auto* device = pending.events[index].device;
        const bool is_first_connected_device =
            pending.events[index].event == gameinput::GameInputDeviceEventConnected &&
            state.selected_device == nullptr && device->IsConnected();
        if (!is_first_connected_device) {
            continue;
        }
        device->AddRef();
        state.selected_device.reset(device);
        auto descriptor = diagnostic::inspect_device(*device, state.raw_usb_mode_requested);
        state.bootstrap_measurements.configure(descriptor);
        if (state.bootstrap_measurements.requested()) {
            state.bootstrap_status = state.bootstrap_measurements.click_index_valid()
                                         ? "waiting_for_source_baseline"
                                         : "invalid_click_index_for_observed_button_count";
        }
        {
            const exclusive_lock lock{state.capture.lock};
            state.capture.descriptor.emplace(std::move(descriptor));
        }
        const HRESULT registration = runtime.input->RegisterReadingCallback(device,
            observation_kinds,
            &state.capture,
            &on_reading,
            &state.reading_token);
        state.callbacks.set_reading_registration(registration, state.reading_token);
        state.registration_errors += FAILED(registration) ? 1U : 0U;
    }
}

void update_bootstrap_status(const inputnext::input_qualification::bootstrap_attempt_result attempt,
    std::string_view& status) noexcept {
    using inputnext::input_qualification::bootstrap_attempt_result;
    switch (attempt) {
    case bootstrap_attempt_result::not_requested:
        status = "not_requested";
        break;
    case bootstrap_attempt_result::invalid_click_index:
        status = "invalid_click_index_for_observed_button_count";
        break;
    case bootstrap_attempt_result::waiting_for_source_baseline:
        status = "waiting_for_source_baseline";
        break;
    case bootstrap_attempt_result::waiting_for_click_held:
        status = "waiting_for_click_held";
        break;
    case bootstrap_attempt_result::not_due:
        break;
    case bootstrap_attempt_result::recorded:
        status = "collecting_production_cursor_measurements";
        break;
    case bootstrap_attempt_result::capacity_exhausted:
        status = "attempt_capacity_exhausted";
        break;
    }
}

void update_bootstrap(capture_iteration_state& state,
    gameinput::IGameInput* game_input,
    gameinput::IGameInputDevice* device) {
    for (;;) {
        gameinput::IGameInputReading* observed_reading{};
        {
            const exclusive_lock lock{state.capture.lock};
            if (state.processed_touch_callback_count >= state.capture.callback_count) {
                break;
            }
            observed_reading =
                state.capture.callback_readings[state.processed_touch_callback_count];
            ++state.processed_touch_callback_count;
        }
        state.bootstrap_measurements.observe_reading(observed_reading);
    }

    std::uint64_t observed_callback_drops{};
    {
        const exclusive_lock lock{state.capture.lock};
        observed_callback_drops = state.capture.callback_drops;
    }
    if (observed_callback_drops != 0U) {
        state.bootstrap_status = "callback_capture_lost_no_further_attempts";
        return;
    }
    update_bootstrap_status(state.bootstrap_measurements.try_attempt(game_input, device),
        state.bootstrap_status);
}

void capture_iteration(runtime_api& runtime, capture_iteration_state& state) {
    process_device_events(runtime, state);
    const auto& descriptor = state.capture.descriptor;
    if (state.selected_device == nullptr || !descriptor.has_value()) {
        return;
    }
    state.native_history.advance(*runtime.input, *state.selected_device, *descriptor);
    if (state.bootstrap_measurements.click_index_valid()) {
        update_bootstrap(state, runtime.input, state.selected_device.get());
    }
}

[[nodiscard]] int run(const options& arguments) {
    runtime_api runtime{};
    if (!initialize_runtime(runtime)) {
        std::wcerr << L"GameInput initialization failed; expected app-local GameInputRedist.dll "
                      L"beside the executable.\n";
        return 3;
    }

    const auto capture_storage = std::make_unique<capture_state>();
    capture_state& capture = *capture_storage;
    diagnostic::native_capture native_history;
    inputnext::input_qualification::bootstrap_measurement bootstrap_measurements{
        arguments.bootstrap_click_index};
    device_owner selected_device{};
    callback_lifecycle callbacks{runtime.input};
    gameinput::GameInputCallbackToken device_token{};
    const HRESULT device_registration = runtime.input->RegisterDeviceCallback(nullptr,
        observation_kinds,
        &capture,
        &on_device,
        &device_token);
    callbacks.set_device_registration(device_registration, device_token);
    if (FAILED(device_registration)) {
        std::wcerr << L"RegisterDeviceCallback failed: 0x" << std::hex
                   << static_cast<unsigned long>(device_registration) << L"\n";
        return 3;
    }

    std::wcout << L"Passive GameInput capture is ready. During the next " << arguments.seconds
               << L" seconds, please perform: touch without clicking; click left and release; "
                  L"click right and release; "
                  L"hold each click while moving the finger; then press ordinary View and hand it "
                  L"off to a touchpad click. "
                  L"Do not change controller mode.\n";
    if (arguments.dualsense_usb_raw) {
        std::wcout << L"Experimental DualSense USB raw touch decoding is requested. It will only "
                      L"read a single advertised 64-byte input report (ID 1) with the observed "
                      L"15-button layout; it does not grant production support.\n";
        std::wcout << L"For side and latching evidence: touch without clicking and swipe; hold "
                      L"the left click for about one second and release; repeat on the right; "
                      L"then hold a click while moving across the center.\n";
    }
    if (arguments.bootstrap_click_index.has_value()) {
        std::wcout
            << L"Experimental native bootstrap measurement will use raw controller button index "
            << *arguments.bootstrap_click_index
            << L" after callback source baseline is observed; this diagnostic does not qualify or "
               L"grant production support.\n";
    }
    std::wcout.flush();

    std::uint32_t processed_touch_callback_count{};
    std::string_view bootstrap_status = arguments.bootstrap_click_index.has_value()
                                            ? "waiting_for_matching_device"
                                            : "not_requested";
    gameinput::GameInputCallbackToken reading_token{};
    std::uint64_t registration_errors{};
    std::uint32_t processed_device_count{};
    capture_iteration_state iteration_state{.capture = capture,
        .callbacks = callbacks,
        .selected_device = selected_device,
        .native_history = native_history,
        .bootstrap_measurements = bootstrap_measurements,
        .bootstrap_status = bootstrap_status,
        .reading_token = reading_token,
        .registration_errors = registration_errors,
        .processed_device_count = processed_device_count,
        .processed_touch_callback_count = processed_touch_callback_count,
        .raw_usb_mode_requested = arguments.dualsense_usb_raw};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{arguments.seconds};
    while (std::chrono::steady_clock::now() < deadline) {
        capture_iteration(runtime, iteration_state);
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    callbacks.drain();

    const auto& descriptor = capture.descriptor;
    diagnostic::native_capture native_tail;
    if (selected_device != nullptr && descriptor.has_value()) {
        native_history.finish(*runtime.input, *selected_device, *descriptor);
        native_tail =
            diagnostic::native_capture::read_tail(*runtime.input, *selected_device, *descriptor);
    }
    try {
        const auto observed_button_count = descriptor.has_value() ? descriptor->button_count : 0U;
        auto final_bootstrap_status = bootstrap_status;
        if (!descriptor.has_value()) {
            final_bootstrap_status = bootstrap_measurements.requested()
                                         ? "no_matching_device_observed"
                                         : "not_requested";
        }
        const auto bootstrap_summary =
            bootstrap_measurements.summary(final_bootstrap_status, observed_button_count);
        std::vector<diagnostic::reading_view> callback_records;
        callback_records.reserve(capture.callback_count);
        for (std::uint32_t index = 0U; index < capture.callback_count; ++index) {
            callback_records.push_back(
                diagnostic::reading_view{.data = std::cref(capture.callbacks[index]),
                    .canonical_identity =
                        diagnostic::canonical_identity(*capture.callback_readings[index])});
        }
        const auto runtime_file_version = to_utf8(runtime.file_version);
        const auto runtime_path = to_utf8(runtime.path);
        const diagnostic::callback_capture_view callbacks_view{.records = callback_records,
            .drops = capture.callback_drops,
            .missing_gamepad = capture.callback_gamepad_missing,
            .short_button_state = capture.callback_button_short,
            .truncated_contacts = capture.callback_contact_truncated};
        std::vector<diagnostic::reading_view> native_records;
        std::vector<diagnostic::reading_view> tail_records;
        const diagnostic::report_context report{.capture_seconds = arguments.seconds,
            .sdk_version = INPUTNEXT_GAMEINPUT_SDK_VERSION,
            .runtime_file_version = runtime_file_version,
            .runtime_path = runtime_path,
            .architecture = computer_architecture(),
            .registration_errors = registration_errors,
            .device_event_drops = capture.device_event_drops,
            .raw_usb_requested = arguments.dualsense_usb_raw,
            .device = descriptor.has_value() ? &*descriptor : nullptr,
            .callbacks = callbacks_view,
            .native_forward = native_history.view(native_records),
            .native_tail = native_tail.view(tail_records),
            .bootstrap = bootstrap_summary};
        std::ofstream output{arguments.output, std::ios::binary};
        if (!output) {
            throw std::runtime_error("cannot open requested output path");
        }
        diagnostic::write_capture_report(output, report);
        output.flush();
        if (!output) {
            throw std::runtime_error("failed while writing the requested output file");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 4;
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const auto arguments = parse_options(argc, argv);
        if (!arguments.has_value()) {
            std::wcerr << L"Usage: inputnext_input_qualification --seconds 1..60 --output "
                          L"<absolute-json-path> [--bootstrap-click-index 0..63] "
                          L"[--dualsense-usb-raw]\n";
            return 2;
        }
        return run(*arguments);
    } catch (...) {
        return 4;
    }
}
