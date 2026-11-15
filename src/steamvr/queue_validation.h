#pragma once
#include "module.h"

namespace ct::steamvr {
struct QueueMethod {
    size_t size;
    const char *signature;
};
struct QueueLayout {
    const char *rtti;
    QueueMethod methods[9];
};
inline constexpr QueueLayout ServerQueueLayout{
    "20CVRBlockQueueManager",
    {
        {1136, "c1c72e522a433abdf123075cd0fd483c180a2df7dda0ddfeb78b2ede384453fa"},
        {1560, "e2e8313962a6654ff629fbc16cc089c416ec5c3846bc198e86c42397f66232ab"},
        {308, "bfa0f73813e56006d95df5c3dc43577ec56a74ed8adc17b0e52f4145d3c8e5b3"},
        {236, "a54ae217c0062a34597425911cc2891a1e03563c0e69e6a522095c18b402b355"},
        {232, "25eb20485fdd3667582cf9b4f988da50bf2218d240f42ba14ddee5701beadcc5"},
        {468, "3a62819dc72fb035999ce3fc0cc7314e9abc7531d46706de2bd4b1ca7ccdce90"},
        {252, "80cc624043cb7b1cbedd77a23bc148136abdfcd6e4dfa7aaf03faff58b9563cc"},
        {232, "702b5a6238485f744c2c4214231886f7e7a29c85a0fbbe4212e92353ba646bfe"},
        {244, "b04ddc6229ab158d2ae1468a0b946fd4c059d439fab1b651f7c340165c968129"},
    }};
inline bool validate_queue_layout(const uintptr_t *table, const ct::steamvr::Module &module,
                                  const QueueLayout &layout) {
    if (!module.type(table, 9, layout.rtti))
        return false;
    for (size_t i = 0; i < 9; ++i) {
        if (!module.method(table[i], layout.methods[i].size, layout.methods[i].signature))
            return false;
        for (size_t j = 0; j < i; ++j)
            if (table[i] < table[j] + layout.methods[j].size &&
                table[j] < table[i] + layout.methods[i].size)
                return false;
    }
    return true;
}
} // namespace ct::steamvr
