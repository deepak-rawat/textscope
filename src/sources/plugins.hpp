#pragma once

#include "core/core.hpp"
#include "parserplugin.hpp"
#include <QPluginLoader>

namespace textscope {
class Plugins {
public:
    void discover(const QString& directory);
    QStringList descriptions() const;
    QStringList errors() const;
    bool needsParsing(const QString& path, const QByteArray& prefix) const;
    void configure(QWidget* parent);
    std::shared_ptr<const Snapshot> open(const QString& path,
        const OpenOptions& options, const CancelFlag& cancel, const Progress& progress) const;
private:
    std::vector<std::unique_ptr<QPluginLoader>> loaders_;
    std::vector<ParserPlugin*> parsers_;
    QStringList errors_;
};
}
