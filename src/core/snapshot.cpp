#include "internal.hpp"
#include <QDateTime>
#include <QFileInfo>
#include <QtCore5Compat/QTextCodec>
#include <sys/stat.h>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <io.h>
#endif

namespace textscope {
namespace {
constexpr qint64 chunkSize = 256 * 1024;
struct LineRecord { std::uint64_t first, end; };

QString decode(QTextCodec* codec, QTextCodec::ConverterState& state,
    const QByteArray& data, bool final = false) {
    auto text = codec->toUnicode(data.constData(), data.size(), &state);
    if (final && state.remainingChars) {
        text += QChar::ReplacementCharacter;
        state.remainingChars = 0;
    }
    return text;
}

struct Signature {
#ifdef Q_OS_WIN
    BY_HANDLE_FILE_INFORMATION information{};
    FILE_BASIC_INFO basic{};
    static Signature read(const QFile& file) {
        Signature result;
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()));
        if (!GetFileInformationByHandle(handle, &result.information) ||
            !GetFileInformationByHandleEx(handle, FileBasicInfo, &result.basic, sizeof(result.basic)))
            detail::fail(QString("Cannot inspect source file (Windows error %1)").arg(GetLastError()));
        return result;
    }
#else
    struct stat value{};
    static Signature read(const QFile& file) {
        Signature result;
        if (::fstat(file.handle(), &result.value))
            throw std::runtime_error("Cannot inspect source file");
        return result;
    }
#endif
    static Signature read(const QString& path) {
#ifdef Q_OS_WIN
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) detail::fail(file.errorString());
        return read(file);
#else
        Signature result;
        if (::stat(QFile::encodeName(path).constData(), &result.value))
            throw std::runtime_error("Source file is unavailable or changed");
        return result;
#endif
    }
    bool operator==(const Signature& other) const {
#ifdef Q_OS_WIN
        const auto& a = information;
        const auto& b = other.information;
        return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
            a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow &&
            a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
            basic.LastWriteTime.QuadPart == other.basic.LastWriteTime.QuadPart &&
            basic.ChangeTime.QuadPart == other.basic.ChangeTime.QuadPart;
#else
        const auto& a = value;
        const auto& b = other.value;
#ifdef Q_OS_MACOS
        const bool times = a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec &&
            a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
            a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec &&
            a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
#else
        const bool times = a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
            a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
            a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
            a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
#endif
        return times && a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
            a.st_size == b.st_size;
#endif
    }
};
}

struct Snapshot::Impl {
    explicit Impl(std::size_t budget) : index(budget / 4), byteBudget(budget - budget / 4) {}
    detail::Records<LineRecord> index;
    std::size_t byteBudget;
    QByteArray memory;
    std::unique_ptr<QTemporaryFile> disk;
    mutable std::unique_ptr<QFile> source;
    mutable std::mutex mutex;
    QString path;
    QByteArray codecName;
    QTextCodec* codec = nullptr;
    QTextCodec* inputCodec = nullptr;
    Signature signature;
    std::uint64_t bytes = 0;
    std::uint64_t storedBytes = 0;
    int bom = 0;
    int unit = 1;
    bool little = true;
    bool direct = false;
    unsigned cr = 13, lf = 10;

