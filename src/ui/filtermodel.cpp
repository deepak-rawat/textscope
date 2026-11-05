#include "ui/ui.hpp"

#include <QDataStream>
#include <QIODevice>
#include <QMimeData>
#include <QSet>
#include <algorithm>

namespace textscope {
namespace {
const QString filterMime = QStringLiteral("application/x-textscope-filter-rows");
}

FilterModel::FilterModel(QObject* parent) : QAbstractTableModel(parent) {}
int FilterModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(filters_.size());
}
int FilterModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : 1;
}
QVariant FilterModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole) return {};
    if (orientation == Qt::Vertical) return section + 1;
    return section == 0 ? tr("Filters") : QVariant{};
}
QVariant FilterModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= int(filters_.size()) || index.column() != 0) return {};
    const auto& f = filters_[index.row()];
    if (role == Qt::CheckStateRole && index.column() == 0)
        return f.enabled ? Qt::Checked : Qt::Unchecked;
    if (role == Qt::ToolTipRole)
        return f.description.isEmpty() ? f.pattern : f.description;
    if (role == Qt::ForegroundRole && f.foreground) return *f.foreground;
    if (role == Qt::BackgroundRole && f.background) return *f.background;
    if (role == Qt::AccessibleDescriptionRole)
        return f.description;
    if (role != Qt::DisplayRole) return {};
    const auto prefix = index.row() < 26
        ? QString("%1. ").arg(QChar('a' + index.row())) : QString("%1. ").arg(index.row() + 1);
    auto summary = f.kind == Filter::Kind::Marker
        ? tr("Marked by marker %1").arg(f.marker + 1)
        : tr("Matches \"%1\" (%2%3)").arg(f.pattern,
            f.caseSensitive ? tr("case sensitive") : tr("case insensitive"),
            f.regularExpression ? tr(", regular expression") : QString());
    if (f.excluding) summary = tr("Excluding: %1").arg(summary);
    if (!f.description.isEmpty()) summary += " — " + f.description;
    const auto count = index.row() < int(counts_.size())
        ? QString::number(counts_[index.row()]) : QString("...");
    return prefix + summary + QString("  [%1 / %2]").arg(count).arg(total_);
}
Qt::ItemFlags FilterModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) return Qt::ItemIsDropEnabled;
    auto result = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
    if (index.column() == 0) result |= Qt::ItemIsUserCheckable;
    return result;
}
bool FilterModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || index.row() >= int(filters_.size()) || index.column() != 0 || role != Qt::CheckStateRole)
        return false;
    const auto checkState = value.toInt();
    if (checkState != Qt::Checked && checkState != Qt::Unchecked) return false;
    if (filters_[index.row()].enabled == (checkState == Qt::Checked)) return true;
    filters_[index.row()].enabled = checkState == Qt::Checked;
    emit dataChanged(index, index, {Qt::CheckStateRole});
    emit filtersChanged();
    return true;
}
Qt::DropActions FilterModel::supportedDropActions() const { return Qt::MoveAction; }
QStringList FilterModel::mimeTypes() const { return {filterMime}; }
QMimeData* FilterModel::mimeData(const QModelIndexList& indexes) const {
    auto* mime = new QMimeData;
    QList<int> rows;
    for (const auto& index : indexes)
        if (index.isValid() && index.row() < rowCount() && !rows.contains(index.row())) rows.append(index.row());
    std::sort(rows.begin(), rows.end());
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << quint64(reinterpret_cast<quintptr>(this)) << rows;
    mime->setData(filterMime, bytes);
    return mime;
}
bool FilterModel::dropMimeData(const QMimeData* mime, Qt::DropAction action,
    int row, int column, const QModelIndex& parent) {
    if (action == Qt::IgnoreAction) return true;
    if (action != Qt::MoveAction || column > 0 || !mime->hasFormat(filterMime)) return false;
    QByteArray bytes = mime->data(filterMime);
    if (bytes.size() < 12 || bytes.size() > 12 + qsizetype(filters_.size()) * 4) return false;
    QDataStream stream(&bytes, QIODevice::ReadOnly);
    quint64 owner = 0;
    QList<int> rows;
    stream >> owner >> rows;
    if (stream.status() != QDataStream::Ok || owner != quint64(reinterpret_cast<quintptr>(this)) || rows.isEmpty())
        return false;
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    if (rows.first() < 0 || rows.last() >= rowCount()) return false;
    int target = row < 0 ? (parent.isValid() ? parent.row() : rowCount()) : row;
    if (target < 0 || target > rowCount()) return false;
    std::vector<Filter> moved, remaining;
    std::vector<LineId> movedCounts, remainingCounts;
    for (int i = 0; i < rowCount(); ++i) {
        const bool moving = rows.contains(i);
        (moving ? moved : remaining).push_back(filters_[i]);
        (moving ? movedCounts : remainingCounts).push_back(i < int(counts_.size()) ? counts_[i] : 0);
    }
    target -= int(std::count_if(rows.begin(), rows.end(), [target](int r) { return r < target; }));
    remaining.insert(remaining.begin() + target, moved.begin(), moved.end());
    remainingCounts.insert(remainingCounts.begin() + target, movedCounts.begin(), movedCounts.end());
    beginResetModel();
    filters_ = std::move(remaining);
    counts_ = std::move(remainingCounts);
    endResetModel();
    emit filtersChanged();
    return true;
}
void FilterModel::setFilters(const std::vector<Filter>& filters) {
    const bool sameOrder = filters.size() == filters_.size() &&
        std::equal(filters.begin(), filters.end(), filters_.begin(), [](const Filter& a, const Filter& b) {
            return !a.id.isEmpty() || !b.id.isEmpty() ? a.id == b.id :
                a.kind == b.kind && a.pattern == b.pattern && a.marker == b.marker;
        });
    if (sameOrder) {
        filters_ = filters;
        if (rowCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
        return;
    }
    beginResetModel();
    filters_ = filters;
    counts_.assign(filters.size(), 0);
    endResetModel();
}
void FilterModel::setCounts(const std::vector<LineSet>& matches, LineId total) {
    total_ = total;
    counts_.clear();
    for (const auto& match : matches) counts_.push_back(match.count());
    if (rowCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, 0));
}
const std::vector<Filter>& FilterModel::filters() const { return filters_; }
}
