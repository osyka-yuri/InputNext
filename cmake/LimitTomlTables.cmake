# Bound implicit dotted tables as well as explicit and inline tables. Upstream's
# TOML_MAX_NESTED_VALUES limits recursive values but does not limit dotted keys.
set(parser_path "${SOURCE_DIR}/include/toml++/impl/parser.inl")
file(SHA256 "${parser_path}" actual_hash)
set(original_hash "147519f449768891a343309c1ab4c31b593e3c222d3fc3ec752b0ba8050b14ca")
set(patched_hash "975ca6a66b7f97d97d4c718558078ec08d380bbd9d34f746e7db1476295ccc9a")
if(actual_hash STREQUAL patched_hash)
    return()
endif()
if(NOT actual_hash STREQUAL original_hash)
    message(FATAL_ERROR "Unexpected toml++ parser source; review the pinned table-limit patch.")
endif()

file(READ "${parser_path}" parser)
string(REGEX MATCHALL "emplace_hint<table>|emplace_back<table>|new table\\{\\}" table_creations "${parser}")
list(LENGTH table_creations table_creation_count)
if(NOT table_creation_count EQUAL 6)
    message(FATAL_ERROR "The toml++ table allocation sites changed.")
endif()

set(table_budget [=[size_t nested_values = {};
        size_t table_count = 1; // The root table already exists.

        bool claim_table()
        {
            if (table_count == 128)
            {
                set_error("exceeded maximum table count of 128"sv);
                return false;
            }
            ++table_count;
            return true;
        }]=])
string(REPLACE "size_t nested_values = {};" "${table_budget}" parser "${parser}")
string(REGEX REPLACE
    "\n([\t ]+)([^\n]*(emplace_hint<table>|emplace_back<table>|new table\\{\\})[^\n]*)"
    "\n\\1if (!claim_table()) return_after_error({});\n\\1\\2"
    parser "${parser}"
)
file(WRITE "${parser_path}" "${parser}")
file(SHA256 "${parser_path}" actual_hash)
if(NOT actual_hash STREQUAL patched_hash)
    message(FATAL_ERROR "The toml++ table-limit patch did not produce the reviewed source.")
endif()
