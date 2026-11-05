#include "internal.hpp"

namespace textscope {
namespace {
struct Range {
    LineId first, last, prefix;
};
}

struct LineSet::Storage {
    explicit Storage(std::size_t budget = 0) : ranges(budget) {}
    detail::Records<Range> ranges;
    LineId count = 0;
};

LineSet::LineSet() : storage_(std::make_shared<Storage>()) {}
LineSet::LineSet(std::shared_ptr<const Storage> storage) : storage_(std::move(storage)) {
    if (!storage_)
        storage_ = std::make_shared<Storage>();
}
LineSet LineSet::all(LineId count) {
    LineSetBuilder builder;
    if (count)
        builder.addRange(0, count - 1);
    return builder.finish();
}
LineId LineSet::count() const { return storage_->count; }
LineId LineSet::rankBefore(LineId line) const {
    auto lo = std::uint64_t(0), hi = storage_->ranges.size();
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        if (storage_->ranges.get(mid).last < line)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == storage_->ranges.size())
        return count();
    const auto range = storage_->ranges.get(lo);
    return range.prefix + (line > range.first ? line - range.first : 0);
}
bool LineSet::contains(LineId line) const {
    auto lo = std::uint64_t(0), hi = storage_->ranges.size();
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        if (storage_->ranges.get(mid).last < line)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < storage_->ranges.size() && storage_->ranges.get(lo).first <= line;
}
LineId LineSet::at(LineId rank) const {
    if (rank >= count())
        throw std::runtime_error("Line rank out of bounds");
    auto lo = std::uint64_t(0), hi = storage_->ranges.size();
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        const auto range = storage_->ranges.get(mid);
        if (rank < range.prefix)
            hi = mid;
        else if (rank - range.prefix > range.last - range.first)
            lo = mid + 1;
        else
            return range.first + (rank - range.prefix);
    }
    throw std::runtime_error("Corrupt line set");
}
std::optional<LineId> LineSet::next(LineId line, bool forward, bool wrap) const {
    if (!count())
        return {};
    auto rank = rankBefore(line);
    if (forward) {
        if (contains(line))
            ++rank;
        if (rank < count())
            return at(rank);
        return wrap ? std::optional<LineId>(at(0)) : std::nullopt;
    }
    if (rank)
        return at(rank - 1);
    return wrap ? std::optional<LineId>(at(count() - 1)) : std::nullopt;
}
std::size_t LineSet::residentBytes() const {
    return storage_->ranges.residentBytes() + sizeof(storage_);
}
void LineSet::forEachRange(const std::function<void(LineId, LineId)>& visitor) const {
    std::array<std::pair<LineId, LineId>, 4096> block;
    for (std::uint64_t first = 0; first < rangeCount();) {
        const auto count = readRanges(first, block.data(), block.size());
        for (std::size_t i = 0; i < count; ++i)
            visitor(block[i].first, block[i].second);
        first += count;
    }
}
std::uint64_t LineSet::rangeCount() const { return storage_->ranges.size(); }
std::pair<LineId, LineId> LineSet::rangeAt(std::uint64_t index) const {
    const auto range = storage_->ranges.get(index);
    return {range.first, range.last};
}
std::size_t LineSet::readRanges(std::uint64_t first,
    std::pair<LineId, LineId>* target, std::size_t capacity) const {
    if (!capacity || first >= rangeCount())
        return 0;
    if (!target)
        throw std::runtime_error("Missing range block buffer");
    std::array<Range, 4096> records;
    const auto count = storage_->ranges.read(first, records.data(), std::min(capacity, records.size()));
    for (std::size_t i = 0; i < count; ++i)
        target[i] = {records[i].first, records[i].last};
    return count;
}

struct LineSetBuilder::Impl {
    explicit Impl(std::size_t budget) : storage(std::make_shared<LineSet::Storage>(budget)) {}
    std::shared_ptr<LineSet::Storage> storage;
    std::optional<Range> last;
    std::optional<Range> add(LineId first, LineId end) {
        if (first > end)
            throw std::runtime_error("Invalid line range");
        if (last && first < last->first)
            throw std::runtime_error("Line ranges must be added in sorted order");
        const auto maximum = std::numeric_limits<LineId>::max();
        if (last && (first <= last->last ||
            (last->last != maximum && first == last->last + 1))) {
            if (end <= last->last)
                return {};
            const auto additional = end - last->last;
            if (additional > maximum - storage->count)
                throw std::runtime_error("Line count overflow");
            storage->count += additional;
            last->last = end;
            return {};
        }
        if (end - first == maximum || end - first + 1 > maximum - storage->count)
            throw std::runtime_error("Line count overflow");
        auto previous = last;
        last = Range{first, end, storage->count};
        storage->count += end - first + 1;
        return previous;
    }
};
LineSetBuilder::LineSetBuilder(std::size_t budget) : impl_(std::make_unique<Impl>(budget)) {}
LineSetBuilder::~LineSetBuilder() = default;
LineSetBuilder::LineSetBuilder(LineSetBuilder&&) noexcept = default;
LineSetBuilder& LineSetBuilder::operator=(LineSetBuilder&&) noexcept = default;
void LineSetBuilder::add(LineId line) { addRange(line, line); }
void LineSetBuilder::addRange(LineId first, LineId last) {
    if (!impl_ || !impl_->storage)
        throw std::runtime_error("Line set builder already finished");
    if (auto previous = impl_->add(first, last))
        impl_->storage->ranges.append(*previous);
}
void LineSetBuilder::addRanges(const std::pair<LineId, LineId>* ranges, std::size_t count) {
    if (!impl_ || !impl_->storage)
        throw std::runtime_error("Line set builder already finished");
    if (count && !ranges)
        throw std::runtime_error("Missing range block buffer");
    std::array<Range, 4096> records;
    std::size_t buffered = 0;
    try {
        for (std::size_t i = 0; i < count; ++i) {
            if (auto previous = impl_->add(ranges[i].first, ranges[i].second)) {
                records[buffered++] = *previous;
                if (buffered == records.size()) {
                    impl_->storage->ranges.appendMany(records.data(), buffered);
                    buffered = 0;
                }
            }
        }
    } catch (...) {
        impl_->storage->ranges.appendMany(records.data(), buffered);
        throw;
    }
    impl_->storage->ranges.appendMany(records.data(), buffered);
}
LineSet LineSetBuilder::finish() {
    if (!impl_ || !impl_->storage)
        throw std::runtime_error("Line set builder already finished");
    if (impl_->last)
        impl_->storage->ranges.append(*impl_->last);
    impl_->storage->ranges.seal();
    LineSet result(impl_->storage);
    impl_->storage.reset();
    return result;
}
}
