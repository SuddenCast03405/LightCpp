// lightcpp — LightC++ IDE main entry point.

#include "LightCpp/App/MainWindow.hpp"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QPainter>
#include <QPixmap>
#include <QSplashScreen>

namespace {

/// The loading screen shown while MainWindow assembles itself: locating
/// MSVC, reading the configuration files and loading every plugin all
/// happen inside its constructor, before the main window can show anything.
QPixmap makeSplashPixmap()
{
    QPixmap pix(440, 280);
    pix.fill(QColor(22, 25, 31));

    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing);

    QFont title = painter.font();
    title.setPointSize(30);
    title.setBold(true);
    painter.setFont(title);
    painter.setPen(Qt::white);
    painter.drawText(pix.rect().adjusted(0, -36, 0, -36),
                     Qt::AlignCenter, QStringLiteral("LightC++"));

    QFont status;
    status.setPointSize(10);
    painter.setFont(status);
    painter.setPen(QColor(148, 154, 165));
    painter.drawText(pix.rect().adjusted(0, 48, 0, 48),
                     Qt::AlignCenter, QStringLiteral("Loading..."));
    return pix;
}

}  // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("LightC++"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("LightCpp"));

    // High-DPI is handled automatically by Qt 6.

    // The loading screen goes up before the (slow) main-window constructor
    // so the user sees something immediately. It closes once the window is
    // exposed, with the start page already in place of the first editor tab.
    QSplashScreen splash(makeSplashPixmap());
    splash.show();
    QApplication::processEvents();  // paint it before the blocking constructor

    MainWindow window;
    window.show();
    splash.finish(&window);

    return app.exec();
}
