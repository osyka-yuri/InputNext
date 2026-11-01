include("${ABI_MANIFEST_HELPER}")

set(_valid_manifest [=[{"library":"sample.dll","version":"1","exports":[{"ordinal":1,"name":"SampleGetState","noname":false}]}]=])
inputnext_validate_abi_manifest("${_valid_manifest}" "sample.dll" _valid_error _valid_count)
if(NOT _valid_error STREQUAL "" OR NOT _valid_count EQUAL 1)
    message(FATAL_ERROR "The oracle rejected a valid manifest: ${_valid_error}")
endif()

set(_cases
    "invalid_json|{"
    "missing_exports|{\"library\":\"sample.dll\",\"version\":\"1\"}"
    "unknown_field|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":[],\"extra\":0}"
    "wrong_library_type|{\"library\":1,\"version\":\"1\",\"exports\":[]}"
    "wrong_version_type|{\"library\":\"sample.dll\",\"version\":1,\"exports\":[]}"
    "wrong_exports_type|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":{}}"
    "empty_exports|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":[]}"
    "wrong_export_type|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":[1]}"
    "missing_ordinal|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":[{\"name\":\"SampleGetState\",\"noname\":false}]}"
    "string_ordinal|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":[{\"ordinal\":\"1\",\"name\":\"SampleGetState\",\"noname\":false}]}"
    "zero_ordinal|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":[{\"ordinal\":0,\"name\":\"SampleGetState\",\"noname\":false}]}"
    "negative_ordinal|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":[{\"ordinal\":-1,\"name\":\"SampleGetState\",\"noname\":false}]}"
    "fractional_ordinal|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":[{\"ordinal\":1.5,\"name\":\"SampleGetState\",\"noname\":false}]}"
    "out_of_range_ordinal|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":[{\"ordinal\":65536,\"name\":\"SampleGetState\",\"noname\":false}]}"
    "duplicate_ordinal|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":[{\"ordinal\":1,\"name\":\"SampleGetState\",\"noname\":false},{\"ordinal\":1,\"name\":\"SampleSetState\",\"noname\":false}]}"
    "missing_name|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":[{\"ordinal\":1,\"noname\":false}]}"
    "empty_name|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":[{\"ordinal\":1,\"name\":\"\",\"noname\":false}]}"
    "invalid_name|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":[{\"ordinal\":1,\"name\":\"Sample Get State\",\"noname\":false}]}"
    "duplicate_name|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":[{\"ordinal\":1,\"name\":\"SampleGetState\",\"noname\":false},{\"ordinal\":2,\"name\":\"SampleGetState\",\"noname\":true}]}"
    "missing_noname|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":[{\"ordinal\":1,\"name\":\"SampleGetState\"}]}"
    "wrong_noname_type|{\"library\":\"sample.dll\",\"version\":\"1\",\"exports\":[{\"ordinal\":1,\"name\":\"SampleGetState\",\"noname\":\"true\"}]}"
)

foreach(_case IN LISTS _cases)
    string(FIND "${_case}" "|" _separator)
    string(SUBSTRING "${_case}" 0 ${_separator} _name)
    math(EXPR _json_start "${_separator} + 1")
    string(SUBSTRING "${_case}" ${_json_start} -1 _json)
    inputnext_validate_abi_manifest("${_json}" "sample.dll" _error _count)
    if(_error STREQUAL "")
        message(FATAL_ERROR "The oracle accepted malformed case '${_name}'")
    endif()
endforeach()

list(LENGTH _cases _case_count)
message(STATUS "ABI manifest oracle rejected ${_case_count} malformed cases")