    void append(const QByteArray& chunk) {
        if (direct)
            return;
        if (!disk && std::uint64_t(memory.size()) + std::uint64_t(chunk.size()) > byteBudget) {
            disk = detail::temporaryFile();
            detail::writeAll(*disk, memory.constData(), memory.size());
            memory.clear();
            memory.squeeze();
        }
        if (disk)
            detail::writeAll(*disk, chunk.constData(), chunk.size());
        else
            memory.append(chunk);
    }
    QByteArray read(std::uint64_t offset, qint64 length) const {
        if (offset > storedBytes || length < 0)
            throw std::runtime_error("Snapshot byte offset out of bounds");
        length = qint64(std::min<std::uint64_t>(std::uint64_t(length), storedBytes - offset));
        if (!direct && !disk)
            return memory.mid(detail::fileOffset(offset), length);
        std::lock_guard<std::mutex> lock(mutex);
        QFile* file = direct ? source.get() : disk.get();
        if (!file->seek(detail::fileOffset(offset)))
            detail::fail(file->errorString());
        auto result = file->read(length);
        if (result.size() != length)
            throw std::runtime_error("Cannot read snapshot; source may have changed");
        return result;
    }
    void selectCodec(const QByteArray& header, const QByteArray& requested) {
        codecName = requested;
        if (header.startsWith(QByteArray::fromHex("0000feff"))) {
            codecName = "UTF-32BE"; bom = 4;
        } else if (header.startsWith(QByteArray::fromHex("fffe0000"))) {
            codecName = "UTF-32LE"; bom = 4;
        } else if (header.startsWith(QByteArray::fromHex("efbbbf"))) {
            codecName = "UTF-8"; bom = 3;
        } else if (header.startsWith(QByteArray::fromHex("feff"))) {
            codecName = "UTF-16BE"; bom = 2;
        } else if (header.startsWith(QByteArray::fromHex("fffe"))) {
            codecName = "UTF-16LE"; bom = 2;
        }
        codec = QTextCodec::codecForName(codecName);
        if (!codec)
            detail::fail(QStringLiteral("Unknown encoding: ") + QString::fromLatin1(codecName));
        codecName = codec->name();
        const auto normalized = codecName.toUpper();
        if (normalized.startsWith("UTF-16") || normalized.startsWith("UTF-32")) {
            unit = normalized.startsWith("UTF-16") ? 2 : 4;
            little = normalized.endsWith("LE") ||
                (!normalized.endsWith("BE") && Q_BYTE_ORDER == Q_LITTLE_ENDIAN);
            codecName = QByteArray(unit == 2 ? "UTF-16" : "UTF-32") + (little ? "LE" : "BE");
            codec = QTextCodec::codecForName(codecName);
        } else {
            if (!direct && normalized != "UTF-8") {
                inputCodec = codec;
                codec = QTextCodec::codecForName("UTF-8");
                bom = 0;
                return;
            }
            if (direct && (normalized.contains("2022") || normalized.contains("UTF-7") ||
                normalized.startsWith("HZ") || normalized.contains("SCSU") ||
                normalized.contains("BOCU")))
                throw std::runtime_error("Stateful encodings require an immutable snapshot, not direct access");
            const auto newline = codec->fromUnicode(QStringLiteral("\n"));
            const auto carriage = codec->fromUnicode(QStringLiteral("\r"));
            if (newline.size() != 1 || carriage.size() != 1)
                throw std::runtime_error("Encoding does not support byte-indexed line boundaries");
            lf = static_cast<unsigned char>(newline[0]);
            cr = static_cast<unsigned char>(carriage[0]);
        }
    }
    void capture(QFile& file, const CancelFlag& cancel, const Progress& progress) {
        std::uint64_t offset = 0, first = std::uint64_t(bom);
        std::optional<std::uint64_t> pendingCR;
        QByteArray carry;
        auto add = [&](std::uint64_t end) { index.append(LineRecord{first, end}); };
        auto consume = [&](const QByteArray& chunk) {
            if (chunk.isEmpty())
                return;
            append(chunk);
            const auto chunkFirst = offset;
            offset += std::uint64_t(chunk.size());
            auto data = carry + chunk;
            const auto dataFirst = chunkFirst - std::uint64_t(carry.size());
            qsizetype cursor = dataFirst < std::uint64_t(bom)
                ? qsizetype(std::uint64_t(bom) - dataFirst) : 0;
            for (; cursor + unit <= data.size(); cursor += unit) {
                const auto position = dataFirst + std::uint64_t(cursor);
                unsigned code = 0;
                for (int j = 0; j < unit; ++j) {
                    const auto byte = static_cast<unsigned char>(data[cursor + j]);
                    code |= unsigned(byte) << (8 * (little ? j : unit - j - 1));
                }
                if (pendingCR) {
                    add(*pendingCR);
                    first = *pendingCR + std::uint64_t(unit);
                    pendingCR.reset();
                    if (code == lf) {
                        first = position + std::uint64_t(unit);
                        continue;
                    }
                }
                if (code == cr)
                    pendingCR = position;
                else if (code == lf) {
                    add(position);
                    first = position + std::uint64_t(unit);
                }
                if ((cursor & 65535) == 0)
                    detail::checkCancelled(cancel);
            }
            carry = data.mid(cursor);
        };
        std::uint64_t inputOffset = 0;
        QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);
        while (!file.atEnd()) {
            detail::checkCancelled(cancel);
            const auto chunk = file.read(chunkSize);
            if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
                detail::fail(file.errorString());
            if (chunk.isEmpty())
                break;
            inputOffset += std::uint64_t(chunk.size());
            consume(inputCodec ? decode(inputCodec, state, chunk).toUtf8() : chunk);
            if (progress)
                progress(inputOffset, bytes);
        }
        if (inputCodec)
            consume(decode(inputCodec, state, {}, true).toUtf8());
        storedBytes = offset;
        if (inputOffset != bytes)
            throw std::runtime_error("Source file changed during capture");
        if (pendingCR) {
            add(*pendingCR);
            first = *pendingCR + std::uint64_t(unit);
        }
        if (first < storedBytes)
            add(storedBytes);
        index.seal();
        if (disk && !disk->flush())
            detail::fail(disk->errorString());
        detail::checkCancelled(cancel);
    }
};

