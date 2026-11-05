#include "ui/ui.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QIcon>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSet>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <exception>

namespace textscope {
namespace {
class ColorDelegate final : public QStyledItemDelegate {
public:
    ColorDelegate(bool background, QObject* parent) : QStyledItemDelegate(parent), background_(background) {}
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(option.fontMetrics.height() + 4);
        return size;
    }
protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        const QColor color = index.data(Qt::UserRole).value<QColor>();
        if (!color.isValid()) return;
        if (!background_) return;
        option->backgroundBrush = color;
        option->palette.setColor(QPalette::Text, qGray(color.rgb()) < 128 ? Qt::white : Qt::black);
        option->palette.setColor(QPalette::Highlight, color);
        option->palette.setColor(QPalette::HighlightedText, option->palette.color(QPalette::Text));
    }
private:
    bool background_;
};

QComboBox* colorPicker(QDialog& dialog, const QString& name, const QString& title,
    std::optional<QColor>& value, const std::function<void()>& preview, bool background) {
    auto* combo = new QComboBox(&dialog);
    combo->setObjectName(name);
    combo->setAccessibleName(title);
    combo->setItemDelegate(new ColorDelegate(background, combo));
    combo->setMaxVisibleItems(20);
    combo->addItem(QObject::tr("[Default]"));
    const auto addColor = [combo, background](const QString& label, const QColor& color, int position) {
        QIcon icon;
        if (!background) {
            QPixmap swatch(16, 16);
            swatch.fill(color);
            {
                QPainter painter(&swatch);
                painter.setPen(Qt::gray);
                painter.drawRect(0, 0, 15, 15);
            }
            icon = QIcon(swatch);
        }
        combo->insertItem(position, icon, label, color);
    };
    QSet<QRgb> colors;
    for (const auto& name : QColor::colorNames()) {
        if (name == "transparent") continue;
        const QColor color(name);
        if (colors.contains(color.rgba())) continue;
        colors.insert(color.rgba());
        QString label = name;
        label[0] = label[0].toUpper();
        addColor(label, color, combo->count());
    }
    const auto selectColor = [combo, background, addColor](const std::optional<QColor>& color) {
        QSignalBlocker blocker(combo);
        int index = 0;
        if (color) {
            index = combo->findData(*color);
            if (index < 0) {
                addColor(color->name(), *color, combo->count() - 1);
                index = combo->count() - 2;
            }
        }
        combo->setCurrentIndex(index);
        auto palette = combo->palette();
        const auto base = combo->parentWidget()->palette();
        if (!background) {
            combo->setPalette(base);
            return;
        }
        const auto buttonColor = color.value_or(base.color(QPalette::Button));
        const auto textColor = color
            ? (qGray(color->rgb()) < 128 ? QColor(Qt::white) : QColor(Qt::black))
            : base.color(QPalette::ButtonText);
        palette.setColor(QPalette::Button, buttonColor);
        palette.setColor(QPalette::ButtonText, textColor);
        combo->setPalette(palette);
    };
    combo->addItem(QObject::tr("Custom..."));
    selectColor(value);
    QObject::connect(combo, &QComboBox::currentIndexChanged, &dialog,
        [combo, &dialog, valuePtr = &value, preview, selectColor, title](int index) {
            if (index == combo->count() - 1) {
                QColorDialog picker(valuePtr->value_or(Qt::white), &dialog);
                picker.setObjectName("filterColorDialog");
                picker.setWindowTitle(title);
                // Native macOS color panels are shared/modeless; this selection must be transactional.
                picker.setOption(QColorDialog::DontUseNativeDialog);
                if (picker.exec() == QDialog::Accepted) *valuePtr = picker.selectedColor();
            } else {
                const auto color = combo->itemData(index).value<QColor>();
                *valuePtr = color.isValid() ? std::optional<QColor>(color) : std::nullopt;
            }
            selectColor(*valuePtr);
            preview();
        });
    return combo;
}
}

Qt::KeyboardModifier commandModifier() {
    // Qt maps ControlModifier to Command on macOS.
    return Qt::ControlModifier;
}

