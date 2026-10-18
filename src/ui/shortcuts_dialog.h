#pragma once
// Keyboard shortcut editor. Every command QAction carries an objectName "cmd.<id>" and a
// "defaultShortcut" property; overrides live in QSettings group "shortcuts".
#include <QDialog>

class QAction;
class QLineEdit;
class QTableWidget;

namespace cut::ui {

// Applies saved overrides to the actions (call once after creating them).
void applyShortcutOverrides(const QList<QAction*>& actions);

class ShortcutsDialog : public QDialog {
    Q_OBJECT
public:
    ShortcutsDialog(QList<QAction*> actions, QWidget* parent = nullptr);
    void apply();

private:
    void rebuild();
    void markConflicts();
    QList<QAction*> actions_;
    QTableWidget* table_;
    QLineEdit* filter_;
};

} // namespace cut::ui
