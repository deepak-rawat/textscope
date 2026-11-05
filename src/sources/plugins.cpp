#include "plugins.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryFile>
#include <QDebug>
#include <stdexcept>

namespace textscope {
void Plugins::discover(const QString& directory)
{
    const QDir dir(directory);
    for (const auto& file : dir.entryList(QDir::Files)) {
        const auto path = dir.absoluteFilePath(file);
        if (!QLibrary::isLibrary(path)) continue;
        auto loader = std::make_unique<QPluginLoader>(path);
        QObject* instance = loader->instance();
        if (!instance) {
            errors_.append(file + ": " + loader->errorString());
            qWarning().noquote() << errors_.last();
            continue;
        }
        auto* parser = qobject_cast<ParserPlugin*>(instance);
        if (!parser) {
            errors_.append(file + ": not a version-1 native TextScope parser");
            qWarning().noquote() << errors_.last();
            loader->unload();
            continue;
        }
        parsers_.push_back(parser);
        loaders_.push_back(std::move(loader));
    }
}

QStringList Plugins::descriptions() const
{
    QStringList names;
    for (const auto* parser : parsers_) names.append(parser->name());
    return names;
}
QStringList Plugins::errors() const { return errors_; }
bool Plugins::needsParsing(const QString& path, const QByteArray& prefix) const
{
    for (const auto* parser : parsers_)
        if (parser->accepts(path, prefix)) return true;
    return false;
}
void Plugins::configure(QWidget* parent)
{
    for (auto* parser : parsers_)
        if (parser->configurable()) parser->configure(parent);
}

std::shared_ptr<const Snapshot> Plugins::open(const QString& path,
    const OpenOptions& options, const CancelFlag& cancel, const Progress& progress) const
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly))
        throw std::runtime_error(input.errorString().toStdString());
    const auto prefix = input.peek(65536);
    for (const auto* parser : parsers_) {
        if (!parser->accepts(path, prefix)) continue;
        QTemporaryFile output(QDir::tempPath() + "/textscope-parser-XXXXXX");
        if (!output.open()) throw std::runtime_error(output.errorString().toStdString());
        output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        const std::atomic_bool notCancelled{false};
        parser->parse(input, output, cancel ? *cancel : notCancelled);
        if (!output.flush()) throw std::runtime_error(output.errorString().toStdString());
        OpenOptions parsedOptions = options;
        parsedOptions.direct = false;
        parsedOptions.encoding = "UTF-8";
        return Snapshot::open(output.fileName(), parsedOptions, cancel, progress);
    }
    input.close();
    return Snapshot::open(path, options, cancel, progress);
}
}
