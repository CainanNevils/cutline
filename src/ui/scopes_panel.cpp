#include "ui/scopes_panel.h"

#include "engine/playback.h"
#include "ui/session.h"
#include "ui/theme.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace cut::ui {

namespace {

constexpr double kKr = 0.2126, kKb = 0.0722, kKg = 1 - kKr - kKb;  // Rec.709
constexpr double kVecScale = 236;  // vectorscope px per unit of Cb/Cr (of 256)

inline int luma8(int r, int g, int b) { return std::clamp(int(kKr * r + kKg * g + kKb * b + 0.5), 0, 255); }

// count -> 0..1 with a soft knee so single pixels stay visible and solid areas saturate
inline double density(int count, double full) { return count <= 0 ? 0.0 : std::min(1.0, std::sqrt(count / full)); }

void vecPoint(double r, double g, double b, double& x, double& y) {
    const double yv = kKr * r + kKg * g + kKb * b;
    const double cb = (b - yv) / 1.8556, cr = (r - yv) / 1.5748;
    x = 128 + cb * kVecScale;
    y = 128 - cr * kVecScale;
}

} // namespace

ScopesPanel::ScopesPanel(Session* s, QWidget* parent) : QWidget(parent), s_(s) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(2);
    auto* top = new QHBoxLayout;
    modeBox_ = new QComboBox(this);
    modeBox_->addItems({tr("Waveform (Luma)"), tr("RGB Parade"), tr("Vectorscope"), tr("Histogram")});
    modeBox_->setToolTip(tr("Scope type (measures the Program monitor)"));
    top->addWidget(modeBox_);
    top->addStretch(1);
    v->addLayout(top);
    v->addStretch(1);
    setMinimumSize(220, 160);
    QSettings st;
    mode_ = Mode(std::clamp(st.value("scopes/mode", 0).toInt(), 0, 3));
    modeBox_->setCurrentIndex(int(mode_));
    connect(modeBox_, &QComboBox::currentIndexChanged, this, [this](int i) {
        setMode(Mode(i));
        QSettings().setValue("scopes/mode", i);
    });
    connect(s_, &Session::scopeFrame, this, [this](int viewer, const QImage& img) {
        if (active_ && viewer == s_->programViewer()) setFrame(img);
    });
    connect(s_, &Session::engineRestarted, this, [this] {
        if (active_) setActive(true);
    });
    connect(s_, &Session::changed, this, [this] {
        if (!frame_.isNull() && s_->seq().clips.empty() && s_->seq().captions.empty()) {
            frame_ = {};
            rebuild();
        }
    });
}

void ScopesPanel::setMode(Mode m) {
    mode_ = m;
    if (modeBox_->currentIndex() != int(m)) modeBox_->setCurrentIndex(int(m));
    rebuild();
}

void ScopesPanel::setActive(bool on) {
    active_ = on;
    if (auto* e = s_->engine()) e->setScopes(s_->programViewer(), on);
}

void ScopesPanel::hideEvent(QHideEvent*) { setActive(false); }

void ScopesPanel::setFrame(const QImage& rgba) {
    frame_ = rgba.format() == QImage::Format_RGBA8888 ? rgba : rgba.convertToFormat(QImage::Format_RGBA8888);
    rebuild();
}

