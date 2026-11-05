#pragma once

#include <QTableView>
#include <functional>

namespace textscope {
class FilterView final : public QTableView {
public:
    explicit FilterView(QWidget* parent = nullptr);
    std::function<void(const QString&)> command;
    bool checkboxAt(const QPoint& position) const;
protected:
    void keyPressEvent(QKeyEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
private:
    bool checkPressed_ = false;
    QPersistentModelIndex pressedIndex_;
};
}
