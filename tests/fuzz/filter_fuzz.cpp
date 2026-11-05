#include "persistence/persistence.hpp"
#include <cstddef>
#include <cstdint>
#include <stdexcept>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > 1024 * 1024) return 0;
    try {
        const auto filters = textscope::readFilters(QByteArray(reinterpret_cast<const char*>(data),
            static_cast<qsizetype>(size)));
        const auto restored = textscope::readFilters(textscope::writeFilters(filters));
        if (restored.filters.size() != filters.filters.size()) std::abort();
    } catch (const std::runtime_error&) {
        // Malformed XML and unsupported filters are expected parser rejections.
    }
    return 0;
}
