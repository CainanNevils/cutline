#pragma once
#include <QTreeWidget>
#include <QWidget>

class QLineEdit;

namespace cut::ui {

class Session;

// Effects & transitions browser with a search field and the user's effect presets; items drag
// onto timeline clips / cuts, double-click applies to the selection (effects, presets) or the
// edit point nearest the playhead (transitions).
class EffectsPanel : public QWidget {
    Q_OBJECT
public:
    explicit EffectsPanel(Session* s, QWidget* parent = nullptr);
    QTreeWidget* tree() const { return tree_; }
    void setFilter(const QString& text);

private:
    void rebuildPresets();
    void apply(QTreeWidgetItem* it);
    void contextMenu(const QPoint& pos);
    Session* s_;
    QLineEdit* search_;
    QTreeWidget* tree_;
    QTreeWidgetItem* presets_ = nullptr;
};

} // namespace cut::ui
