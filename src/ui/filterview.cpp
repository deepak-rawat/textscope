#include "filterview.hpp"
#include "ui/ui.hpp"

#include <QHeaderView>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyledItemDelegate>

namespace textscope {
namespace {
QRect checkRect(const QStyleOptionViewItem& option, const QWidget* widget) {
    const int size = std::max(1, std::min({
        widget->style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, widget),
        widget->style()->pixelMetric(QStyle::PM_IndicatorHeight, nullptr, widget),
        option.rect.height() - 4}));
    return {option.rect.left() + 3, option.rect.top() + (option.rect.height() - size) / 2, size, size};
}

class FilterDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        const auto background = index.data(Qt::BackgroundRole).value<QColor>();
        const auto foreground = index.data(Qt::ForegroundRole).value<QColor>();
        const bool colored = background.isValid() || foreground.isValid();
        const bool selected = item.state.testFlag(QStyle::State_Selected);
        const auto rowColor = background.isValid() ? background
            : item.palette.color(selected && !colored ? QPalette::Highlight : QPalette::Base);
        painter->save();
        painter->fillRect(item.rect, rowColor);
        const auto check = checkRect(item, option.widget);
        const bool checked = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
        const auto checkColor = checked ? item.palette.color(QPalette::Highlight) : QColor(Qt::white);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(qGray(rowColor.rgb()) < 128 ? Qt::white : Qt::black, 1));
        painter->setBrush(checkColor);
        painter->drawRoundedRect(QRectF(check).adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
        if (checked) {
            QPainterPath tick;
            tick.moveTo(check.left() + check.width() * 0.2, check.top() + check.height() * 0.5);
            tick.lineTo(check.left() + check.width() * 0.43, check.top() + check.height() * 0.72);
            tick.lineTo(check.left() + check.width() * 0.8, check.top() + check.height() * 0.27);
            painter->setPen(QPen(qGray(checkColor.rgb()) < 128 ? Qt::white : Qt::black,
                std::max(1.5, check.width() / 8.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->drawPath(tick);
        }
        painter->restore();
        const auto textColor = foreground.isValid() ? foreground
            : colored ? item.palette.color(QPalette::Text)
            : item.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        painter->setPen(textColor);
        painter->setFont(item.font);
        const auto textRect = item.rect.adjusted(check.width() + 9, 0, -4, 0);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
            item.fontMetrics.elidedText(item.text, Qt::ElideRight, textRect.width()));
        if (selected) {
            painter->setPen(QPen(textColor, 1, Qt::DotLine));
            painter->drawRect(item.rect.adjusted(0, 0, -1, -1));
        }
        painter->restore();
    }
    bool editorEvent(QEvent*, QAbstractItemModel*, const QStyleOptionViewItem&, const QModelIndex&) override {
        // Checkbox changes are handled by FilterView so a click cannot also edit/toggle twice.
        return false;
    }
};
}

FilterView::FilterView(QWidget* parent) : QTableView(parent) {
    setItemDelegate(new FilterDelegate(this));
    setShowGrid(false);
    setAlternatingRowColors(false);
    horizontalHeader()->hide();
    verticalHeader()->hide();
    horizontalHeader()->setStretchLastSection(true);
    verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    verticalHeader()->setDefaultSectionSize(fontMetrics().height() + 4);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setWordWrap(false);
}

bool FilterView::checkboxAt(const QPoint& position) const {
    const auto index = indexAt(position);
    if (!index.isValid()) return false;
    QStyleOptionViewItem option;
    option.rect = visualRect(index);
    return checkRect(option, this).adjusted(-3, -3, 3, 3).contains(position);
}

void FilterView::mousePressEvent(QMouseEvent* event) {
    checkPressed_ = event->button() == Qt::LeftButton && checkboxAt(event->position().toPoint());
    pressedIndex_ = checkPressed_ ? indexAt(event->position().toPoint()) : QModelIndex();
    QTableView::mousePressEvent(event);
}

void FilterView::mouseReleaseEvent(QMouseEvent* event) {
    const auto pressed = pressedIndex_;
    const bool toggle = checkPressed_ && event->button() == Qt::LeftButton &&
        checkboxAt(event->position().toPoint()) && indexAt(event->position().toPoint()) == pressed;
    checkPressed_ = false;
    pressedIndex_ = QModelIndex();
    QTableView::mouseReleaseEvent(event);
    if (toggle && pressed.isValid())
        model()->setData(pressed, pressed.data(Qt::CheckStateRole).toInt() == Qt::Checked
            ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
}

void FilterView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (checkboxAt(event->position().toPoint())) {
        checkPressed_ = false;
        pressedIndex_ = QModelIndex();
        event->accept();
        return;
    }
    QTableView::mouseDoubleClickEvent(event);
}

void FilterView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space && event->modifiers() == Qt::NoModifier) {
        const auto rows = selectionModel()->selectedRows();
        // Snapshot the indexes first: updates may synchronously refresh the view.
        for (const auto& index : rows)
            model()->setData(index, index.data(Qt::CheckStateRole).toInt() == Qt::Checked
                ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
        event->accept();
    } else if ((event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return) && command) {
        command("edit-filter");
        event->accept();
    } else if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && command) {
        command("remove-filter");
        event->accept();
    } else if (event->modifiers() == commandModifier() && command &&
        event->key() >= Qt::Key_Left && event->key() <= Qt::Key_Down) {
        const auto key = event->key();
        command(key == Qt::Key_Left ? "dock-left" : key == Qt::Key_Right ? "dock-right"
            : key == Qt::Key_Up ? "dock-top" : "dock-bottom");
        event->accept();
    } else QTableView::keyPressEvent(event);
}
}
