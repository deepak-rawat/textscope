#pragma once

#include "persistence/persistence.hpp"
#include "sources/plugins.hpp"
#include "ui/ui.hpp"
#include <QObject>
#include <QSettings>
#include <QTimer>
#include <deque>
#include <QPointer>

class QFileDialog;

namespace textscope {

class LoadFeedback : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
signals:
    void previewReady(QString text);
    void progressReady(qulonglong done, qulonglong total);
};

struct Startup {
    QString input;
    QStringList filters;
    QString configuration;
    bool clipboard = false;
    bool direct = false;
    LineId line = 0;
    QByteArray encoding;
};

class Session : public QObject {
    Q_OBJECT
public:
    explicit Session(const Startup& startup = {}, QObject* parent = nullptr);
    ~Session() override;
    MainWindow* window() const;
    std::shared_ptr<const Snapshot> document() const;
    const Analysis& analysis() const;
    const Markers& markers() const;
    bool busy() const;
    void open(const QString& path);
    void pasteText(const QString& text, bool confirm = true);
signals:
    void newWindowRequested();
    void previewAvailable();
private:
    void command(const QString& name);
    void chooseFile();
    void changeFilters();
    void refresh();
    void updateStatus();
    void showError(const QString& message);
    void applyPreferences();
    void editPreferences();
    void chooseEncoding();
    void loadFilterFile(const QString& path, bool append);
    bool saveFilterFile(bool choosePath);
    bool confirmDirty();
    bool confirmReplacement();
    void editSelectedFilters();
    void addFilter(const QString& pattern = {});
    void copyFilters();
    void pasteFilters();
    void copyLines();
    void saveLines();
    void findText(bool edit, bool forward);
    void navigate(int filter, bool forward);
    void navigateMarker(int marker, bool forward);
    void toggleMarker(int marker);
    void runNextMarkerTask();
    void setLoading(bool loading);
    void cancelJobs();
    void beginTask(const QString& activity);
    void endTask();
    void afterLoad(std::shared_ptr<const Snapshot> snapshot, const QString& path);
    void rememberHistory(const QString& pattern);
    QAction* action(const QString& name) const;
    std::vector<int> selectedFilters() const;
    FilterFile filterFile() const;

    MainWindow* window_;
    QPointer<QFileDialog> openDialog_;
    QSettings settings_;
    Preferences preferences_;
    std::shared_ptr<Plugins> plugins_;
    std::shared_ptr<const Snapshot> snapshot_;
    std::vector<Filter> filters_;
    Analysis analysis_;
    std::vector<Filter> publishedFilters_;
    Markers publishedMarkers_;
    Markers markers_;
    QMap<QString, LineSet> cache_;
    QMap<QString, QString> filterAttributes_;
    QString sourcePath_;
    QString filterPath_;
    QStringList history_;
    QString activity_;
    Filter findQuery_;
    bool hasFind_ = false;
    bool dirty_ = false;
    bool direct_ = false;
    bool updatingModel_ = false;
    int pendingJobs_ = 0;
    std::uint64_t loadRevision_ = 0;
    std::uint64_t analysisRevision_ = 0;
    CancelFlag loadCancel_;
    CancelFlag analysisCancel_;
    CancelFlag auxiliaryCancel_;
    CancelFlag markerCancel_;
    std::deque<std::pair<int, LineSet>> markerTasks_;
    bool markerRunning_ = false;
    bool loading_ = false;
    bool staleAnalysis_ = false;
    QTimer filterTimer_;
    Startup startup_;
};
}
