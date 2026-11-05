#pragma once
#include "internal.hpp"
#define PCRE2_CODE_UNIT_WIDTH 16
#include <pcre2.h>
#include <array>

namespace textscope::detail {
inline int cancellationCallout(pcre2_callout_block*, void* state) {
    const auto& cancel = *static_cast<const CancelFlag*>(state);
    return cancel && cancel->load(std::memory_order_relaxed) ? PCRE2_ERROR_CALLOUT : 0;
}

class Predicate {
public:
    explicit Predicate(const Filter& filter, const CancelFlag& cancel = {});
    ~Predicate();
    Predicate(const Predicate&) = delete;
    Predicate& operator=(const Predicate&) = delete;
    bool matches(const QString& text, bool ascii);
    bool isAsciiLiteral() const { return asciiLiteral_; }
    bool matchesUtf8(QByteArrayView text, bool ascii, bool folded = false);
private:
    Filter filter_;
    CancelFlag cancel_;
    pcre2_code* code_ = nullptr;
    pcre2_match_data* data_ = nullptr;
    pcre2_match_context* context_ = nullptr;
    pcre2_jit_stack* stack_ = nullptr;
    QByteArray asciiPattern_;
    bool asciiLiteral_ = false;
    std::array<qsizetype, 128> skip_{};
    void release();
};
inline bool isAscii(const QString& text, const CancelFlag& cancel = {}) {
    for (qsizetype i = 0; i < text.size(); ++i) {
        if ((i & 65535) == 0)
            checkCancelled(cancel);
        if (text[i].unicode() >= 128)
            return false;
    }
    return true;
}
inline bool isAscii(QByteArrayView text, const CancelFlag& cancel = {}) {
    qsizetype i = 0;
    for (; i + 8 <= text.size(); i += 8) {
        if ((i & 65535) == 0)
            checkCancelled(cancel);
        std::uint64_t word;
        std::memcpy(&word, text.data() + i, sizeof(word));
        if (word & UINT64_C(0x8080808080808080))
            return false;
    }
    for (; i < text.size(); ++i)
        if (static_cast<unsigned char>(text[i]) >= 128)
            return false;
    return true;
}
}