Snapshot::Snapshot(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
    if (!impl_)
        throw std::runtime_error("Missing snapshot storage");
}
Snapshot::~Snapshot() = default;
std::shared_ptr<const Snapshot> Snapshot::open(const QString& path, const OpenOptions& options,
    const CancelFlag& cancel, const Progress& progress) {
    detail::checkCancelled(cancel);
    auto impl = std::make_unique<Impl>(options.memoryBudget);
    impl->path = QFileInfo(path).absoluteFilePath();
    impl->codecName = options.encoding;
    impl->direct = options.direct;
    impl->signature = Signature::read(impl->path);
    impl->source = std::make_unique<QFile>(impl->path);
    if (!impl->source->open(QIODevice::ReadOnly))
        detail::fail(impl->source->errorString());
    const auto openedSignature = Signature::read(*impl->source);
    if (!(openedSignature == impl->signature))
        throw std::runtime_error("Source file changed while opening");
    impl->bytes = std::uint64_t(impl->source->size());
    impl->selectCodec(impl->source->peek(4), options.encoding);
    if (!impl->direct) {
        const auto bound = impl->inputCodec
            ? (impl->bytes > std::uint64_t(std::numeric_limits<qint64>::max()) / 4
                ? std::uint64_t(std::numeric_limits<qint64>::max()) : impl->bytes * 4)
            : impl->bytes;
        if (bound > impl->byteBudget)
            impl->disk = detail::temporaryFile();
        else
            impl->memory.reserve(detail::fileOffset(bound));
    }
    impl->capture(*impl->source, cancel, progress);
    if (!(Signature::read(impl->path) == impl->signature))
        throw std::runtime_error("Source file changed during capture");
    if (!impl->direct)
        impl->source.reset();
    return std::make_shared<Snapshot>(std::move(impl));
}
std::shared_ptr<const Snapshot> Snapshot::fromText(const QString& text, const CancelFlag& cancel) {
    detail::checkCancelled(cancel);
    auto file = detail::temporaryFile();
    const auto data = text.toUtf8();
    detail::writeAll(*file, data.constData(), data.size());
    if (!file->flush())
        detail::fail(file->errorString());
    auto snapshot = open(file->fileName(), {}, cancel);
    auto* mutableSnapshot = const_cast<Snapshot*>(snapshot.get());
    mutableSnapshot->impl_->path = QStringLiteral("Untitled");
    return snapshot;
}
LineId Snapshot::lineCount() const { return impl_->index.size(); }
std::uint64_t Snapshot::byteCount() const { return impl_->bytes; }
QString Snapshot::name() const { return impl_->path; }
QByteArray Snapshot::encoding() const { return impl_->codecName; }
bool Snapshot::isDirect() const { return impl_->direct; }
void Snapshot::verifyUnchanged() const {
    if (impl_->direct && !(Signature::read(impl_->path) == impl_->signature))
        throw std::runtime_error("Source file changed; reopen it before continuing");
}
std::size_t Snapshot::residentBytes() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return sizeof(Impl) + std::size_t(impl_->memory.capacity()) + impl_->index.residentBytes();
}
std::uint64_t Snapshot::indexBytes() const { return impl_->index.size() * sizeof(LineRecord); }
QString Snapshot::line(LineId index, qsizetype maxCharacters) const {
    verifyUnchanged();
    const auto record = impl_->index.get(index);
    QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);
    QString result;
    auto offset = record.first;
    while (offset < record.end && (maxCharacters <= 0 || result.size() < maxCharacters)) {
        const auto wanted = maxCharacters > 0
            ? std::max<qint64>(impl_->unit, std::min<qsizetype>(1024, maxCharacters - result.size()) * 4)
            : chunkSize;
        const auto data = impl_->read(offset, qint64(std::min<std::uint64_t>(
            std::uint64_t(wanted), record.end - offset)));
        result += decode(impl_->codec, state, data);
        offset += std::uint64_t(data.size());
    }
    if (offset == record.end)
        result += decode(impl_->codec, state, {}, true);
    if (maxCharacters > 0 && result.size() > maxCharacters)
        result.truncate(maxCharacters);
    verifyUnchanged();
    return result;
}
std::vector<QString> Snapshot::lines(LineId first, LineId count, qsizetype maxCharacters) const {
    if (first > lineCount())
        throw std::runtime_error("Line index out of bounds");
    count = std::min(count, lineCount() - first);
    std::vector<QString> result;
    if (count > result.max_size())
        throw std::runtime_error("Too many lines to render");
    result.reserve(std::size_t(count));
    for (LineId i = 0; i < count; ++i)
        result.push_back(line(first + i, maxCharacters));
    return result;
}
void Snapshot::forEachLine(const std::function<void(LineId, const QString&)>& visitor,
    const CancelFlag& cancel, const Progress& progress) const {
    verifyUnchanged();
    detail::checkCancelled(cancel);
    QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);
    QString pending;
    LineId id = 0;
    bool skipLF = false;
    auto consume = [&](const QString& text) {
        qsizetype first = 0;
        for (qsizetype i = 0; i < text.size(); ++i) {
            if ((i & 65535) == 0)
                detail::checkCancelled(cancel);
            const auto ch = text[i];
            if (skipLF) {
                skipLF = false;
                if (ch == QLatin1Char('\n')) {
                    first = i + 1;
                    continue;
                }
            }
            if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r')) {
                pending += QStringView(text).mid(first, i - first);
                visitor(id++, pending);
                pending.clear();
                skipLF = ch == QLatin1Char('\r');
                first = i + 1;
            }
        }
        pending += QStringView(text).mid(first);
    };
    for (std::uint64_t offset = std::uint64_t(impl_->bom); offset < impl_->storedBytes;) {
        detail::checkCancelled(cancel);
        const auto data = impl_->read(offset, chunkSize);
        consume(decode(impl_->codec, state, data));
        offset += std::uint64_t(data.size());
        if (progress)
            progress(offset >= impl_->storedBytes ? impl_->bytes :
                std::uint64_t(static_cast<long double>(offset) * impl_->bytes / impl_->storedBytes),
                impl_->bytes);
    }
    consume(decode(impl_->codec, state, {}, true));
    if (!pending.isEmpty())
        visitor(id++, pending);
    if (id != lineCount())
        throw std::runtime_error("Encoding line boundaries are inconsistent");
    verifyUnchanged();
    detail::checkCancelled(cancel);
}
bool Snapshot::hasUtf8Storage() const { return impl_->codec->name() == "UTF-8"; }
void Snapshot::forEachUtf8Line(const std::function<void(LineId, QByteArrayView)>& visitor,
    const CancelFlag& cancel, const Progress& progress, bool foldAscii) const {
    if (!hasUtf8Storage())
        throw std::runtime_error("Byte-line scanning requires UTF-8 storage");
    verifyUnchanged();
    detail::checkCancelled(cancel);
    QByteArray pending;
    LineId id = 0;
    bool skipLF = false;
    for (std::uint64_t offset = std::uint64_t(impl_->bom); offset < impl_->storedBytes;) {
        detail::checkCancelled(cancel);
        auto data = impl_->read(offset, chunkSize);
        if (foldAscii) {
            auto* bytes = data.data();
            for (qsizetype i = 0; i < data.size(); ++i) {
                const auto ch = static_cast<unsigned char>(bytes[i]);
                bytes[i] = char(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
            }
        }
        const char* begin = data.constData();
        const char* end = begin + data.size();
        const char* first = begin;
        const char* lf = static_cast<const char*>(std::memchr(begin, '\n', data.size()));
        const char* cr = static_cast<const char*>(std::memchr(begin, '\r', data.size()));
        while (lf || cr) {
            detail::checkCancelled(cancel);
            const char* boundary = !cr ? lf : !lf ? cr : std::min(lf, cr);
            if (skipLF && boundary == first && *boundary == '\n') {
                skipLF = false;
            } else {
                if (pending.isEmpty()) {
                    visitor(id++, QByteArrayView(first, boundary - first));
                } else {
                    pending.append(first, boundary - first);
                    visitor(id++, QByteArrayView(pending));
                    pending.clear();
                }
                skipLF = *boundary == '\r';
            }
            first = boundary + 1;
            if (boundary == lf)
                lf = static_cast<const char*>(std::memchr(first, '\n', end - first));
            if (boundary == cr)
                cr = static_cast<const char*>(std::memchr(first, '\r', end - first));
        }
        if (first < end) {
            pending.append(first, end - first);
            skipLF = false;
        }
        offset += std::uint64_t(data.size());
        if (progress)
            progress(offset >= impl_->storedBytes ? impl_->bytes :
                std::uint64_t(static_cast<long double>(offset) * impl_->bytes / impl_->storedBytes),
                impl_->bytes);
    }
    if (!pending.isEmpty())
        visitor(id++, QByteArrayView(pending));
    if (id != lineCount())
        throw std::runtime_error("Encoding line boundaries are inconsistent");
    verifyUnchanged();
    detail::checkCancelled(cancel);
}
}
