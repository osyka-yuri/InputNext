set(hashes "")
foreach(suffix IN ITEMS "" "-symbols")
    set(name "${PACKAGE_NAME}${suffix}.zip")
    file(SHA256 "${PACKAGE_DIRECTORY}/${name}" hash)
    string(APPEND hashes "${hash}  ${name}\n")
endforeach()
file(CONFIGURE OUTPUT "${PACKAGE_DIRECTORY}/${PACKAGE_NAME}-SHA256SUMS.txt"
    CONTENT "${hashes}" @ONLY NEWLINE_STYLE LF)
