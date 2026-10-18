#pragma once
// Dark editing theme (Fusion + palette + a small stylesheet) and the custom icon set.
#include <QColor>
#include <QIcon>
#include <QString>

class QApplication;

namespace cut::ui {

namespace col {
inline const QColor bg{0x1d, 0x1d, 0x1d};
inline const QColor panel{0x23, 0x23, 0x23};
inline const QColor panel2{0x2b, 0x2b, 0x2b};
inline const QColor border{0x14, 0x14, 0x14};
inline const QColor text{0xd4, 0xd4, 0xd4};
inline const QColor textDim{0x8c, 0x8c, 0x8c};
inline const QColor accent{0x2d, 0x8c, 0xeb};
inline const QColor playhead{0x4d, 0x9d, 0xff};
inline const QColor video{0x5b, 0x8f, 0xc7};     // video clips
inline const QColor audio{0x4f, 0xa8, 0x6c};     // audio clips
inline const QColor title{0xb0, 0x6c, 0xd8};     // titles
inline const QColor caption{0xd8, 0xa2, 0x3c};   // captions
inline const QColor offline{0xc0, 0x40, 0x40};
inline const QColor selection{0xf2, 0xc1, 0x4e};
inline const QColor marker{0x3f, 0xb9, 0x50};
inline const QColor inOut{0x2d, 0x8c, 0xeb, 50};
} // namespace col

void applyTheme(QApplication& app);

// Icons are simple original SVG glyphs tinted with the theme text colour.
QIcon icon(const QString& name, const QColor& color = col::text);

// Label colours shared by clips and markers.
struct LabelColor {
    const char* name;  // untranslated; pass through QCoreApplication::translate("Label", name)
    uint32_t argb;
};
inline constexpr LabelColor kLabelColors[] = {{"Green", 0xFF3FB950}, {"Red", 0xFFE5534B},  {"Violet", 0xFFB06CD8},
                                             {"Orange", 0xFFE3923C}, {"Yellow", 0xFFE8C547}, {"White", 0xFFE6E6E6},
                                             {"Blue", 0xFF4D9DFF},  {"Cyan", 0xFF39C5CF}, {"Pink", 0xFFE86FA8}};
// Small rounded colour swatch for menus.
QIcon swatchIcon(uint32_t argb);

} // namespace cut::ui
