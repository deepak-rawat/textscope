#pragma once

#include "core.hpp"
#include <QFile>
#include <QTemporaryFile>
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace textscope::detail {

inline void checkCancelled(const CancelFlag& flag) {
    if (flag && flag->load(std::memory_order_relaxed))
        throw std::runtime_error("Cancelled");
}

inline void fail(const QString& message) {
    throw std::runtime_error(message.toUtf8().constData());
}

inline qint64 fileOffset(std::uint64_t offset) {
    if (offset > std::uint64_t(std::numeric_limits<qint64>::max()))
        throw std::runtime_error("Storage exceeds supported file size");
    return qint64(offset);
}

inline std::unique_ptr<QTemporaryFile> temporaryFile() {
    // QTemporaryFile owns an open, private file and removes it on destruction.
    auto file = std::make_unique<QTemporaryFile>(
        QStringLiteral(".textscope-XXXXXX"));
    if (!file->open())
        fail(QStringLiteral("Cannot create snapshot storage: ") + file->errorString());
    return file;
}

inline void writeAll(QFile& file, const char* data, qint64 size) {
    while (size) {
        const qint64 written = file.write(data, size);
        if (written <= 0)
            fail(QStringLiteral("Cannot write snapshot storage: ") + file.errorString());
        data += written;
        size -= written;
    }
}

// Fixed-width records permit binary search on disk without resident page tables.
template<class T> class Records {
public:
    explicit Records(std::size_t budget) : budget_(budget) {}
    void append(const T& value) {
        if (count_ >= std::uint64_t(std::numeric_limits<qint64>::max()) / sizeof(T))
            throw std::runtime_error("Record storage size overflow");
        if (!file_ && (data_.size() >= budget_ / sizeof(T) ||
            (data_.size() == data_.capacity() &&
             data_.capacity() > budget_ / sizeof(T) / 2)))
            spill();
        if (file_) {
            if (writeBuffer_.isEmpty())
                writeBuffer_.reserve(64 * 1024);
            writeBuffer_.append(reinterpret_cast<const char*>(&value), sizeof(T));
            cacheStart_ = std::numeric_limits<std::uint64_t>::max();
        } else {
            data_.push_back(value);
        }
        ++count_;
        if (writeBuffer_.size() >= 64 * 1024)
            flushPending();
    }
    T get(std::uint64_t index) const {
        if (index >= count_)
            throw std::runtime_error("Record index out of bounds");
        if (!file_)
            return data_[std::size_t(index)];
        std::lock_guard<std::mutex> lock(mutex_);
        flushPending();
        constexpr std::uint64_t perPage = 4096 / sizeof(T);
        const auto start = index / perPage * perPage;
        if (cacheStart_ != start) {
            const auto bytes = std::min(perPage, count_ - start) * sizeof(T);
            if (!file_->seek(fileOffset(start * sizeof(T))))
                fail(file_->errorString());
            cache_ = file_->read(qint64(bytes));
            if (std::uint64_t(cache_.size()) != bytes)
                throw std::runtime_error("Cannot read index storage");
            cache_.squeeze();
            cacheStart_ = start;
        }
        T value;
        std::memcpy(&value, cache_.constData() + (index - start) * sizeof(T), sizeof(T));
        return value;
    }
    std::size_t read(std::uint64_t first, T* target, std::size_t capacity) const {
        if (first >= count_ || !capacity)
            return 0;
        const auto count = std::size_t(std::min<std::uint64_t>(capacity, count_ - first));
        if (!file_) {
            std::copy_n(data_.data() + std::size_t(first), count, target);
            return count;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        flushPending();
        if (!file_->seek(fileOffset(first * sizeof(T))))
            fail(file_->errorString());
        auto* bytes = reinterpret_cast<char*>(target);
        qint64 remaining = qint64(count * sizeof(T));
        while (remaining) {
            const auto read = file_->read(bytes, remaining);
            if (read <= 0)
                throw std::runtime_error("Cannot read range block");
            bytes += read;
            remaining -= read;
        }
        return count;
    }
    void appendMany(const T* values, std::size_t count) {
        if (!count)
            return;
        if (count > std::uint64_t(std::numeric_limits<qint64>::max()) / sizeof(T) - count_)
            throw std::runtime_error("Record storage size overflow");
        if (!file_ && count_ + count > budget_ / sizeof(T))
            spill();
        if (!file_) {
            if (data_.capacity() < data_.size() + count)
                data_.reserve(std::max(data_.size() + count,
                    std::min(budget_ / sizeof(T), data_.capacity() * 2)));
            data_.insert(data_.end(), values, values + count);
        } else {
            flushPending();
            if (!file_->seek(fileOffset(count_ * sizeof(T))))
                fail(file_->errorString());
            writeAll(*file_, reinterpret_cast<const char*>(values), qint64(count * sizeof(T)));
        }
        count_ += count;
        cacheStart_ = std::numeric_limits<std::uint64_t>::max();
    }
    void replaceLast(const T& value) {
        if (!count_)
            throw std::runtime_error("Empty record storage");
        if (!file_) {
            data_.back() = value;
        } else {
            flushPending();
            if (!file_->seek(fileOffset((count_ - 1) * sizeof(T))))
                fail(file_->errorString());
            writeAll(*file_, reinterpret_cast<const char*>(&value), sizeof(T));
            cacheStart_ = std::numeric_limits<std::uint64_t>::max();
        }
    }
    std::uint64_t size() const { return count_; }
    void seal() {
        std::lock_guard<std::mutex> lock(mutex_);
        flushPending();
        writeBuffer_.squeeze();
    }
    std::size_t residentBytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sizeof(*this) + data_.capacity() * sizeof(T) +
            std::size_t(cache_.capacity()) + std::size_t(writeBuffer_.capacity());
    }
private:
    void flushPending() const {
        if (writeBuffer_.isEmpty())
            return;
        const auto first = count_ - std::uint64_t(writeBuffer_.size()) / sizeof(T);
        if (!file_->seek(fileOffset(first * sizeof(T))))
            fail(file_->errorString());
        writeAll(*file_, writeBuffer_.constData(), writeBuffer_.size());
        writeBuffer_.clear();
    }
    void spill() {
        file_ = temporaryFile();
        writeAll(*file_, reinterpret_cast<const char*>(data_.data()),
                 qint64(data_.size() * sizeof(T)));
        std::vector<T>().swap(data_);
    }
    std::size_t budget_;
    std::uint64_t count_ = 0;
    std::vector<T> data_;
    std::unique_ptr<QTemporaryFile> file_;
    mutable std::mutex mutex_;
    mutable QByteArray cache_;
    mutable QByteArray writeBuffer_;
    mutable std::uint64_t cacheStart_ = std::numeric_limits<std::uint64_t>::max();
};

}