std::optional<Filter> editFilter(QWidget* parent, const Filter& initial,
    const QStringList& history, bool finding, bool adding) {
    QDialog dialog(parent);
    dialog.setWindowTitle(finding ? QObject::tr("Find")
        : adding ? QObject::tr("Add Filter") : QObject::tr("Edit Filter"));
    dialog.setObjectName(finding ? "findDialog" : "filterEditor");
    auto* layout = new QVBoxLayout(&dialog);
    auto* panel = new QWidget(&dialog);
    auto* grid = new QGridLayout(panel);
    grid->setContentsMargins(4, 4, 4, 4);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);
    layout->addWidget(panel);

    auto* kind = new QComboBox(panel);
    kind->setObjectName("filterKind");
    kind->addItems({QObject::tr("Matches text"), QObject::tr("Marked by marker")});
    kind->setCurrentIndex(initial.kind == Filter::Kind::Marker && !finding ? 1 : 0);
    auto* pattern = new QComboBox(panel);
    pattern->setObjectName("filterPattern");
    pattern->setAccessibleName(QObject::tr("Text"));
    pattern->setEditable(true);
    pattern->addItems(history);
    pattern->setEditText(initial.pattern);
    pattern->setMinimumWidth(340);
    auto* marker = new QComboBox(panel);
    marker->setObjectName("filterMarker");
    for (int i = 1; i <= 8; ++i) marker->addItem(QString::number(i));
    marker->setCurrentIndex(initial.marker);
    auto* textLabel = new QLabel(QObject::tr("&Text:"), panel);
    textLabel->setMinimumWidth(textLabel->fontMetrics().horizontalAdvance(QObject::tr("Marker:")) + 4);
    textLabel->setBuddy(pattern);
    grid->addWidget(textLabel, 1, 0);
    grid->addWidget(pattern, 1, 1, 1, 5);
    grid->addWidget(marker, 1, 1, 1, 5);

    auto* excluding = new QCheckBox(QObject::tr("&Excluding"), panel);
    excluding->setObjectName("filterExcluding");
    excluding->setChecked(initial.excluding);
    auto* sensitive = new QCheckBox(QObject::tr("&Case-sensitive"), panel);
    sensitive->setObjectName("filterCaseSensitive");
    sensitive->setChecked(initial.caseSensitive);
    auto* regex = new QCheckBox(QObject::tr("&Regular expression"), panel);
    regex->setObjectName("filterRegex");
    regex->setChecked(initial.regularExpression);
    auto* checks = new QHBoxLayout;
    if (!finding) checks->addWidget(excluding);
    else excluding->hide();
    checks->addWidget(sensitive);
    checks->addWidget(regex);
    checks->addStretch();
    grid->addLayout(checks, finding ? 2 : 3, 1, 1, 5);

    auto foreground = initial.foreground;
    auto background = initial.background;
    const auto preview = [&] {
        auto palette = pattern->palette();
        const auto base = dialog.palette();
        palette.setColor(QPalette::Text, foreground.value_or(base.color(QPalette::Text)));
        palette.setColor(QPalette::Base, background.value_or(base.color(QPalette::Base)));
        pattern->setPalette(palette);
        marker->setPalette(palette);
    };
    if (!finding) {
        auto* kindLabel = new QLabel(QObject::tr("&Filter:"), panel);
        kindLabel->setBuddy(kind);
        grid->addWidget(kindLabel, 0, 0);
        grid->addWidget(kind, 0, 1);
        auto* foregroundPicker = colorPicker(dialog, "filterForeground", QObject::tr("Text Color"), foreground, preview, false);
        auto* backgroundPicker = colorPicker(dialog, "filterBackground", QObject::tr("Background"), background, preview, true);
        auto* foregroundLabel = new QLabel(QObject::tr("Text C&olor:"), panel);
        auto* backgroundLabel = new QLabel(QObject::tr("&Background:"), panel);
        foregroundLabel->setBuddy(foregroundPicker);
        backgroundLabel->setBuddy(backgroundPicker);
        grid->addWidget(foregroundLabel, 0, 2);
        grid->addWidget(foregroundPicker, 0, 3);
        grid->addWidget(backgroundLabel, 0, 4);
        grid->addWidget(backgroundPicker, 0, 5);
    } else kind->hide();

    auto* description = new QLineEdit(initial.description, panel);
    description->setObjectName("filterDescription");
    description->setAccessibleName(QObject::tr("Description"));
    auto* descriptionLabel = new QLabel(QObject::tr("&Description:"), panel);
    descriptionLabel->setBuddy(description);
    grid->addWidget(descriptionLabel, 2, 0);
    grid->addWidget(description, 2, 1, 1, 5);
    description->setVisible(!finding);
    descriptionLabel->setVisible(!finding);
    auto* error = new QLabel(&dialog);
    error->setObjectName("validationError");
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setCenterButtons(true);
    buttons->button(QDialogButtonBox::Ok)->setMinimumWidth(80);
    buttons->button(QDialogButtonBox::Cancel)->setMinimumWidth(80);
    layout->addWidget(buttons);
    Filter result = initial;
    const auto updateKind = [&] {
        const bool text = kind->currentIndex() == 0;
        pattern->setVisible(text);
        marker->setVisible(!text);
        textLabel->setText(text ? QObject::tr("&Text:") : QObject::tr("&Marker:"));
        textLabel->setBuddy(text ? static_cast<QWidget*>(pattern) : marker);
        sensitive->setEnabled(text);
        regex->setEnabled(text);
    };
    QObject::connect(kind, &QComboBox::currentIndexChanged, &dialog, updateKind);
    updateKind();
    preview();
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        result.kind = kind->currentIndex() == 1 ? Filter::Kind::Marker : Filter::Kind::Text;
        result.regularExpression = regex->isChecked();
        result.marker = marker->currentIndex();
        result.pattern = pattern->currentText();
        result.description = description->text();
        result.caseSensitive = sensitive->isChecked();
        result.enabled = finding || initial.enabled;
        result.excluding = !finding && excluding->isChecked();
        result.foreground = foreground;
        result.background = background;
        try {
            validateFilter(result);
            dialog.accept();
        } catch (const std::exception& e) {
            error->setText(QString::fromUtf8(e.what()));
            error->show();
        }
    });
    pattern->setFocus();
    if (pattern->lineEdit()) pattern->lineEdit()->selectAll();
    dialog.resize(finding ? 500 : 660, dialog.sizeHint().height());
    if (dialog.exec() == QDialog::Accepted) return result;
    return std::nullopt;
}
}
