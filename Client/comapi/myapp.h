#ifndef MYAPP_H
#define MYAPP_H

#include <QObject>
#include <QVariant>

class QApplication; //类的向前声明

class MyApp
{
public:
    //系统配置
    static QString m_strAppPath;         //应用程序路径
    static QString m_strDataPath;        //数据保存路径
    static QString m_strRecvPath;        //接收文件目录
    static QString m_strDatabasePath;    //数据库路径
    static QString m_strConfPath;      //配置路径
    static QString m_strSoundPath;       // 配置路径
    static QString m_strRecordPath;      // 录音路径

    static QString m_strFacePath;   //表情
    static QString m_strHeadPath;   //头像路径

    static QString m_strIniFile;         // 配置文件

    static QString m_strHostAddr;       // 服务器地址配置
    static quint16     m_nMsgPort;          // 聊天消息服务器端口配置
    static quint16     m_nFilePort;          // 文件转发服务器端口配置
    static quint16     m_nGroupPort;        // 群组聊天窗口

    static QString m_strUserName;       // 用户名
    static QString m_strPassword;       // 用户密码（内存里始终保留，重连要用）
    static bool    m_bRememberPwd;      // 记住密码（决定口令是否落盘）
    static QString m_strHeadFile;       // 头像文件


    static int     m_nId;

    static int     m_nWinX;
    static int     m_nWinY;

    //初始化
    static void InitApp(const QString &appPath);
    // 创建配置文件
    static void CreatorSettingFile();
    // 读配置文件，加载系统配置
    static void ReadSettingFile();

    // 读取/修改配置文件
    static void SetSettingFile(const QString &group, const QString &key, const QVariant &value);
    static QVariant GetSettingKeyValue(const QString &group, const QString &key, const QVariant &value);
    // 检查目录
    static void CheckDirs();
    static void CheckSound();

    static void SaveConfig();
    /// 保存/清除登录口令：m_bRememberPwd 为假时直接删掉这一项。
    /// 口令用 Windows DPAPI 加密后再写盘，不再以明文躺在 config.ini 里。
    static void SavePassword(const QString &password);

    static void setDefaultHead();

    /// 头像文件名（或历史遗留的完整路径）转成本地完整路径，空值回退 default.png
    static QString HeadFullPath(const QString &head);
};

#endif // MYAPP_H