void ScopesPanel::rebuild() {
    scope_ = {};
    for (auto& h : hist_) h.fill(0);
    if (frame_.isNull()) {
        update();
        return;
    }
    const int W = frame_.width(), H = frame_.height();
    switch (mode_) {
    case Mode::Waveform:
    case Mode::Parade: {
        const int planes = mode_ == Mode::Parade ? 3 : 1;
        std::vector<int> counts(size_t(W) * planes * 256, 0);
        const int stride = W * planes;
        for (int y = 0; y < H; ++y) {
            const uchar* p = frame_.constScanLine(y);
            for (int x = 0; x < W; ++x, p += 4) {
                if (planes == 1) {
                    ++counts[size_t(255 - luma8(p[0], p[1], p[2])) * stride + x];
                } else {
                    for (int c = 0; c < 3; ++c) ++counts[size_t(255 - p[c]) * stride + c * W + x];
                }
            }
        }
        scope_ = QImage(stride, 256, QImage::Format_RGB32);
        const double full = std::max(4.0, H * 0.04);
        static const int tint[4][3] = {{255, 80, 80}, {80, 255, 110}, {90, 150, 255}, {150, 255, 170}};
        for (int y = 0; y < 256; ++y) {
            auto* line = reinterpret_cast<QRgb*>(scope_.scanLine(y));
            for (int x = 0; x < stride; ++x) {
                const double d = density(counts[size_t(y) * stride + x], full);
                const int* t = tint[planes == 1 ? 3 : x / W];
                line[x] = qRgb(int(t[0] * d), int(t[1] * d), int(t[2] * d));
            }
        }
        break;
    }
    case Mode::Vectorscope: {
        struct Acc {
            int n = 0;
            float r = 0, g = 0, b = 0;
        };
        std::vector<Acc> acc(256 * 256);
        for (int y = 0; y < H; ++y) {
            const uchar* p = frame_.constScanLine(y);
            for (int x = 0; x < W; ++x, p += 4) {
                double vx, vy;
                vecPoint(p[0] / 255.0, p[1] / 255.0, p[2] / 255.0, vx, vy);
                const int ix = std::clamp(int(vx), 0, 255), iy = std::clamp(int(vy), 0, 255);
                Acc& a = acc[size_t(iy) * 256 + ix];
                ++a.n;
                a.r += p[0];
                a.g += p[1];
                a.b += p[2];
            }
        }
        scope_ = QImage(256, 256, QImage::Format_ARGB32_Premultiplied);
        const double full = std::max(8.0, double(W) * H * 0.0005);
        for (int y = 0; y < 256; ++y) {
            auto* line = reinterpret_cast<QRgb*>(scope_.scanLine(y));
            for (int x = 0; x < 256; ++x) {
                const Acc& a = acc[size_t(y) * 256 + x];
                if (!a.n) {
                    line[x] = 0;
                    continue;
                }
                // average colour pushed to full brightness, mixed with white so dark hues show
                double r = a.r / a.n, g = a.g / a.n, b = a.b / a.n;
                const double m = std::max({r, g, b, 1.0});
                r = 0.65 * r / m + 0.35;
                g = 0.65 * g / m + 0.35;
                b = 0.65 * b / m + 0.35;
                const double d = density(a.n, full);
                const int al = int(255 * d);
                line[x] = qRgba(int(255 * r * d), int(255 * g * d), int(255 * b * d), al);
            }
        }
        break;
    }
    case Mode::Histogram:
        for (int y = 0; y < H; ++y) {
            const uchar* p = frame_.constScanLine(y);
            for (int x = 0; x < W; ++x, p += 4) {
                ++hist_[0][p[0]];
                ++hist_[1][p[1]];
                ++hist_[2][p[2]];
                ++hist_[3][luma8(p[0], p[1], p[2])];
            }
        }
        break;
    }
    update();
}

QRect ScopesPanel::plotRect() const {
    const int top = modeBox_->geometry().bottom() + 8;
    QRect r(34, top, width() - 34 - 10, height() - top - 8);
    if (mode_ == Mode::Vectorscope) {
        const int side = std::min(width() - 20, r.height());
        r = QRect((width() - side) / 2, top, side, side);
    }
    return r;
}

void ScopesPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x16, 0x16, 0x17));
    const QRect pr = plotRect();
    if (pr.width() < 20 || pr.height() < 20) return;
    p.fillRect(pr, QColor(0x0c, 0x0c, 0x0d));
    QFont f = font();
    f.setPointSizeF(7.5);
    p.setFont(f);
    const QColor grid(255, 255, 255, 38), label(0x8c, 0x8c, 0x8c);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (!scope_.isNull()) p.drawImage(pr, scope_);

    if (mode_ == Mode::Waveform || mode_ == Mode::Parade) {
        for (int pct = 0; pct <= 100; pct += 25) {
            const int y = pr.bottom() - int(pr.height() * pct / 100.0 + 0.5);
            p.setPen(grid);
            p.drawLine(pr.left(), y, pr.right(), y);
            p.setPen(label);
            p.drawText(QRect(0, y - 8, pr.left() - 4, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(pct));
        }
        if (mode_ == Mode::Parade) {
            p.setPen(QColor(255, 255, 255, 70));
            for (int i = 1; i < 3; ++i) {
                const int x = pr.left() + pr.width() * i / 3;
                p.drawLine(x, pr.top(), x, pr.bottom());
            }
        }
    } else if (mode_ == Mode::Vectorscope) {
        p.setRenderHint(QPainter::Antialiasing);
        const double s = pr.width() / 256.0;
        auto map = [&](double x, double y) { return QPointF(pr.left() + x * s, pr.top() + y * s); };
        const QPointF c = map(128, 128);
        p.setPen(grid);
        p.setBrush(Qt::NoBrush);
        // 100 % saturation boundary (approximately the hexagon of primaries) and the 75 % one
        for (double sat : {1.0, 0.75}) {
            QPolygonF hex;
            const double prim[6][3] = {{1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {1, 1, 0}};
            for (const auto& q : prim) {
                double x, y;
                vecPoint(q[0] * sat, q[1] * sat, q[2] * sat, x, y);
                hex << map(x, y);
            }
            hex << hex.front();
            p.drawPolyline(hex);
        }
        p.drawLine(map(128, 4), map(128, 252));
        p.drawLine(map(4, 128), map(252, 128));
        // skin tone line (I axis, ~123 degrees)
        p.setPen(QColor(230, 180, 140, 90));
        const double ang = 123.0 * std::numbers::pi / 180.0;
        p.drawLine(c, QPointF(c.x() + std::cos(ang) * 120 * s, c.y() - std::sin(ang) * 120 * s));
        // 75 % targets with labels
        struct Tg {
            const char* n;
            double r, g, b;
        };
        const Tg tg[] = {{"R", .75, 0, 0}, {"Mg", .75, 0, .75}, {"B", 0, 0, .75},
                         {"Cy", 0, .75, .75}, {"G", 0, .75, 0}, {"Yl", .75, .75, 0}};
        for (const Tg& t : tg) {
            double x, y;
            vecPoint(t.r, t.g, t.b, x, y);
            const QPointF q = map(x, y);
            p.setPen(QColor(255, 255, 255, 110));
            p.drawRect(QRectF(q.x() - 4, q.y() - 4, 8, 8));
            const QPointF dir = q - c;
            const double len = std::hypot(dir.x(), dir.y());
            const QPointF lp = len > 0 ? q + dir / len * 12 : q;
            p.setPen(label);
            p.drawText(QRectF(lp.x() - 10, lp.y() - 7, 20, 14), Qt::AlignCenter, t.n);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
        if (!scope_.isNull()) p.drawImage(pr, scope_);  // trace above the graticule
    } else if (!frame_.isNull()) {  // histogram
        int peak = 1;
        for (int c = 0; c < 4; ++c)
            for (int i = 1; i < 255; ++i) peak = std::max(peak, hist_[c][i]);  // ignore clipped ends
        p.setPen(grid);
        for (int i = 1; i < 4; ++i) {
            const int x = pr.left() + pr.width() * i / 4;
            p.drawLine(x, pr.top(), x, pr.bottom());
        }
        p.setRenderHint(QPainter::Antialiasing);
        const QColor fill[3] = {QColor(255, 60, 60, 110), QColor(60, 255, 90, 110), QColor(70, 130, 255, 110)};
        auto path = [&](int c) {
            QPainterPath pp;
            pp.moveTo(pr.left(), pr.bottom());
            for (int i = 0; i < 256; ++i) {
                const double v = std::min(1.0, double(hist_[c][i]) / peak);
                pp.lineTo(pr.left() + pr.width() * (i + 0.5) / 256.0, pr.bottom() - v * (pr.height() - 2));
            }
            pp.lineTo(pr.right(), pr.bottom());
            pp.closeSubpath();
            return pp;
        };
        p.setPen(Qt::NoPen);
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        for (int c = 0; c < 3; ++c) {
            p.setBrush(fill[c]);
            p.drawPath(path(c));
        }
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(235, 235, 235, 200), 1.2));
        p.drawPath(path(3));
    }
    if (frame_.isNull()) {
        p.setPen(label);
        f.setPointSizeF(9);
        p.setFont(f);
        p.drawText(pr, Qt::AlignCenter, tr("No video in the Program monitor"));
    }
}

} // namespace cut::ui
