#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QColor>
#include <QString>
#include <QMap>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace textscope {

using LineId = std::uint64_t;
using CancelFlag = std::shared_ptr<std::atomic_bool>;
using Progress = std::function<void(std::uint64_t, std::uint64_t)>;

struct Filter {
    enum class Kind { Text, Marker };
    QString id;
    Kind kind = Kind::Text;
    QString pattern;
    QString description;
    bool enabled = true;
    bool excluding = false;
    bool caseSensitive = false;
    bool regularExpression = false;
    int marker = 0;
    std::optional<QColor> foreground;
    std::optional<QColor> background;
    QMap<QString, QString> extraAttributes;
};

QString predicateKey(const Filter& filter);
void validateFilter(const Filter& filter);

class LineSet {
public:
    LineSet();
    static LineSet all(LineId count);
    LineId count() const;
    bool contains(LineId line) const;
    LineId at(LineId rank) const;
    LineId rankBefore(LineId line) const;
    std::optional<LineId> next(LineId line, bool forward, bool wrap = true) const;
    std::size_t residentBytes() const;
    void forEachRange(const std::function<void(LineId, LineId)>& visitor) const;
    std::uint64_t rangeCount() const;
    std::pair<LineId, LineId> rangeAt(std::uint64_t index) const;
    std::size_t readRanges(std::uint64_t first,
        std::pair<LineId, LineId>* target, std::size_t capacity) const;
    struct Storage;
    explicit LineSet(std::shared_ptr<const Storage> storage);
private:
    std::shared_ptr<const Storage> storage_;
};

class LineSetBuilder {
public:
    explicit LineSetBuilder(std::size_t memoryBudget = 8 * 1024 * 1024);
    ~LineSetBuilder();
    LineSetBuilder(LineSetBuilder&&) noexcept;
    LineSetBuilder& operator=(LineSetBuilder&&) noexcept;
    void add(LineId line);
    void addRange(LineId first, LineId lastInclusive);
    void addRanges(const std::pair<LineId, LineId>* ranges, std::size_t count);
    LineSet finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

using Markers = std::array<LineSet, 8>;

struct OpenOptions {
    QByteArray encoding = "UTF-8";
    bool direct = false;
    std::size_t memoryBudget = 32 * 1024 * 1024;
};

class Snapshot {
public:
    static std::shared_ptr<const Snapshot> open(const QString& path,
        const OpenOptions& options = {}, const CancelFlag& cancel = {},
        const Progress& progress = {});
    static std::shared_ptr<const Snapshot> fromText(const QString& text,
        const CancelFlag& cancel = {});
    ~Snapshot();
    LineId lineCount() const;
    std::uint64_t byteCount() const;
    QString name() const;
    QByteArray encoding() const;
    bool isDirect() const;
    QString line(LineId index, qsizetype maxCharacters = 0) const;
    std::vector<QString> lines(LineId first, LineId count,
        qsizetype maxCharacters = 0) const;
    void verifyUnchanged() const;
    std::size_t residentBytes() const;
    std::uint64_t indexBytes() const;
    void forEachLine(const std::function<void(LineId, const QString&)>& visitor,
        const CancelFlag& cancel = {}, const Progress& progress = {}) const;
    bool hasUtf8Storage() const;
    void forEachUtf8Line(const std::function<void(LineId, QByteArrayView)>& visitor,
        const CancelFlag& cancel = {}, const Progress& progress = {},
        bool foldAscii = false) const;
    struct Impl;
    explicit Snapshot(std::unique_ptr<Impl> impl);
private:
    std::unique_ptr<Impl> impl_;
};

struct Analysis {
    std::vector<LineSet> matches;
    LineSet accepted;
    std::uint64_t scannedBytes = 0;
};

Analysis analyze(const std::shared_ptr<const Snapshot>& snapshot,
    const std::vector<Filter>& filters, const Markers& markers,
    const std::vector<std::optional<LineSet>>& cached = {},
    const CancelFlag& cancel = {}, const Progress& progress = {});
LineSet compose(LineId total, const std::vector<Filter>& filters,
    const std::vector<LineSet>& matches, const CancelFlag& cancel = {});
std::optional<LineId> find(const std::shared_ptr<const Snapshot>& snapshot,
    const Filter& query, LineId current, bool forward, bool wrap,
    const CancelFlag& cancel = {});

}
