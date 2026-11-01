#pragma once

#include "fixtures/table_fixture_protocol.hpp"

namespace inputnext::xinput_dll_probe {

void check_guarded_calls(HMODULE facade14, HMODULE facade13);
void check_permissive_calls(HMODULE facade14, xinput_dll_fixture::snapshot_fn get_snapshot);
void check_permissive_dsound_call(HMODULE facade13, xinput_dll_fixture::snapshot_fn get_snapshot);

} // namespace inputnext::xinput_dll_probe
