#pragma once

#include "core/core.hpp"
#include <QFont>
#include <QMap>
#include <QSettings>

namespace textscope {

struct FilterFile {
    std::vector<Filter> filters;
    bool filteredOnly = false;
    QMap<QString, QString> attributes;
};

FilterFile readFilters(const QByteArray& xml);
FilterFile loadFilters(const QString& path);
QByteArray writeFilters(const FilterFile& file);
void saveFilters(const QString& path, const FilterFile& file);

struct Preferences {
    QFont font;
    QByteArray encoding = "UTF-8";
    int zoom = 100;
    int tabWidth = 4;
    int markerVisibility = 2;
    bool lineNumbers = true;
    bool tooltips = true;
    bool copyLineNumbers = false;
    bool confirmReplacement = true;
    bool rememberLayout = true;
    bool fullPathTitle = false;
    bool markToggleChanges = true;
    QColor textColor = Qt::black;
    QColor windowColor = Qt::white;
    QColor selectionColor = QColor("#0078d7");
    QColor selectionTextColor = Qt::white;
};

void migrateLegacySettings(QSettings& settings, QSettings& legacy);
Preferences loadPreferences(QSettings& settings);
void savePreferences(QSettings& settings, const Preferences& preferences);
QByteArray writePreferences(const Preferences& preferences);
Preferences readPreferences(const QByteArray& xml, Preferences current);
void writeFileAtomically(const QString& path, const QByteArray& bytes);
QByteArray readSmallFile(const QString& path);
QStringList recentFiles(QSettings& settings, const QString& key);
void rememberFile(QSettings& settings, const QString& key, const QString& path);

}
