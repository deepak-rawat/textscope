#include "sources/parserplugin.hpp"
#include <QObject>
#include <stdexcept>

class DemoParser final : public QObject, public textscope::ParserPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID TEXTSCOPE_PARSER_IID FILE "demo.json")
    Q_INTERFACES(textscope::ParserPlugin)
public:
    QString name() const override { return "Streaming .textscopedemo header parser"; }
    bool accepts(const QString& path, const QByteArray& prefix) const override
    {
        return path.endsWith(".textscopedemo") && prefix.startsWith("#TEXTSCOPE-DEMO\n");
    }
    void parse(QIODevice& input, QIODevice& output, const std::atomic_bool& cancel) const override
    {
        if (input.read(16) != "#TEXTSCOPE-DEMO\n")
            throw std::runtime_error("Invalid demonstration header");
        while (!input.atEnd()) {
            if (cancel.load()) throw std::runtime_error("Cancelled");
            const auto block = input.read(64 * 1024);
            if (block.isEmpty() && !input.atEnd())
                throw std::runtime_error("Parser read failed");
            if (output.write(block) != block.size())
                throw std::runtime_error("Parser write failed");
        }
    }
};

#include "demo.moc"
