#include "predicate.hpp"
#include <QHash>
#include <queue>

namespace textscope {
namespace {
struct RangeReader {
    explicit RangeReader(const LineSet& value) : set(value) { load(); }
    const LineSet& set;
    std::array<std::pair<LineId, LineId>, 4096> block;
    std::uint64_t first = 0;
    std::size_t position = 0, count = 0;
    bool valid() const { return position < count; }
    const std::pair<LineId, LineId>& range() const { return block[position]; }
    void next() {
        if (++position == count) {
            first += count;
            load();
        }
    }
    void load() {
        count = set.readRanges(first, block.data(), block.size());
        position = 0;
    }
};
struct RangeWriter {
    LineSetBuilder builder;
    std::array<std::pair<LineId, LineId>, 4096> block;
    std::size_t count = 0;
    void add(LineId first, LineId last) {
        if (count && (first <= block[count - 1].second ||
            (block[count - 1].second != std::numeric_limits<LineId>::max() &&
             first == block[count - 1].second + 1))) {
            block[count - 1].second = std::max(block[count - 1].second, last);
            return;
        }
        block[count++] = {first, last};
        if (count == block.size())
            flush();
    }
    void flush() {
        builder.addRanges(block.data(), count);
        count = 0;
    }
    LineSet finish() {
        flush();
        return builder.finish();
    }
};

LineSet unite(LineId total, const std::vector<const LineSet*>& sets, const CancelFlag& cancel) {
    if (!total || sets.empty())
        return {};
    if (sets.size() == 1 && (!sets[0]->rangeCount() ||
        sets[0]->rangeAt(sets[0]->rangeCount() - 1).second < total))
        return *sets[0];
    struct Cursor {
        LineId first, last;
        std::size_t set;
        bool operator>(const Cursor& other) const { return first > other.first; }
    };
    std::vector<std::unique_ptr<RangeReader>> readers;
    readers.reserve(sets.size());
    std::priority_queue<Cursor, std::vector<Cursor>, std::greater<Cursor>> heap;
    for (std::size_t i = 0; i < sets.size(); ++i) {
        readers.push_back(std::make_unique<RangeReader>(*sets[i]));
        if (readers[i]->valid()) {
            const auto range = readers[i]->range();
            if (range.first < total)
                heap.push(Cursor{range.first, std::min(range.second, total - 1), i});
        }
    }
    RangeWriter result;
    while (!heap.empty()) {
        detail::checkCancelled(cancel);
        auto cursor = heap.top();
        heap.pop();
        result.add(cursor.first, cursor.last);
        readers[cursor.set]->next();
        if (readers[cursor.set]->valid()) {
            const auto range = readers[cursor.set]->range();
            if (range.first < total) {
                cursor.first = range.first;
                cursor.last = std::min(range.second, total - 1);
                heap.push(cursor);
            }
        }
    }
    return result.finish();
}

LineSet difference(const LineSet& include, const LineSet& exclude, const CancelFlag& cancel) {
    if (!exclude.count() || !include.count())
        return include;
    RangeWriter result;
    RangeReader inclusions(include), exclusions(exclude);
    while (inclusions.valid()) {
        detail::checkCancelled(cancel);
        const auto range = inclusions.range();
        auto first = range.first;
        bool remainder = true;
        while (exclusions.valid()) {
            detail::checkCancelled(cancel);
            const auto exclusion = exclusions.range();
            if (exclusion.second < first) {
                exclusions.next();
                continue;
            }
            if (exclusion.first > range.second)
                break;
            if (exclusion.first > first)
                result.add(first, exclusion.first - 1);
            if (exclusion.second >= range.second) {
                remainder = false;
                break;
            }
            first = exclusion.second + 1;
            exclusions.next();
        }
        if (remainder)
            result.add(first, range.second);
        inclusions.next();
    }
    return result.finish();
}
}

LineSet compose(LineId total, const std::vector<Filter>& filters,
    const std::vector<LineSet>& matches, const CancelFlag& cancel) {
    detail::checkCancelled(cancel);
    if (matches.size() < filters.size())
        throw std::runtime_error("Missing filter match sets");
    std::vector<const LineSet*> includes, excludes;
    for (std::size_t i = 0; i < filters.size(); ++i) {
        if (!filters[i].enabled)
            continue;
        (filters[i].excluding ? excludes : includes).push_back(&matches[i]);
    }
    const auto included = includes.empty() ? LineSet::all(total) : unite(total, includes, cancel);
    const auto excluded = unite(total, excludes, cancel);
    const auto output = difference(included, excluded, cancel);
    detail::checkCancelled(cancel);
    return output;
}

Analysis analyze(const std::shared_ptr<const Snapshot>& snapshot,
    const std::vector<Filter>& filters, const Markers& markers,
    const std::vector<std::optional<LineSet>>& cached, const CancelFlag& cancel,
    const Progress& progress) {
    if (!snapshot)
        throw std::runtime_error("No snapshot to analyze");
    detail::checkCancelled(cancel);
    snapshot->verifyUnchanged();
    struct Group {
        Filter filter;
        std::optional<LineSet> result;
        std::unique_ptr<detail::Predicate> predicate;
        std::unique_ptr<RangeWriter> builder;
    };
    Analysis result;
    result.matches.resize(filters.size());
    std::vector<Group> groups;
    std::vector<std::size_t> mapping;
    mapping.reserve(filters.size());
    QHash<QString, std::size_t> lookup;
    for (std::size_t i = 0; i < filters.size(); ++i) {
        detail::checkCancelled(cancel);
        const auto key = predicateKey(filters[i]);
        auto found = lookup.constFind(key);
        std::size_t index;
        if (found == lookup.cend()) {
            index = groups.size();
            lookup.insert(key, index);
            Group group;
            group.filter = filters[i];
            group.predicate = std::make_unique<detail::Predicate>(filters[i], cancel);
            if (filters[i].kind == Filter::Kind::Marker)
                group.result = markers[std::size_t(filters[i].marker)];
            groups.push_back(std::move(group));
        } else {
            index = found.value();
        }
        if (filters[i].kind == Filter::Kind::Text && i < cached.size() && cached[i])
            groups[index].result = cached[i];
        mapping.push_back(index);
    }
    std::vector<Group*> pending;
    for (auto& group : groups) {
        if (!group.result) {
            group.builder = std::make_unique<RangeWriter>();
            pending.push_back(&group);
        }
    }
    if (!pending.empty()) {
        const bool byteLiterals = snapshot->hasUtf8Storage() &&
            std::all_of(pending.begin(), pending.end(),
                [](const Group* group) { return group->predicate->isAsciiLiteral(); });
        if (byteLiterals) {
            const bool needsCaseFold = std::any_of(pending.begin(), pending.end(),
                [](const Group* group) { return !group->filter.caseSensitive; });
            const bool foldChunk = pending.size() >= 3 && std::all_of(pending.begin(), pending.end(),
                [](const Group* group) { return !group->filter.caseSensitive; });
            snapshot->forEachUtf8Line([&](LineId id, QByteArrayView text) {
                const bool ascii = !needsCaseFold || detail::isAscii(text, cancel);
                for (auto* group : pending)
                    if (group->predicate->matchesUtf8(text, ascii, foldChunk))
                        group->builder->add(id, id);
            }, cancel, progress, foldChunk);
        } else {
            snapshot->forEachLine([&](LineId id, const QString& text) {
                const bool ascii = detail::isAscii(text, cancel);
                for (auto* group : pending)
                    if (group->predicate->matches(text, ascii))
                        group->builder->add(id, id);
            }, cancel, progress);
        }
        result.scannedBytes = snapshot->byteCount();
        for (auto* group : pending)
            group->result = group->builder->finish();
    } else if (progress) {
        progress(snapshot->byteCount(), snapshot->byteCount());
    }
    for (std::size_t i = 0; i < filters.size(); ++i)
        result.matches[i] = *groups[mapping[i]].result;
    result.accepted = compose(snapshot->lineCount(), filters, result.matches, cancel);
    snapshot->verifyUnchanged();
    detail::checkCancelled(cancel);
    return result;
}

std::optional<LineId> find(const std::shared_ptr<const Snapshot>& snapshot,
    const Filter& query, LineId current, bool forward, bool wrap, const CancelFlag& cancel) {
    if (query.kind != Filter::Kind::Text)
        throw std::runtime_error("Find requires a text predicate");
    const auto result = analyze(snapshot, {query}, {}, {}, cancel);
    return result.matches.front().next(current, forward, wrap);
}
}
