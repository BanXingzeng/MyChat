#include "mainwindow.h"
#include <QApplication>
#include <QIcon>
#include <QLocale>
#include <QSystemTrayIcon>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setApplicationName(QStringLiteral("局域网通信服务器"));
    a.setWindowIcon(QIcon(QStringLiteral(":/resource/images/ic_app.png")));
    // 关闭窗口只是隐藏到托盘，程序要留着继续监听
    a.setQuitOnLastWindowClosed(false);
    // 让日期/星期按中文显示
    QLocale::setDefault(QLocale(QLocale::Chinese, QLocale::China));

    MainWindow w;
    w.show();

    return a.exec();
}
