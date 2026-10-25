#include "core_layout_assertions.h"

int main(void) {
    const input_next_core_api_v1 table = {0};
    const input_next_reader_v1 reader = {sizeof(input_next_reader_v1), 0, {0, 0}, 0};
    return table.struct_size != 0 || reader.reserved != 0;
}
