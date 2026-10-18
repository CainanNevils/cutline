#include "ui/theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QSvgRenderer>

namespace cut::ui {

namespace {
// Style sheets take images from files: small SVGs for the spin box arrows and check marks.
QString styleImages() {
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath("cutline-style");
    QDir().mkpath(dir);
    const std::pair<const char*, const char*> files[] = {
        {"up.svg", R"(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="5" viewBox="0 0 8 5"><path d="M0 5L4 0l4 5z" fill="#b8b8b8"/></svg>)"},
        {"down.svg", R"(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="5" viewBox="0 0 8 5"><path d="M0 0l4 5 4-5z" fill="#b8b8b8"/></svg>)"},
        {"up_off.svg", R"(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="5" viewBox="0 0 8 5"><path d="M0 5L4 0l4 5z" fill="#555555"/></svg>)"},
        {"down_off.svg", R"(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="5" viewBox="0 0 8 5"><path d="M0 0l4 5 4-5z" fill="#555555"/></svg>)"},
        {"check.svg", R"(<svg xmlns="http://www.w3.org/2000/svg" width="13" height="13" viewBox="0 0 13 13"><path d="M3 6.5l2.5 2.5L10 4" stroke="#ffffff" stroke-width="1.8" fill="none" stroke-linecap="round" stroke-linejoin="round"/></svg>)"},
    };
    for (const auto& [name, svg] : files) {
        QFile f(QDir(dir).filePath(name));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(svg);
    }
    return QDir::fromNativeSeparators(dir);
}
} // namespace

void applyTheme(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, col::panel);
    p.setColor(QPalette::WindowText, col::text);
    p.setColor(QPalette::Base, col::bg);
    p.setColor(QPalette::AlternateBase, col::panel2);
    p.setColor(QPalette::ToolTipBase, col::panel2);
    p.setColor(QPalette::ToolTipText, col::text);
    p.setColor(QPalette::Text, col::text);
    p.setColor(QPalette::Button, col::panel2);
    p.setColor(QPalette::ButtonText, col::text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, col::accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, col::accent);
    p.setColor(QPalette::PlaceholderText, col::textDim);
    p.setColor(QPalette::Disabled, QPalette::Text, col::textDim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, col::textDim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, col::textDim);
    app.setPalette(p);
    QFont f("Segoe UI", 9);
    app.setFont(f);
    QString css = R"(
        QMainWindow::separator { background: #141414; width: 4px; height: 4px; }
        QMainWindow::separator:hover { background: #2d8ceb; }
        QDockWidget { titlebar-close-icon: none; color: #c8c8c8; }
        QDockWidget::title { background: #262626; padding: 4px 8px; border-bottom: 1px solid #141414; }
        QTabBar::tab { background: #232323; color: #9a9a9a; padding: 5px 12px; border: none; }
        QTabBar::tab:selected { color: #e8e8e8; border-bottom: 2px solid #2d8ceb; }
        QTabBar::tab:hover { color: #e0e0e0; }
        QToolButton { border: 1px solid transparent; border-radius: 3px; padding: 3px; }
        QToolButton:hover { background: #333333; border-color: #3a3a3a; }
        QToolButton:checked { background: #2d4f72; border-color: #2d8ceb; }
        QPushButton { background: #2f2f2f; border: 1px solid #3c3c3c; border-radius: 3px; padding: 4px 12px; }
        QPushButton:hover { background: #383838; }
        QPushButton:default { border-color: #2d8ceb; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit, QTextEdit {
            background: #181818; border: 1px solid #3a3a3a; border-radius: 2px; padding: 2px 4px;
            selection-background-color: #2d8ceb; }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QPlainTextEdit:focus { border-color: #2d8ceb; }
        QHeaderView::section { background: #262626; color: #a0a0a0; border: none; border-right: 1px solid #1a1a1a; padding: 3px 6px; }
        QTreeView, QListView, QTableView { border: none; outline: 0; }
        QTreeView::item:selected, QListView::item:selected, QTableView::item:selected { background: #2d4f72; }
        QScrollBar:vertical { background: #1d1d1d; width: 11px; margin: 0; }
        QScrollBar:horizontal { background: #1d1d1d; height: 11px; margin: 0; }
        QScrollBar::handle { background: #454545; border-radius: 4px; min-height: 24px; min-width: 24px; margin: 2px; }
        QScrollBar::handle:hover { background: #5a5a5a; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QMenu { background: #262626; border: 1px solid #3a3a3a; }
        QMenu::item { padding: 4px 24px 4px 20px; }
        QMenu::item:selected { background: #2d8ceb; }
        QMenu::separator { height: 1px; background: #3a3a3a; margin: 4px 6px; }
        QMenuBar { background: #1f1f1f; }
        QMenuBar::item:selected { background: #333333; }
        QStatusBar { background: #1a1a1a; color: #9a9a9a; }
        QToolTip { background: #2b2b2b; color: #e0e0e0; border: 1px solid #444; }
        QSlider::groove:horizontal { height: 4px; background: #3a3a3a; border-radius: 2px; }
        QSlider::handle:horizontal { width: 12px; margin: -5px 0; background: #c8c8c8; border-radius: 6px; }
        QSlider::groove:vertical { width: 4px; background: #3a3a3a; border-radius: 2px; }
        QSlider::handle:vertical { height: 12px; margin: 0 -5px; background: #c8c8c8; border-radius: 6px; }
        QGroupBox { border: 1px solid #333; border-radius: 3px; margin-top: 10px; padding-top: 6px; }
        QGroupBox::title { subcontrol-origin: margin; left: 8px; color: #b0b0b0; }
        QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid #5a5a5a; border-radius: 2px; background: #181818; }
        QCheckBox::indicator:hover { border-color: #2d8ceb; }
        QCheckBox::indicator:checked { background: #2d8ceb; border-color: #2d8ceb; image: url(IMG/check.svg); }
        QCheckBox::indicator:disabled { border-color: #3a3a3a; background: #202020; }
        QCheckBox::indicator:checked:disabled { background: #3a4a5a; }
        QSpinBox::up-button, QDoubleSpinBox::up-button {
            subcontrol-origin: border; subcontrol-position: top right; width: 15px;
            border-left: 1px solid #333; border-top-right-radius: 2px; background: #222; }
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            subcontrol-origin: border; subcontrol-position: bottom right; width: 15px;
            border-left: 1px solid #333; border-bottom-right-radius: 2px; background: #222; }
        QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
        QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover { background: #343434; }
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(IMG/up.svg); width: 8px; height: 5px; }
        QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(IMG/down.svg); width: 8px; height: 5px; }
        QSpinBox::up-arrow:disabled, QDoubleSpinBox::up-arrow:disabled,
        QSpinBox::up-arrow:off, QDoubleSpinBox::up-arrow:off { image: url(IMG/up_off.svg); }
        QSpinBox::down-arrow:disabled, QDoubleSpinBox::down-arrow:disabled,
        QSpinBox::down-arrow:off, QDoubleSpinBox::down-arrow:off { image: url(IMG/down_off.svg); }
    )";
    app.setStyleSheet(css.replace("IMG", styleImages()));
}

namespace {
// 24x24 glyphs. "C" is replaced by the tint colour.
const QHash<QString, QString>& glyphs() {
    static const QHash<QString, QString> g = {
        {"play", R"(<path d="M7 4.5v15l12-7.5z" fill="C"/>)"},
        {"pause", R"(<rect x="6" y="5" width="4" height="14" fill="C"/><rect x="14" y="5" width="4" height="14" fill="C"/>)"},
        {"stepback", R"(<rect x="5" y="5" width="2.5" height="14" fill="C"/><path d="M19 5v14l-10-7z" fill="C"/>)"},
        {"stepfwd", R"(<rect x="16.5" y="5" width="2.5" height="14" fill="C"/><path d="M5 5v14l10-7z" fill="C"/>)"},
        {"gotoin", R"(<path d="M5 4v16M9 6v12l9-6z" stroke="C" stroke-width="2" fill="C"/>)"},
        {"gotoout", R"(<path d="M19 4v16M15 6v12l-9-6z" stroke="C" stroke-width="2" fill="C"/>)"},
        {"markin", R"(<path d="M9 4H6v16h3" stroke="C" stroke-width="2.2" fill="none"/>)"},
        {"markout", R"(<path d="M15 4h3v16h-3" stroke="C" stroke-width="2.2" fill="none"/>)"},
        {"insert", R"(<path d="M4 7h7v10H4zM13 7h7v10h-7z" stroke="C" stroke-width="1.6" fill="none"/><path d="M12 2v8M9 7l3 3 3-3" stroke="C" stroke-width="1.8" fill="none"/>)"},
        {"overwrite", R"(<rect x="5" y="9" width="14" height="10" stroke="C" stroke-width="1.6" fill="none"/><path d="M12 2v9M9 8l3 3 3-3" stroke="C" stroke-width="1.8" fill="none"/>)"},
        {"select", R"(<path d="M6 3l12 9-5.5 1 3.5 7-2.5 1-3.5-7L6 18z" fill="C"/>)"},
        {"razor", R"(<path d="M4 20L20 4M14 4h6v6" stroke="C" stroke-width="1.8" fill="none"/><circle cx="7" cy="17" r="2.5" stroke="C" fill="none" stroke-width="1.5"/>)"},
        {"ripple", R"(<path d="M4 5v14M8 9l4 3-4 3M20 5v14M16 9l-4 3 4 3" stroke="C" stroke-width="1.8" fill="none"/>)"},
        {"text", R"(<path d="M5 5h14M12 5v15M9 20h6" stroke="C" stroke-width="2.2" fill="none"/>)"},
        {"hand", R"(<path d="M8 12V6a1.5 1.5 0 013 0v5V4.5a1.5 1.5 0 013 0V11V6a1.5 1.5 0 013 0v8c0 4-2.5 7-6.5 7S5 18 4 15l-1-3a1.5 1.5 0 012.6-1.4L8 13z" stroke="C" stroke-width="1.5" fill="none"/>)"},
        {"zoom", R"(<circle cx="10" cy="10" r="6" stroke="C" stroke-width="2" fill="none"/><path d="M14.5 14.5L20 20M7 10h6M10 7v6" stroke="C" stroke-width="2"/>)"},
        {"lock", R"(<rect x="5" y="11" width="14" height="9" rx="1.5" fill="C"/><path d="M8 11V8a4 4 0 018 0v3" stroke="C" stroke-width="2" fill="none"/>)"},
        {"unlock", R"(<rect x="5" y="11" width="14" height="9" rx="1.5" stroke="C" stroke-width="1.6" fill="none"/><path d="M8 11V8a4 4 0 017.5-2" stroke="C" stroke-width="1.6" fill="none"/>)"},
        {"eye", R"(<path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z" stroke="C" stroke-width="1.6" fill="none"/><circle cx="12" cy="12" r="3" fill="C"/>)"},
        {"eyeoff", R"(<path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z" stroke="C" stroke-width="1.6" fill="none" opacity=".5"/><path d="M4 4l16 16" stroke="C" stroke-width="2"/>)"},
        {"speaker", R"(<path d="M4 9h4l5-4v14l-5-4H4z" fill="C"/><path d="M16 8a5 5 0 010 8M18.5 5.5a8.5 8.5 0 010 13" stroke="C" stroke-width="1.6" fill="none"/>)"},
        {"mute", R"(<path d="M4 9h4l5-4v14l-5-4H4z" fill="C"/><path d="M16 9l5 6M21 9l-5 6" stroke="C" stroke-width="1.8"/>)"},
        {"magnet", R"(<path d="M6 4v8a6 6 0 0012 0V4h-4v8a2 2 0 01-4 0V4z" stroke="C" stroke-width="1.6" fill="none"/><path d="M6 7h4M14 7h4" stroke="C" stroke-width="1.6"/>)"},
        {"marker", R"(<path d="M7 3h10v12l-5 5-5-5z" fill="C"/>)"},
        {"link", R"(<path d="M10 14a4 4 0 005.7 0l3-3a4 4 0 00-5.7-5.7l-1 1M14 10a4 4 0 00-5.7 0l-3 3a4 4 0 005.7 5.7l1-1" stroke="C" stroke-width="1.8" fill="none"/>)"},
        {"folder", R"(<path d="M3 6a1 1 0 011-1h5l2 2h9a1 1 0 011 1v10a1 1 0 01-1 1H4a1 1 0 01-1-1z" fill="C"/>)"},
        {"film", R"(<rect x="3" y="5" width="18" height="14" rx="1" stroke="C" stroke-width="1.6" fill="none"/><path d="M7 5v14M17 5v14M3 9h4M3 15h4M17 9h4M17 15h4" stroke="C" stroke-width="1.4"/>)"},
        {"wave", R"(<path d="M3 12h2M7 8v8M10 5v14M13 9v6M16 7v10M19 10v4" stroke="C" stroke-width="1.8"/>)"},
        {"image", R"(<rect x="3" y="5" width="18" height="14" rx="1" stroke="C" stroke-width="1.6" fill="none"/><path d="M5 17l5-6 4 4 2-2 3 4" stroke="C" stroke-width="1.6" fill="none"/><circle cx="16" cy="9" r="1.6" fill="C"/>)"},
        {"proxy", R"(<rect x="3" y="6" width="12" height="12" stroke="C" stroke-width="1.6" fill="none"/><rect x="11" y="3" width="10" height="8" fill="C"/>)"},
        {"safe", R"(<rect x="3" y="5" width="18" height="14" stroke="C" stroke-width="1.4" fill="none"/><rect x="6" y="8" width="12" height="8" stroke="C" stroke-width="1.2" stroke-dasharray="2 1.5" fill="none"/>)"},
        {"add", R"(<path d="M12 5v14M5 12h14" stroke="C" stroke-width="2"/>)"},
        {"trash", R"(<path d="M5 7h14M10 7V5h4v2M7 7l1 13h8l1-13" stroke="C" stroke-width="1.6" fill="none"/>)"},
        {"key", R"(<path d="M12 5l7 7-7 7-7-7z" fill="C"/>)"},
        {"keyhollow", R"(<path d="M12 5l7 7-7 7-7-7z" stroke="C" stroke-width="1.6" fill="none"/>)"},
        {"stopwatch", R"(<circle cx="12" cy="13" r="7" stroke="C" stroke-width="1.6" fill="none"/><path d="M12 13V9M10 3h4M12 3v3" stroke="C" stroke-width="1.6"/>)"},
        {"prev", R"(<path d="M15 6l-6 6 6 6" stroke="C" stroke-width="2" fill="none"/>)"},
        {"next", R"(<path d="M9 6l6 6-6 6" stroke="C" stroke-width="2" fill="none"/>)"},
        {"sequence", R"(<rect x="3" y="5" width="18" height="14" rx="2" stroke="C" stroke-width="1.6" fill="none"/><path d="M6 9h7M9 12h9M6 15h5" stroke="C" stroke-width="2" fill="none"/>)"},
        {"ratestretch", R"(<path d="M3 6v12M21 6v12M7 12h10M7 12l3-3M7 12l3 3M17 12l-3-3M17 12l-3 3" stroke="C" stroke-width="1.8" fill="none"/>)"},
        {"record", R"(<rect x="9" y="3" width="6" height="11" rx="3" stroke="C" stroke-width="1.7" fill="none"/><path d="M6 11a6 6 0 0012 0M12 17v4M9 21h6" stroke="C" stroke-width="1.7" fill="none"/>)"},
        {"plus", R"(<path d="M12 5v14M5 12h14" stroke="C" stroke-width="2" fill="none" stroke-linecap="round"/>)"},
        {"transition", R"(<rect x="3" y="6" width="11" height="12" stroke="C" stroke-width="1.6" fill="none"/><rect x="10" y="6" width="11" height="12" fill="C" opacity=".45"/>)"},
        {"preset", R"(<path d="M7 3h10v18l-5-4-5 4z" stroke="C" stroke-width="1.7" fill="none" stroke-linejoin="round"/>)"},
        {"reset", R"(<path d="M5 12a7 7 0 107-7H8M8 2L5 5l3 3" stroke="C" stroke-width="1.8" fill="none"/>)"},
        {"fx", R"(<text x="3" y="17" font-family="Segoe UI" font-weight="bold" font-style="italic" font-size="13" fill="C">fx</text>)"},
        {"list", R"(<path d="M8 6h12M8 12h12M8 18h12M4 6h1M4 12h1M4 18h1" stroke="C" stroke-width="2"/>)"},
        {"grid", R"(<rect x="4" y="4" width="7" height="7" fill="C"/><rect x="13" y="4" width="7" height="7" fill="C"/><rect x="4" y="13" width="7" height="7" fill="C"/><rect x="13" y="13" width="7" height="7" fill="C"/>)"},
        {"import", R"(<path d="M12 3v11M8 10l4 4 4-4M4 15v5h16v-5" stroke="C" stroke-width="1.8" fill="none"/>)"},
        {"caption", R"(<rect x="3" y="5" width="18" height="14" rx="1.5" stroke="C" stroke-width="1.6" fill="none"/><path d="M6 12h5M13 12h5M6 15h8" stroke="C" stroke-width="1.6"/>)"},
        {"target", R"(<rect x="5" y="7" width="14" height="10" rx="2" fill="C"/>)"},
        {"loop", R"(<path d="M4 12a6 6 0 016-6h8M15 3l3 3-3 3M20 12a6 6 0 01-6 6H6M9 21l-3-3 3-3" stroke="C" stroke-width="1.6" fill="none"/>)"},
        {"lift", R"(<rect x="3" y="14" width="18" height="6" stroke="C" stroke-width="1.6" fill="none"/><rect x="9" y="14" width="6" height="6" fill="C" opacity=".35"/><path d="M12 11V3M9 6l3-3 3 3" stroke="C" stroke-width="1.8" fill="none"/>)"},
        {"extract", R"(<path d="M3 14h6v6H3zM15 14h6v6h-6z" stroke="C" stroke-width="1.6" fill="none"/><path d="M12 11V3M9 6l3-3 3 3M10 17h4" stroke="C" stroke-width="1.8" fill="none"/>)"},
        {"camera", R"(<path d="M3 8h4l2-3h6l2 3h4v11H3z" stroke="C" stroke-width="1.6" fill="none"/><circle cx="12" cy="13" r="3.5" stroke="C" stroke-width="1.6" fill="none"/>)"},
        {"cutline", R"(<rect x="2" y="4" width="20" height="16" rx="3" fill="#2d8ceb"/><path d="M8 8l4 4-4 4M12 7l4 10" stroke="#fff" stroke-width="2" fill="none"/>)"},
    };
    return g;
}
} // namespace

QIcon icon(const QString& name, const QColor& color) {
    static QHash<QString, QIcon> cache;
    QString key = name + color.name(QColor::HexArgb);
    auto it = cache.find(key);
    if (it != cache.end()) return *it;
    QString body = glyphs().value(name);
    if (body.isEmpty()) return {};
    body.replace("\"C\"", "\"" + color.name() + "\"");
    QByteArray svg = ("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">" + body + "</svg>").toUtf8();
    QSvgRenderer r(svg);
    QIcon ic;
    for (int s : {16, 20, 24, 32, 48, 64, 256}) {
        QPixmap pm(s, s);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        r.render(&p);
        p.end();
        ic.addPixmap(pm);
    }
    cache.insert(key, ic);
    return ic;
}

QIcon swatchIcon(uint32_t argb) {
    QPixmap pm(12, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor::fromRgba(argb));
    p.drawRoundedRect(QRectF(1, 1, 10, 10), 2, 2);
    return QIcon(pm);
}

} // namespace cut::ui
