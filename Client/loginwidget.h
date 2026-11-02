#ifndef LOGINWIDGET_H
#define LOGINWIDGET_H

#include <QWidget>
#include <QPixmap>
#include <QSize>
#include "basewidget/customwidget.h"

class MySocket;
namespace Ui {
class LoginWidget;
}

class LoginWidget : public CustomMoveWidget
{
    Q_OBJECT

public:
    explicit LoginWidget(QWidget *parent = nullptr);
    ~LoginWidget();

    void sendLogin();
    void loginSuccess(const QJsonValue &dataVal);

private slots:
    void on_btnLogin_clicked();
    void sltStatus(const quint8 &status,const QJsonValue &dataVal);

    void on_btnWinMenu_clicked();

    void on_btnCancel_clicked();

    void on_btnReister_clicked();

    // 服务器配置页
    void on_btnSaveCfg_clicked();
    void on_btnWinClose_2_clicked();
    void on_btnWinMin_2_clicked();

    // 窗口按钮
    void on_btnWinClose_clicked();
    void on_btnWinMin_clicked();

    // 连接成功/失败
    void sltConnectSuccess();
    void sltConnectFailed(const QString &errorString);

protected:
    // 监听顶部背景图控件的尺寸变化，按新尺寸重新缩放（自适应）
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void loadCfgToUi();     // 把当前生效的配置填到配置页输入框
    void saveLoginCfg();    // 记住/忘记账号密码（自动登录）
    void updateBanner();    // 顶部背景图按控件大小等比缩放（铺满、不变形）
    Ui::LoginWidget *ui;
    MySocket *m_tcpSocket;
    bool m_bAutoLogin = false;   // 连上服务器后自动提交登录

    QPixmap m_bgPixmap;          // 背景图原图（只加载一次）
    QPixmap m_bgScaled;          // 当前尺寸下缩放后的缓存，尺寸没变就不重复缩放
    QSize   m_bgScaledSize;
};

#endif // LOGINWIDGET_H
