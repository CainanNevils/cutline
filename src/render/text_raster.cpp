#include "render/text_raster.h"

#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QLinearGradient>
#include <QPainterPath>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cut {

namespace {
QColor argb(uint32_t c) { return QColor::fromRgba(c); }

// 3-pass box blur on a premultiplied ARGB32 image (approximates a gaussian).
void blur(QImage& img, int radius) {
    if (radius <= 0) return;
    radius = std::max(1, radius / 2);  // three box passes of r/2 ~ gaussian with sigma ~ r/2
    const int w = img.width(), h = img.height();
    std::vector<uint32_t> tmp(size_t(w) * h);
    auto pass = [&](bool horizontal) {
        uint32_t* px = reinterpret_cast<uint32_t*>(img.bits());
        const int stride = img.bytesPerLine() / 4;
        const int n = horizontal ? w : h, lines = horizontal ? h : w;
        for (int l = 0; l < lines; ++l) {
            int acc[4] = {0, 0, 0, 0};
            auto at = [&](int i) -> uint32_t {
                i = std::clamp(i, 0, n - 1);
                return horizontal ? px[l * stride + i] : px[i * stride + l];
            };
            for (int i = -radius; i <= radius; ++i) {
                uint32_t v = at(i);
                for (int c = 0; c < 4; ++c) acc[c] += (v >> (c * 8)) & 0xff;
            }
            const int win = 2 * radius + 1;
            for (int i = 0; i < n; ++i) {
                uint32_t o = 0;
                for (int c = 0; c < 4; ++c) o |= uint32_t(acc[c] / win) << (c * 8);
                tmp[horizontal ? size_t(l) * w + i : size_t(i) * w + l] = o;
                uint32_t add = at(i + radius + 1), sub = at(i - radius);
                for (int c = 0; c < 4; ++c) acc[c] += int((add >> (c * 8)) & 0xff) - int((sub >> (c * 8)) & 0xff);
            }
        }
        for (int y = 0; y < h; ++y) std::memcpy(px + y * stride, tmp.data() + size_t(y) * w, size_t(w) * 4);
    };
    for (int i = 0; i < 3; ++i) {
        pass(true);
        pass(false);
    }
}
} // namespace

uint64_t textHash(const std::string& s, const TextStyle& st, int visibleChars) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(s.data(), s.size());
    mix(st.font.data(), st.font.size());
    double nums[] = {st.size, double(st.weight), double(st.italic), double(st.fill), double(st.stroke), st.strokeWidth,
                     double(st.strokeColor), double(st.shadow), st.shadowX, st.shadowY, st.shadowBlur,
                     double(st.shadowColor), double(st.box), double(st.boxColor), st.boxPadding, st.boxOpacity,
                     double(st.align), st.lineSpacing, st.boxRadius, st.tracking, double(st.gradient), double(st.fill2)};
    mix(nums, sizeof(nums));
    mix(&visibleChars, sizeof(visibleChars));
    return h;
}

TextImage rasterizeText(const std::string& utf8, const TextStyle& st, double scale, int visibleChars) {
    TextImage out;
    QFont font(QString::fromStdString(st.font));
    font.setPixelSize(std::max(1, int(std::lround(st.size * scale))));
    font.setWeight(QFont::Weight(std::clamp(st.weight, 100, 900)));
    font.setItalic(st.italic);
    font.setHintingPreference(QFont::PreferNoHinting);
    if (st.tracking != 0) font.setLetterSpacing(QFont::AbsoluteSpacing, st.size * scale * st.tracking / 100.0);
    QFontMetricsF fm(font);
    QStringList lines = QString::fromStdString(utf8).split('\n');
    const double lineH = fm.height() * std::max(0.5, st.lineSpacing);
    double blockW = 1;
    for (const QString& l : lines) blockW = std::max(blockW, fm.horizontalAdvance(l));
    const double blockH = lineH * double(lines.size());
    const double sw = st.stroke ? st.strokeWidth * scale : 0;
    const double boxPad = st.box ? st.boxPadding * scale : 0;
    const double sx = st.shadow ? st.shadowX * scale : 0, sy = st.shadow ? st.shadowY * scale : 0;
    const double sb = st.shadow ? st.shadowBlur * scale : 0;
    const double pad = std::max({sw, boxPad}) + 2;
    const double shadowExtra = st.shadow ? std::max(std::abs(sx), std::abs(sy)) + sb * 2 + 2 : 0;
    const double margin = pad + shadowExtra;
    out.width = std::max(2, int(std::ceil(blockW + 2 * margin)));
    out.height = std::max(2, int(std::ceil(blockH + 2 * margin)));
    QImage img(out.width, out.height, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    QPainterPath path, fullPath;
    int remaining = visibleChars;
    for (int i = 0; i < lines.size(); ++i) {
        double lw = fm.horizontalAdvance(lines[i]);
        double x = margin + (st.align == 0 ? 0 : st.align == 1 ? (blockW - lw) / 2 : blockW - lw);
        double y = margin + lineH * i + (lineH - fm.height()) / 2 + fm.ascent();
        fullPath.addText(QPointF(x, y), font, lines[i]);
        if (visibleChars < 0) {
            path.addText(QPointF(x, y), font, lines[i]);
        } else if (remaining > 0) {
            const int n = std::min<int>(remaining, int(lines[i].size()));
            path.addText(QPointF(x, y), font, lines[i].left(n));
            remaining -= n;
        }
    }

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    if (st.box) {
        QColor bc = argb(st.boxColor);
        bc.setAlphaF(std::clamp(st.boxOpacity / 100.0, 0.0, 1.0) * bc.alphaF());
        const QRectF box(margin - boxPad, margin - boxPad, blockW + 2 * boxPad, blockH + 2 * boxPad);
        const double r = std::min(st.boxRadius * scale, std::min(box.width(), box.height()) / 2);
        p.setPen(Qt::NoPen);
        p.setBrush(bc);
        if (r > 0) p.drawRoundedRect(box, r, r);
        else p.drawRect(box);
    }
    if (st.shadow) {
        QImage sh(out.width, out.height, QImage::Format_ARGB32_Premultiplied);
        sh.fill(Qt::transparent);
        {
            QPainter sp(&sh);
            sp.setRenderHint(QPainter::Antialiasing, true);
            sp.translate(sx, sy);
            if (sw > 0) {
                QPen pen(argb(st.shadowColor), sw * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
                sp.strokePath(path, pen);
            }
            sp.fillPath(path, argb(st.shadowColor));
        }
        blur(sh, int(std::lround(sb)));
        p.drawImage(0, 0, sh);
    }
    if (sw > 0) {
        QPen pen(argb(st.strokeColor), sw * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.strokePath(path, pen);
    }
    if (st.gradient) {
        const QRectF b = fullPath.boundingRect();  // (the gradient stays put while revealing)
        QLinearGradient g(b.topLeft(), b.bottomLeft());
        g.setColorAt(0, argb(st.fill));
        g.setColorAt(1, argb(st.fill2));
        p.fillPath(path, g);
    } else {
        p.fillPath(path, argb(st.fill));
    }
    p.end();

    out.bgra.resize(size_t(out.width) * out.height * 4);
    for (int y = 0; y < out.height; ++y)
        std::memcpy(out.bgra.data() + size_t(y) * out.width * 4, img.constScanLine(y), size_t(out.width) * 4);
    return out;
}

} // namespace cut
