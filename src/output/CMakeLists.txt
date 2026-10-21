add_library(inputnext_output_policy STATIC
    policy/policy.cpp
)
target_include_directories(inputnext_output_policy PUBLIC "${PROJECT_SOURCE_DIR}/include")
inputnext_configure_target(inputnext_output_policy)

add_library(inputnext_output_authority STATIC
    authority/incarnation.cpp
    authority/state.cpp
    authority/cleanup.cpp
    authority/lifecycle.cpp
    authority/mutation.cpp
    authority/facts.cpp
    authority/snapshot.cpp
    composition/composition.cpp
    composition/internal_producer.cpp
)
target_include_directories(inputnext_output_authority
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_output_authority
    PUBLIC inputnext_output_policy inputnext_devices inputnext_focus
    PRIVATE Ole32
)

inputnext_configure_target(inputnext_output_authority)
