#include "ui/ui.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QRunnable>
#include <QScrollBar>
#include <QSet>
#include <QStaticText>
#include <QThreadPool>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <atomic>

namespace textscope {
namespace {
struct RenderLine {
    QString text;
    bool truncated = false;
    int width = 0;
    QStaticText layout;
};
QColor blend(QColor color, QColor base) {
    return QColor((color.red() * 2 + base.red()) / 3,
        (color.green() * 2 + base.green()) / 3, (color.blue() * 2 + base.blue()) / 3);
}
QString expandTabs(const QString& text, int tabWidth, qsizetype limit) {
    QString result;
    result.reserve(std::min(limit, text.size() * 2));
    qsizetype column = 0;
    for (QChar character : text) {
        if (character == '\t') {
            const int spaces = tabWidth - int(column % tabWidth);
            result.append(QString(std::min<qsizetype>(spaces, limit - result.size()), ' '));
            column += spaces;
        } else {
            result.append(character);
            ++column;
        }
        if (result.size() >= limit) break;
    }
    return result;
}
}

struct LogView::Impl {
    LogView* owner;
    std::shared_ptr<const Snapshot> snapshot;
    std::shared_ptr<const QStringList> preview;
    std::vector<Filter> filters;
    Analysis analysis;
    Markers markers;
    LineSet selected;
    LineSet dragBase;
    LineId current = 0;
    LineId anchor = 0;
    LineId origin = 0;
    bool filtered = false;
    bool numbers = true;
    bool tooltips = true;
    int markerMode = 2;
    int tabWidth = 4;
    int zoom = 100;
    int rowHeight = 20;
    int gutter = 0;
    int markerWidth = 0;
    int textWidth = 0;
    qsizetype maxCharacters = 16384;
    quint64 revision = 0;
    bool syncingScroll = false;
    bool dragging = false;
    int inFlight = 0;
    int wheelRemainder = 0;
    int pixelRemainder = 0;
    std::shared_ptr<std::atomic_bool> obsolete = std::make_shared<std::atomic_bool>(false);
    QFont baseFont;
    QHash<quint64, RenderLine> cache;
    QSet<quint64> pending;

