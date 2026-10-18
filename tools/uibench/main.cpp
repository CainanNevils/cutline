// uibench: timeline paint cost with a large project (budget: 500 clips pan/zoom at 60 fps, no
// frame > 33 ms). Runs offscreen; waits for thumbnails/waveforms so painting is realistic.
//   uibench <project.cutproj> [--frames N]
#include "ui/session.h"
#include "ui/theme.h"
#include "ui/timeline_widget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QScrollBar>
#include <QThread>

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <vector>

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");
    qputenv("CUTLINE_TEST", "1");
    QApplication app(argc, argv);
    QApplication::setOrganizationName("Cutline");
    QApplication::setApplicationName("Cutline");
    cut::ui::applyTheme(app);
    if (argc < 2) {
        std::fprintf(stderr, "usage: uibench <project.cutproj> [--frames N]\n");
        return 1;
    }
    int frames = 600;
    for (int i = 2; i < argc; ++i)
        if (std::string(argv[i]) == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
    cut::ui::Session s;
    QString err;
    if (!s.openProject(QString::fromLocal8Bit(argv[1]), err)) {
        std::fprintf(stderr, "%s\n", err.toLocal8Bit().constData());
        return 1;
    }
    // wait for thumbnails / waveforms (cache or generation), max 120 s
    QElapsedTimer wait;
    wait.start();
    while ((s.jobs().running() || s.jobs().pending()) && wait.elapsed() < 120000) {
        app.processEvents();
        QThread::msleep(20);
    }
    app.processEvents();
    cut::ui::TimelineWidget w(&s);
    w.resize(1900, 640);
    w.show();
    w.zoomToFit();
    app.processEvents();
    std::vector<double> ms;
    QElapsedTimer t;
    const double basePps = w.pixelsPerSecond();
    for (int i = 0; i < frames; ++i) {
        // alternate zooming in/out around the middle and panning
        const int phase = (i / 150) % 4;
        if (phase == 0) w.setPixelsPerSecond(basePps * (1 + (i % 150) * 0.12), 900);
        else if (phase == 1) w.setPixelsPerSecond(basePps * (1 + (150 - i % 150) * 0.12), 900);
        else {
            auto* hb = w.findChildren<QScrollBar*>().front();
            if (w.pixelsPerSecond() < basePps * 6) w.setPixelsPerSecond(basePps * 6, 900);
            hb->setValue(hb->value() + (phase == 2 ? 25 : -25));
        }
        t.start();
        w.repaint();
        ms.push_back(double(t.nsecsElapsed()) / 1e6);
    }
    std::sort(ms.begin(), ms.end());
    double avg = std::accumulate(ms.begin(), ms.end(), 0.0) / double(ms.size());
    std::printf("{\"test\":\"timeline_paint\",\"clips\":%zu,\"frames\":%d,\"ms_avg\":%.2f,\"ms_p99\":%.2f,\"ms_max\":%.2f,"
                "\"budget_ms\":16.7}\n",
                s.seq().clips.size(), frames, avg, ms[size_t(double(ms.size()) * 0.99)], ms.back());
    w.zoomToFit();
    w.grab().save("uibench_timeline.png");
    s.shutdown();
    return 0;
}
