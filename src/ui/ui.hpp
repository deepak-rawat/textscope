#pragma once

#include "core/core.hpp"
#include <QAbstractScrollArea>
#include <QAbstractTableModel>
#include <QMainWindow>
#include <QTableView>

class QDockWidget;

namespace textscope {

class FilterModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit FilterModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex&, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex&) const override;
    bool setData(const QModelIndex&, const QVariant&, int role) override;
    Qt::DropActions supportedDropActions() const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList&) const override;
    bool dropMimeData(const QMimeData*, Qt::DropAction, int, int, const QModelIndex&) override;
    void setFilters(const std::vector<Filter>& filters);
    void setCounts(const std::vector<LineSet>& matches, LineId total);
    const std::vector<Filter>& filters() const;
signals:
    void filtersChanged();
private:
    std::vector<Filter> filters_;
    std::vector<LineId> counts_;
    LineId total_ = 0;
};

class LogView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit LogView(QWidget* parent = nullptr);
    ~LogView() override;
    void setDocument(std::shared_ptr<const Snapshot> snapshot,
        const std::vector<Filter>& filters, const Analysis& analysis,
        const Markers& markers);
    void setLoadingPreview(QString text);
    void setLoadingPreview(const QStringList& lines);
    void clearLoadingPreview();
    void setFilteredOnly(bool enabled);
    bool filteredOnly() const;
    void setLineNumbers(bool enabled);
    void setMarkerVisibility(int mode);
    void setFilterTooltips(bool enabled);
    void setTabWidth(int width);
    void setMaxCharacters(qsizetype limit);
    void setTextFont(const QFont& font);
    void setZoom(int percent);
    int zoom() const;
    LineId currentLine() const;
    LineId firstVisibleRank() const;
    LineSet selection() const;
    void selectAll();
    void goToLine(LineId line, bool extend = false);
signals:
    void selectionChanged();
    void zoomChanged(int percent);
    void textFontChanged(const QFont& font);
    void toggleMarkerRequested(int marker);
    void navigateRequested(int filter, bool forward);
    void markerNavigationRequested(int marker, bool forward);
    void toggleFilterRequested(int index);
    void addFilterRequested(QString pattern);
    void errorOccurred(QString error);
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    bool viewportEvent(QEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    LogView* logView() const;
    FilterModel* filterModel() const;
    QTableView* filterView() const;
    QDockWidget* filterDock() const;
    void setStatus(QString source, LineId selected, LineId filtered,
        LineId total, int zoom, QString activity = {});
    void setRecentFiles(const QStringList& files, const QStringList& filters);
    void setBusy(bool busy);
signals:
    void commandRequested(QString command);
    void pathDropped(QString path);
    void textDropped(QString text);
    void closeRequested(bool* accepted);
protected:
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
    void closeEvent(QCloseEvent*) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::optional<Filter> editFilter(QWidget* parent, const Filter& initial,
    const QStringList& history, bool finding = false, bool adding = false);
Qt::KeyboardModifier commandModifier();

}
