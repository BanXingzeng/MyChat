#include "loginwidget.h"
#include <QApplication>
#include <QIcon>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>
//#include "uipage/chatwindow.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // Qt 自带对话框（确定/取消、是/否）的按钮文字来自 Qt 的翻译文件，
    // 程序自身文案是中文写死的。这里补上；找不到文件就跳过，不影响启动。
    QTranslator qtTranslator;
    const QStringList qmDirs = {
        QCoreApplication::applicationDirPath() + "/translations",
        QLibraryInfo::location(QLibraryInfo::TranslationsPath)
    };
    for (const QString &dir : qmDirs) {
        if (qtTranslator.load("qt_" + QLocale::system().name(), dir)) {
            a.installTranslator(&qtTranslator);
            break;
        }
    }

    // 标题栏 / 任务栏图标，和 exe 图标用同一个
    a.setWindowIcon(QIcon(QStringLiteral(":/resource/ico/app.ico")));
    LoginWidget w;
//    ChatWindow w;
    w.show();
    return a.exec();
}
