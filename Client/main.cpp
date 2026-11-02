#include "loginwidget.h"
#include <QApplication>
#include <QIcon>
//#include "uipage/chatwindow.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    // 标题栏 / 任务栏图标，和 exe 图标用同一个
    a.setWindowIcon(QIcon(QStringLiteral(":/resource/ico/app.ico")));
    LoginWidget w;
//    ChatWindow w;
    w.show();
    return a.exec();
}
