// lightcpp — LightC++ IDE main entry point.

#include "LightCpp/App/MainWindow.hpp"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("LightC++"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("LightCpp"));

    // High-DPI is handled automatically by Qt 6.

    MainWindow window;
    window.show();

    return app.exec();
}
