#include "predicate.hpp"
#include <QDataStream>

namespace textscope {
QString predicateKey(const Filter& filter) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << quint8(filter.kind == Filter::Kind::Marker ? 1 : 0);
    if (filter.kind == Filter::Kind::Marker)
        stream << qint32(filter.marker);
    else
        stream << filter.pattern << filter.caseSensitive << filter.regularExpression;
    return QString::fromLatin1(bytes.toBase64());
}
void validateFilter(const Filter& filter) {
    detail::Predicate predicate(filter);
}
}

namespace textscope::detail {
namespace {
QString errorText(int error) {
    std::array<PCRE2_UCHAR, 256> buffer{};
    const auto size = pcre2_get_error_message(error, buffer.data(), buffer.size());
    return size >= 0 ? QString::fromUtf16(reinterpret_cast<const char16_t*>(buffer.data()), size)
                     : QString::number(error);
}
unsigned lower(unsigned c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
}
Predicate::Predicate(const Filter& filter, const CancelFlag& cancel) : filter_(filter), cancel_(cancel) {
    checkCancelled(cancel_);
    if (filter.kind == Filter::Kind::Marker) {
        if (filter.marker < 0 || filter.marker >= 8)
            throw std::runtime_error("Marker index must be between 0 and 7");
        return;
    }
    if (!filter.regularExpression) {
        asciiLiteral_ = isAscii(filter.pattern, cancel_);
        if (asciiLiteral_) {
            asciiPattern_ = filter.pattern.toLatin1();
            if (!filter.caseSensitive)
                asciiPattern_ = asciiPattern_.toLower();
            skip_.fill(asciiPattern_.size());
            for (qsizetype i = 0; i + 1 < asciiPattern_.size(); ++i)
                skip_[static_cast<unsigned char>(asciiPattern_[i])] = asciiPattern_.size() - i - 1;
        }
        return;
    }
    int error = 0;
    PCRE2_SIZE offset = 0;
    const auto options = PCRE2_UTF | PCRE2_UCP | PCRE2_AUTO_CALLOUT |
        (filter.caseSensitive ? 0u : PCRE2_CASELESS);
    code_ = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(filter.pattern.utf16()),
        PCRE2_SIZE(filter.pattern.size()), options, &error, &offset, nullptr);
    if (!code_)
        fail(QStringLiteral("Regex error at UTF-16 offset %1: %2").arg(qulonglong(offset)).arg(errorText(error)));
    try {
        data_ = pcre2_match_data_create_from_pattern(code_, nullptr);
        context_ = pcre2_match_context_create(nullptr);
        if (!data_ || !context_)
            throw std::runtime_error("Cannot allocate regex matching state");
        pcre2_set_match_limit(context_, 1000000);
        pcre2_set_depth_limit(context_, 1000);
        pcre2_set_heap_limit(context_, 8192);
        pcre2_set_callout(context_, &cancellationCallout, &cancel_);
        const auto jit = pcre2_jit_compile(code_, PCRE2_JIT_COMPLETE);
        if (jit == 0) {
            stack_ = pcre2_jit_stack_create(32 * 1024, 1024 * 1024, nullptr);
            if (!stack_)
                throw std::runtime_error("Cannot allocate regex JIT stack");
            pcre2_jit_stack_assign(context_, nullptr, stack_);
        } else if (jit != PCRE2_ERROR_JIT_BADOPTION && jit != PCRE2_ERROR_NOMEMORY) {
            fail(QStringLiteral("Regex JIT error: ") + errorText(jit));
        }
    } catch (...) {
        release();
        throw;
    }
}
void Predicate::release() {
    pcre2_jit_stack_free(stack_);
    pcre2_match_context_free(context_);
    pcre2_match_data_free(data_);
    pcre2_code_free(code_);
}
Predicate::~Predicate() { release(); }
bool Predicate::matches(const QString& text, bool ascii) {
    checkCancelled(cancel_);
    if (filter_.regularExpression) {
        const auto result = pcre2_match(code_, reinterpret_cast<PCRE2_SPTR>(text.utf16()),
            PCRE2_SIZE(text.size()), 0, 0, data_, context_);
        checkCancelled(cancel_);
        if (result == PCRE2_ERROR_NOMATCH)
            return false;
        if (result < 0)
            fail(QStringLiteral("Regex matching error: ") + errorText(result));
        return true;
    }
    if (filter_.pattern.isEmpty())
        return true;
    if (filter_.caseSensitive || !ascii || asciiPattern_.isEmpty())
        return text.contains(filter_.pattern, filter_.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
    const auto length = asciiPattern_.size();
    qsizetype end = length - 1;
    qsizetype checked = 0;
    while (end < text.size()) {
        qsizetype j = length - 1;
        while (j >= 0 && lower(text[end - (length - 1 - j)].unicode()) ==
               static_cast<unsigned char>(asciiPattern_[j])) {
            if ((j & 65535) == 0)
                checkCancelled(cancel_);
            --j;
        }
        if (j < 0)
            return true;
        end += skip_[lower(text[end].unicode())];
        if (end - checked >= 65536) {
            checkCancelled(cancel_);
            checked = end;
        }
    }
    return false;
}
bool Predicate::matchesUtf8(QByteArrayView text, bool ascii, bool folded) {
    checkCancelled(cancel_);
    if (asciiPattern_.isEmpty())
        return true;
    if (filter_.caseSensitive || (folded && ascii))
        return text.indexOf(QByteArrayView(asciiPattern_)) >= 0;
    if (!ascii)
        return QString::fromUtf8(text).contains(filter_.pattern, Qt::CaseInsensitive);
    const auto length = asciiPattern_.size();
    qsizetype end = length - 1, checked = 0;
    while (end < text.size()) {
        qsizetype j = length - 1;
        while (j >= 0 && lower(static_cast<unsigned char>(text[end - (length - 1 - j)])) ==
               static_cast<unsigned char>(asciiPattern_[j])) {
            if ((j & 65535) == 0)
                checkCancelled(cancel_);
            --j;
        }
        if (j < 0)
            return true;
        const auto shift = skip_[lower(static_cast<unsigned char>(text[end]))];
        if (shift > text.size() - 1 - end)
            break;
        end += shift;
        if (end - checked >= 65536) {
            checkCancelled(cancel_);
            checked = end;
        }
    }
    return false;
}
}
