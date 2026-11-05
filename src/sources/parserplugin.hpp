#pragma once

#include <QIODevice>
#include <QString>
#include <QtPlugin>
#include <atomic>

class QWidget;

namespace textscope {
class ParserPlugin {
public:
    virtual ~ParserPlugin() = default;
    virtual QString name() const = 0;
    virtual bool accepts(const QString& path, const QByteArray& prefix) const = 0;
    virtual void parse(QIODevice& input, QIODevice& utf8Output,
        const std::atomic_bool& cancelled) const = 0;
    virtual bool configurable() const { return false; }
    virtual void configure(QWidget*) {}
};
}

#define TEXTSCOPE_PARSER_IID "io.github.deepak-rawat.textscope.ParserPlugin/1.0"
Q_DECLARE_INTERFACE(textscope::ParserPlugin, TEXTSCOPE_PARSER_IID)