    explicit Impl(LogView* view) : owner(view) {}
    LineSet visible() const {
        if (preview) return LineSet::all(LineId(preview->size()));
        return filtered ? analysis.accepted : LineSet::all(snapshot ? snapshot->lineCount() : 0);
    }
    int page() const { return std::max(1, owner->viewport()->height() / rowHeight - (preview ? 1 : 0)); }
    LineId maxOrigin() const {
        const LineId count = visible().count();
        return count > LineId(page()) ? count - LineId(page()) : 0;
    }
    int sliderValue(LineId rank) const {
        const auto max = maxOrigin();
        if (max <= LineId(std::numeric_limits<int>::max())) return int(rank);
        return int(std::llround((static_cast<long double>(rank) / max) * std::numeric_limits<int>::max()));
    }
    LineId sliderOrigin(int value) const {
        const auto max = maxOrigin();
        if (value <= 0) return 0;
        if (value >= owner->verticalScrollBar()->maximum()) return max;
        if (max <= LineId(std::numeric_limits<int>::max())) return LineId(std::max(0, value));
        return std::min(max, LineId((static_cast<long double>(value) / std::numeric_limits<int>::max()) * max + 0.5L));
    }
    void scrollTo(LineId rank) {
        origin = std::min(rank, maxOrigin());
        syncingScroll = true;
        owner->verticalScrollBar()->setValue(sliderValue(origin));
        syncingScroll = false;
        owner->viewport()->update();
    }
    void scrollBy(qint64 delta) {
        if (delta < 0) scrollTo(origin > LineId(-delta) ? origin - LineId(-delta) : 0);
        else scrollTo(origin + std::min(LineId(delta), maxOrigin() - std::min(origin, maxOrigin())));
    }
    void geometry() {
        QFont font = baseFont;
        if (baseFont.pointSizeF() > 0)
            font.setPointSizeF(std::max(1.0, baseFont.pointSizeF() * zoom / 100.0));
        else if (baseFont.pixelSize() > 0)
            font.setPixelSize(std::max(1, qRound(baseFont.pixelSize() * zoom / 100.0)));
        if (font != owner->font()) {
            owner->setFont(font);
            emit owner->textFontChanged(font);
        }
        const QFontMetrics metrics(font);
        rowHeight = std::max(1, metrics.height() + 4);
        bool markerInUse = false;
        for (const auto& marker : markers) markerInUse |= marker.count() != 0;
        markerWidth = preview || markerMode == 1 || (markerMode == 2 && !markerInUse) ? 0 : 8 * 16 + 6;
        gutter = markerWidth + (numbers && !preview ? metrics.horizontalAdvance(QString::number(
            qulonglong(snapshot ? snapshot->lineCount() : 0))) + 16 : 0);
        syncingScroll = true;
        const auto max = maxOrigin();
        owner->verticalScrollBar()->setRange(0, int(std::min(max, LineId(std::numeric_limits<int>::max()))));
        owner->verticalScrollBar()->setPageStep(max <= LineId(std::numeric_limits<int>::max()) ? page() :
            std::max(1, int(static_cast<long double>(page()) / std::max<LineId>(1, visible().count()) * std::numeric_limits<int>::max())));
        owner->verticalScrollBar()->setSingleStep(1);
        owner->horizontalScrollBar()->setRange(0, std::max(0, textWidth - std::max(0, owner->viewport()->width() - gutter - 8)));
        owner->horizontalScrollBar()->setPageStep(std::max(1, owner->viewport()->width() - gutter));
        syncingScroll = false;
        scrollTo(origin);
    }
    std::optional<LineId> hit(QPoint point) const {
        if (preview) return {};
        if (point.y() < 0 || point.y() >= owner->viewport()->height()) return {};
        const auto rows = visible();
        const LineId rank = origin + LineId(point.y() / rowHeight);
        return rank < rows.count() ? std::optional<LineId>(rows.at(rank)) : std::nullopt;
    }
    void invalidate() {
        ++revision;
        obsolete->store(true);
        obsolete = std::make_shared<std::atomic_bool>(false);
        inFlight = 0;
        cache.clear();
        pending.clear();
        textWidth = 0;
        geometry();
    }
    void request(const std::vector<LineId>& rows) {
        if ((!snapshot && !preview) || rows.empty() || inFlight >= 2) return;
        ++inFlight;
        for (auto row : rows) pending.insert(row);
        const auto source = snapshot;
        const auto previewSource = preview;
        const auto generation = revision;
        const auto font = owner->font();
        const auto limit = maxCharacters;
        const auto tabs = tabWidth;
        const auto cancelled = obsolete;
        QPointer<LogView> guard(owner);
        QThreadPool::globalInstance()->start(QRunnable::create([source, previewSource, rows, generation, font, limit, tabs, guard, cancelled] {
            QHash<quint64, RenderLine> result;
            QString error;
            try {
                if (cancelled->load()) return;
                if (source) source->verifyUnchanged();
                const QFontMetricsF metrics(font);
                for (auto row : rows) {
                    if (cancelled->load()) return;
                    const auto original = source ? source->line(row, limit + 1) : previewSource->at(qsizetype(row)).left(limit + 1);
                    RenderLine rendered;
                    rendered.truncated = original.size() > limit;
                    rendered.text = expandTabs(original.left(limit), tabs, limit);
                    rendered.truncated |= rendered.text.size() == limit && original.contains('\t');
                    if (rendered.truncated) rendered.text += QObject::tr("  … [display truncated; full source retained]");
                    rendered.width = int(std::min<double>(std::numeric_limits<int>::max(), std::ceil(metrics.horizontalAdvance(rendered.text))));
                    rendered.layout.setTextFormat(Qt::PlainText);
                    rendered.layout.setText(rendered.text);
                    rendered.layout.prepare(QTransform(), font);
                    result.insert(row, std::move(rendered));
                }
            } catch (const std::exception& e) {
                error = QString::fromUtf8(e.what());
            }
            QMetaObject::invokeMethod(qApp, [guard, generation, rows, result = std::move(result), error]() mutable {
                if (!guard || guard->impl_->revision != generation) return;
                auto& d = *guard->impl_;
                --d.inFlight;
                for (auto row : rows) d.pending.remove(row);
                if (!error.isEmpty()) {
                    for (auto row : rows) {
                        RenderLine unavailable;
                        unavailable.text = QObject::tr("[unavailable]");
                        d.cache.insert(row, std::move(unavailable));
                    }
                    emit guard->errorOccurred(error);
                } else {
                    for (auto i = result.begin(); i != result.end(); ++i) {
                        d.textWidth = std::max(d.textWidth, i.value().width);
                        d.cache.insert(i.key(), std::move(i.value()));
                    }
                    if (d.cache.size() > std::max(512, d.page() * 4)) {
                        const auto visible = d.visible();
                        for (auto i = d.cache.begin(); i != d.cache.end();) {
                            const auto rank = visible.rankBefore(i.key());
                            if (!visible.contains(i.key()) || rank < d.origin ||
                                rank - d.origin > LineId(d.page() * 2)) i = d.cache.erase(i);
                            else ++i;
                        }
                    }
                }
                d.geometry();
                guard->viewport()->update();
            }, Qt::QueuedConnection);
        }));
    }
    LineSet interval(LineId a, LineId b) const {
        if (a > b) std::swap(a, b);
        LineSetBuilder builder;
        if (!filtered) builder.addRange(a, b);
        else {
            const auto& accepted = analysis.accepted;
            std::uint64_t low = 0, high = accepted.rangeCount();
            while (low < high) {
                const auto middle = low + (high - low) / 2;
                if (accepted.rangeAt(middle).second < a) low = middle + 1;
                else high = middle;
            }
            for (auto i = low; i < accepted.rangeCount(); ++i) {
                const auto [first, last] = accepted.rangeAt(i);
                if (first > b) break;
                builder.addRange(std::max(first, a), std::min(last, b));
            }
        }
        return builder.finish();
    }
    static LineSet unite(const LineSet& a, const LineSet& b) {
        LineSetBuilder builder;
        std::uint64_t ai = 0, bi = 0;
        std::optional<std::pair<LineId, LineId>> pendingRange;
        while (ai < a.rangeCount() || bi < b.rangeCount()) {
            const bool takeA = bi == b.rangeCount() ||
                (ai < a.rangeCount() && a.rangeAt(ai).first <= b.rangeAt(bi).first);
            const auto range = takeA ? a.rangeAt(ai++) : b.rangeAt(bi++);
            if (!pendingRange) pendingRange = range;
            else if (range.first <= pendingRange->second ||
                (pendingRange->second != std::numeric_limits<LineId>::max() && range.first == pendingRange->second + 1))
                pendingRange->second = std::max(pendingRange->second, range.second);
            else {
                builder.addRange(pendingRange->first, pendingRange->second);
                pendingRange = range;
            }
        }
        if (pendingRange) builder.addRange(pendingRange->first, pendingRange->second);
        return builder.finish();
    }
    void choose(LineId line, bool extend, bool additive, const LineSet* base = nullptr) {
        if (!snapshot || line >= snapshot->lineCount()) return;
        current = line;
        if (extend) {
            auto range = interval(anchor, line);
            selected = additive ? unite(base ? *base : selected, range) : range;
        } else {
            anchor = line;
            if (additive) {
                LineSetBuilder builder;
                const bool remove = selected.contains(line);
                if (!remove) {
                    LineSetBuilder single;
                    single.add(line);
                    selected = unite(selected, single.finish());
                } else {
                    selected.forEachRange([&](LineId first, LineId last) {
                        if (line < first || line > last) builder.addRange(first, last);
                        else {
                            if (first < line) builder.addRange(first, line - 1);
                            if (last > line) builder.addRange(line + 1, last);
                        }
                    });
                    selected = builder.finish();
                }
            } else {
                LineSetBuilder builder;
                builder.add(line);
                selected = builder.finish();
            }
        }
        emit owner->selectionChanged();
        owner->viewport()->update();
    }
    void reveal(LineId line) {
        const auto rows = visible();
        if (!rows.contains(line)) return;
        const auto rank = rows.rankBefore(line);
        if (rank < origin) scrollTo(rank);
        else if (rank - origin >= LineId(page())) scrollTo(rank - LineId(page()) + 1);
    }
};

LogView::LogView(QWidget* parent) : QAbstractScrollArea(parent), impl_(std::make_unique<Impl>(this)) {
    setObjectName("logView");
    setAccessibleName(tr("Source text lines"));
    setAccessibleDescription(tr("Virtualized unwrapped text. Use arrows to move, Shift to select a range, Command or Control to add selections."));
    viewport()->setAccessibleName(tr("Log text viewport"));
    viewport()->setAccessibleDescription(tr("Only visible rows are painted. Copy selected lines to access full source text."));
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setMouseTracking(true);
    impl_->baseFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    impl_->geometry();
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (!impl_->syncingScroll) {
            impl_->origin = impl_->sliderOrigin(value);
            viewport()->update();
        }
    });
    connect(verticalScrollBar(), &QScrollBar::actionTriggered, this, [this](int action) {
        auto& d = *impl_;
        switch (action) {
        case QAbstractSlider::SliderSingleStepAdd: d.scrollBy(1); break;
        case QAbstractSlider::SliderSingleStepSub: d.scrollBy(-1); break;
        case QAbstractSlider::SliderPageStepAdd: d.scrollBy(d.page()); break;
        case QAbstractSlider::SliderPageStepSub: d.scrollBy(-d.page()); break;
        case QAbstractSlider::SliderToMinimum: d.scrollTo(0); break;
        case QAbstractSlider::SliderToMaximum: d.scrollTo(d.maxOrigin()); break;
        default: break;
        }
        verticalScrollBar()->setSliderPosition(d.sliderValue(d.origin));
    });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
}
LogView::~LogView() { impl_->obsolete->store(true); }
void LogView::setDocument(std::shared_ptr<const Snapshot> snapshot,
    const std::vector<Filter>& filters, const Analysis& analysis, const Markers& markers) {
    auto& d = *impl_;
    const bool same = d.snapshot && d.snapshot == snapshot;
    const auto previousVisible = d.visible();
    const auto previousTop = d.origin < previousVisible.count() ? previousVisible.at(d.origin) : d.current;
    d.preview.reset();
    viewport()->setAccessibleName(tr("Log text viewport"));
    viewport()->setAccessibleDescription(tr("Only visible rows are painted. Copy selected lines to access full source text."));
    d.snapshot = std::move(snapshot);
    d.filters = filters;
    d.analysis = analysis;
    d.markers = markers;
    if (!same || !d.snapshot || !d.snapshot->lineCount()) {
        d.selected = {};
        d.current = d.anchor = d.origin = 0;
    } else {
        const auto count = d.snapshot->lineCount();
        d.current = std::min(d.current, count - 1);
        d.anchor = std::min(d.anchor, count - 1);
        d.origin = d.visible().rankBefore(previousTop);
    }
    d.invalidate();
    emit selectionChanged();
}
void LogView::setLoadingPreview(QString text) {
    setLoadingPreview(text.left(65536).split('\n'));
}
void LogView::setLoadingPreview(const QStringList& lines) {
    auto& d = *impl_;
    d.snapshot.reset();
    QStringList bounded;
    qsizetype remaining = 65536;
    for (const auto& line : lines) {
        if (remaining <= 0) break;
        bounded.append(line.left(remaining));
        remaining -= std::min(line.size(), remaining) + 1;
    }
    d.preview = std::make_shared<const QStringList>(std::move(bounded));
    viewport()->setAccessibleName(tr("Loading preview (not final)"));
    viewport()->setAccessibleDescription(tr("Loading preview of a bounded prefix. Not final. Selection and analysis are unavailable."));
    d.filters.clear();
    d.analysis = {};
    d.markers = {};
    d.selected = {};
    d.current = d.anchor = d.origin = 0;
    d.invalidate();
    emit selectionChanged();
}
void LogView::clearLoadingPreview() {
    if (impl_->preview) setDocument({}, {}, {}, {});
}
void LogView::setFilteredOnly(bool enabled) {
    auto& d = *impl_;
    if (d.filtered == enabled) return;
    const auto before = d.visible();
    const auto top = d.origin < before.count() ? before.at(d.origin) : d.current;
    d.filtered = enabled;
    d.origin = d.visible().rankBefore(top);
    d.geometry();
    if (d.selected.contains(d.current))
        d.reveal(d.current);
}
bool LogView::filteredOnly() const { return impl_->filtered; }
void LogView::setLineNumbers(bool enabled) { impl_->numbers = enabled; impl_->geometry(); }
void LogView::setMarkerVisibility(int mode) { impl_->markerMode = std::clamp(mode, 0, 2); impl_->geometry(); }
void LogView::setFilterTooltips(bool enabled) { impl_->tooltips = enabled; if (!enabled) QToolTip::hideText(); }
void LogView::setTabWidth(int width) { impl_->tabWidth = std::clamp(width, 1, 32); impl_->invalidate(); }
void LogView::setMaxCharacters(qsizetype limit) { impl_->maxCharacters = std::clamp<qsizetype>(limit, 1, 1024 * 1024); impl_->invalidate(); }
void LogView::setTextFont(const QFont& font) {
    if (impl_->baseFont == font) return;
    impl_->baseFont = font;
    impl_->invalidate();
}
void LogView::setZoom(int percent) {
    const int bounded = std::clamp(percent, 25, 400);
    if (impl_->zoom == bounded) return;
    impl_->zoom = bounded;
    impl_->invalidate();
    emit zoomChanged(bounded);
}
int LogView::zoom() const { return impl_->zoom; }
LineId LogView::currentLine() const { return impl_->current; }
LineId LogView::firstVisibleRank() const { return impl_->origin; }
LineSet LogView::selection() const { return impl_->selected; }
void LogView::selectAll() {
    if (impl_->preview) return;
    impl_->selected = impl_->visible();
    emit selectionChanged();
    viewport()->update();
}
void LogView::goToLine(LineId line, bool extend) {
    if (!impl_->snapshot || line >= impl_->snapshot->lineCount()) return;
    impl_->choose(line, extend, false);
    impl_->reveal(line);
}
void LogView::paintEvent(QPaintEvent*) {
    auto& d = *impl_;
    QPainter painter(viewport());
    painter.setFont(font());
    const auto base = palette().color(QPalette::Base);
    const auto normalText = palette().color(QPalette::Text);
    painter.fillRect(viewport()->rect(), base);
    const int previewOffset = d.preview ? d.rowHeight : 0;
    if (d.preview) {
        painter.setPen(normalText);
        painter.drawText(QRect(4, 0, std::max(0, viewport()->width() - 8), d.rowHeight),
            Qt::AlignLeft | Qt::AlignVCenter, tr("Loading preview — bounded prefix; not final; selection unavailable"));
    }
    const auto rows = d.visible();
    const QFontMetrics metrics(font());
    std::vector<LineId> missing;
    const int count = viewport()->height() / d.rowHeight + 1;
    for (int i = 0; i < count && d.origin + LineId(i) < rows.count(); ++i) {
        const LineId line = rows.at(d.origin + LineId(i));
        const QRect rect(0, previewOffset + i * d.rowHeight, viewport()->width(), d.rowHeight);
        QColor foreground = normalText, background = base;
        for (std::size_t f = 0; f < d.filters.size() && f < d.analysis.matches.size(); ++f) {
            const auto& filter = d.filters[f];
            if (filter.enabled && !filter.excluding && (filter.foreground || filter.background) &&
                d.analysis.matches[f].contains(line)) {
                foreground = filter.foreground.value_or(normalText);
                background = filter.background.value_or(base);
                break;
            }
        }
        if (!d.preview && !d.analysis.accepted.contains(line)) foreground = blend(foreground, background);
        if (d.selected.contains(line)) {
            background = palette().color(QPalette::Highlight);
            foreground = palette().color(QPalette::HighlightedText);
            const auto luminance = [](QColor c) { return 0.2126 * c.redF() + 0.7152 * c.greenF() + 0.0722 * c.blueF(); };
            if (std::abs(luminance(background) - luminance(foreground)) < 0.4)
                foreground = luminance(background) > 0.5 ? Qt::black : Qt::white;
        }
        painter.fillRect(rect, background);
        painter.setPen(foreground);
        if (d.markerWidth) {
            for (int marker = 0; marker < 8; ++marker) {
                const QRect symbol(3 + marker * 16, rect.y(), 16, d.rowHeight);
                if (d.markers[marker].contains(line)) {
                    painter.fillRect(symbol.adjusted(1, 2, -1, -2), QColor::fromHsv(marker * 45, 160, 210));
                    painter.setPen(Qt::black);
                    painter.drawText(symbol, Qt::AlignCenter, QString::number(marker + 1));
                    painter.setPen(foreground);
                }
            }
        }
        if (d.numbers && !d.preview)
            painter.drawText(QRect(d.markerWidth, rect.y(), d.gutter - d.markerWidth - 8, d.rowHeight),
                Qt::AlignRight | Qt::AlignVCenter, QString::number(qulonglong(line + 1)));
        painter.save();
        painter.setClipRect(QRect(d.gutter, rect.y(), std::max(0, rect.width() - d.gutter), d.rowHeight));
        const auto cached = d.cache.constFind(line);
        const int textX = d.gutter + 4 - horizontalScrollBar()->value();
        const int textY = rect.y() + (d.rowHeight - metrics.height()) / 2;
        if (cached != d.cache.cend() && !cached->layout.text().isEmpty())
            painter.drawStaticText(textX, textY, cached->layout);
        else
            painter.drawText(textX, textY + metrics.ascent(), cached == d.cache.cend() ? tr("Loading…") : cached->text);
        painter.restore();
        if (cached == d.cache.cend() && !d.pending.contains(line)) missing.push_back(line);
        if (!d.preview && line == d.current && hasFocus() && !d.selected.contains(line)) {
            painter.setPen(QPen(foreground, 1, Qt::DotLine));
            painter.drawRect(rect.adjusted(0, 0, -1, -1));
        }
    }
    d.request(missing);
}
void LogView::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    impl_->geometry();
}
void LogView::mousePressEvent(QMouseEvent* event) {
    auto& d = *impl_;
    if (event->button() != Qt::LeftButton) { QAbstractScrollArea::mousePressEvent(event); return; }
    setFocus(Qt::MouseFocusReason);
    const auto line = d.hit(event->position().toPoint());
    if (!line) return;
    const bool additive = event->modifiers().testFlag(commandModifier());
    d.dragBase = d.selected;
    d.choose(*line, event->modifiers().testFlag(Qt::ShiftModifier), additive);
    if (d.markerWidth && event->position().x() >= 3 && event->position().x() < 131) {
        emit toggleMarkerRequested(std::clamp(int((event->position().x() - 3) / 16), 0, 7));
        return;
    }
    d.dragging = true;
}
void LogView::mouseMoveEvent(QMouseEvent* event) {
    auto& d = *impl_;
    if (!event->buttons().testFlag(Qt::LeftButton)) { d.dragging = false; return; }
    if (!d.dragging) return;
    if (event->position().y() < 0) d.scrollBy(-1);
    else if (event->position().y() >= viewport()->height()) d.scrollBy(1);
    const QPoint point(int(event->position().x()), std::clamp(int(event->position().y()), 0, std::max(0, viewport()->height() - 1)));
    const auto line = d.hit(point);
    if (line) d.choose(*line, true, event->modifiers().testFlag(commandModifier()), &d.dragBase);
}
void LogView::mouseReleaseEvent(QMouseEvent* event) {
    impl_->dragging = false;
    if (event->button() == Qt::LeftButton) event->accept();
    else QAbstractScrollArea::mouseReleaseEvent(event);
}
void LogView::mouseDoubleClickEvent(QMouseEvent* event) {
    const auto line = impl_->hit(event->position().toPoint());
    if (!line || !impl_->snapshot || event->button() != Qt::LeftButton) return;
    const auto snapshot = impl_->snapshot;
    const auto generation = impl_->revision;
    QPointer<LogView> guard(this);
    QThreadPool::globalInstance()->start(QRunnable::create([snapshot, line, generation, guard] {
        QString text, error;
        try { text = snapshot->line(*line); }
        catch (const std::exception& e) { error = QString::fromUtf8(e.what()); }
        QMetaObject::invokeMethod(qApp, [guard, generation, text, error] {
            if (!guard || guard->impl_->revision != generation) return;
            if (error.isEmpty()) emit guard->addFilterRequested(text);
            else emit guard->errorOccurred(error);
        }, Qt::QueuedConnection);
    }));
}
void LogView::keyPressEvent(QKeyEvent* event) {
    auto& d = *impl_;
    if (d.preview) { event->accept(); return; }
    const int key = event->key();
    const auto mods = event->modifiers();
    const bool command = mods.testFlag(commandModifier());
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    const bool plain = !(mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    if (command && key >= Qt::Key_1 && key <= Qt::Key_8 && !shift) {
        emit toggleMarkerRequested(key - Qt::Key_1);
        event->accept(); return;
    }
    if (command && shift && key >= Qt::Key_A && key <= Qt::Key_Z) {
        emit toggleFilterRequested(key - Qt::Key_A);
        event->accept(); return;
    }
    if (plain && key >= Qt::Key_A && key <= Qt::Key_Z) {
        emit navigateRequested(key - Qt::Key_A, !shift);
        event->accept(); return;
    }
    if (plain && key >= Qt::Key_1 && key <= Qt::Key_8) {
        emit markerNavigationRequested(key - Qt::Key_1, !shift);
        event->accept(); return;
    }
    if (plain && shift && key >= 0 && key <= 0xffff) {
        const auto marker = QStringLiteral("!@#$%^&*").indexOf(QChar(key));
        if (marker >= 0) {
            emit markerNavigationRequested(int(marker), false);
            event->accept(); return;
        }
    }
    if (plain && key == Qt::Key_Space) {
        emit navigateRequested(-1, !shift);
        event->accept(); return;
    }
    if (event->matches(QKeySequence::SelectAll)) { selectAll(); event->accept(); return; }
    const auto rows = d.visible();
    if (rows.count()) {
        LineId rank = std::min(rows.rankBefore(d.current), rows.count() - 1);
        bool navigation = true;
        switch (key) {
        case Qt::Key_Up: if (rank) --rank; break;
        case Qt::Key_Down: if (rank + 1 < rows.count()) ++rank; break;
        case Qt::Key_PageUp: rank = rank > LineId(d.page()) ? rank - LineId(d.page()) : 0; break;
        case Qt::Key_PageDown: rank += std::min(LineId(d.page()), rows.count() - 1 - rank); break;
        case Qt::Key_Home: rank = 0; break;
        case Qt::Key_End: rank = rows.count() - 1; break;
        default: navigation = false; break;
        }
        if (navigation) {
            const auto line = rows.at(rank);
            d.choose(line, shift, command && shift);
            d.reveal(line);
            event->accept(); return;
        }
    }
    QAbstractScrollArea::keyPressEvent(event);
}
void LogView::wheelEvent(QWheelEvent* event) {
    if (event->modifiers().testFlag(commandModifier())) {
        const int delta = event->angleDelta().y() ? event->angleDelta().y() : event->pixelDelta().y();
        if (delta) setZoom(zoom() + (delta > 0 ? 10 : -10));
    } else if (event->modifiers().testFlag(Qt::ShiftModifier) || std::abs(event->angleDelta().x()) > std::abs(event->angleDelta().y())) {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - (event->angleDelta().x() ? event->angleDelta().x() : event->angleDelta().y()));
    } else {
        int delta = 0;
        if (event->pixelDelta().isNull()) {
            impl_->wheelRemainder += event->angleDelta().y();
            delta = impl_->wheelRemainder / 120 * QApplication::wheelScrollLines();
            impl_->wheelRemainder %= 120;
        } else {
            impl_->pixelRemainder += event->pixelDelta().y();
            delta = impl_->pixelRemainder / impl_->rowHeight;
            impl_->pixelRemainder %= impl_->rowHeight;
        }
        impl_->scrollBy(-delta);
    }
    event->accept();
}
bool LogView::viewportEvent(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        QStringList descriptions;
        const auto line = impl_->hit(help->pos());
        if (impl_->tooltips && line) {
            for (std::size_t i = 0; i < impl_->filters.size() && i < impl_->analysis.matches.size(); ++i)
                if (impl_->filters[i].enabled && impl_->analysis.matches[i].contains(*line))
                    descriptions.append(impl_->filters[i].description.isEmpty() ? impl_->filters[i].pattern : impl_->filters[i].description);
        }
        if (!descriptions.isEmpty()) QToolTip::showText(help->globalPos(), descriptions.join('\n'), viewport());
        else QToolTip::hideText();
        return true;
    }
    return QAbstractScrollArea::viewportEvent(event);
}
void LogView::contextMenuEvent(QContextMenuEvent* event) {
    if (impl_->preview) { event->accept(); return; }
    QMenu menu(this);
    auto* copy = menu.addAction(tr("Copy selected lines"));
    connect(copy, &QAction::triggered, this, [this] {
        auto* window = qobject_cast<MainWindow*>(this->window());
        if (window) emit window->commandRequested("copy");
    });
    auto* all = menu.addAction(tr("Select all visible lines"));
    connect(all, &QAction::triggered, this, &LogView::selectAll);
    auto* add = menu.addAction(tr("Add filter…"));
    connect(add, &QAction::triggered, this, [this] { emit addFilterRequested({}); });
    auto* markerMenu = menu.addMenu(tr("Toggle marker on selection"));
    for (int i = 0; i < 8; ++i) {
        auto* marker = markerMenu->addAction(tr("Marker %1").arg(i + 1));
        connect(marker, &QAction::triggered, this, [this, i] { emit toggleMarkerRequested(i); });
    }
    menu.exec(event->globalPos());
}
}
