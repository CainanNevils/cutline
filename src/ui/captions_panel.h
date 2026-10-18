#pragma once
#include <QWidget>

class QTableWidget;

namespace cut::ui {

class Session;

class CaptionsPanel : public QWidget {
    Q_OBJECT
public:
    explicit CaptionsPanel(Session* s, QWidget* parent = nullptr);
    void importSrt();
    void exportSrt();
    void styleDialog();

private:
    void rebuild();
    Session* s_;
    QTableWidget* table_;
    bool updating_ = false;
};

} // namespace cut::ui
