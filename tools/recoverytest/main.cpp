// recoverytest: verifies crash recovery end to end with an isolated app-data folder.
//   recoverytest <project.cutproj>      (runs both phases itself)
// Phase "crash": open project, make an edit, autosave, then terminate the process abruptly (the
// session lock stays behind like after a real crash). Phase "check": a new session must offer the
// autosave and the recovered project must contain the edit.
#include "core/edits.h"
#include "ui/session.h"

#include <QApplication>
#include <QDir>
#include <QProcess>

#include <windows.h>

#include <cstdio>

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    if (argc < 2) {
        std::fprintf(stderr, "usage: recoverytest <project.cutproj>\n");
        return 2;
    }
    const std::string mode = argc > 2 ? argv[2] : "";
    if (mode.empty()) {
        QString dir = QDir::tempPath() + "/cutline_recoverytest";
        QDir(dir).removeRecursively();
        qputenv("CUTLINE_APPDATA", QDir::toNativeSeparators(dir).toLocal8Bit());
        QProcess p1;
        p1.setProcessChannelMode(QProcess::ForwardedChannels);
        p1.start(QString::fromLocal8Bit(argv[0]), {QString::fromLocal8Bit(argv[1]), "crash"});
        p1.waitForFinished(60000);
        std::printf("crash phase exit code %d (expected 3 = killed)\n", p1.exitCode());
        QProcess p2;
        p2.setProcessChannelMode(QProcess::ForwardedChannels);
        p2.start(QString::fromLocal8Bit(argv[0]), {QString::fromLocal8Bit(argv[1]), "check"});
        p2.waitForFinished(60000);
        std::printf("recoverytest: %s\n", p2.exitCode() == 0 ? "PASS" : "FAIL");
        return p2.exitCode();
    }
    QApplication app(argc, argv);
    QApplication::setOrganizationName("CutlineRecoveryTest");
    cut::ui::Session s;
    if (mode == "crash") {
        QString err;
        if (!s.openProject(QString::fromLocal8Bit(argv[1]), err)) return 10;
        s.edit("test edit", [](cut::Project& p) { cut::edit::addMarker(p.sequence, cut::kTicksPerSecond * 3, "recovery-marker"); });
        s.autosaveNow();
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 3);  // no clean shutdown: the lock file remains
    }
    if (mode == "check") {
        QString cand = s.recoveryCandidate();
        if (cand.isEmpty()) {
            std::printf("no recovery candidate offered\n");
            return 1;
        }
        QString err;
        if (!s.recover(cand, err)) {
            std::printf("recover failed: %s\n", err.toLocal8Bit().constData());
            return 1;
        }
        for (const auto& m : s.seq().markers)
            if (m.name == "recovery-marker") {
                std::printf("recovered %s with the edit\n", cand.toLocal8Bit().constData());
                s.shutdown();
                return 0;
            }
        std::printf("edit missing in recovered project\n");
        return 1;
    }
    return 2;
}
