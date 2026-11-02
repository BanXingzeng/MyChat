#ifndef MYAPP_H
#define MYAPP_H

#include <QObject>

typedef enum {
    Administrator = 0x01,
    Mangager,
    Worker
} E_IDENTITY;

class QApplication;

class MyApp
{
public:
    //=======================系统配置部分=========================//
    static QString m_strAppPath;         // 应用程序路径
    static QString m_strDataPath;        // 数据保存路径
    static QString m_strDatabasePath;    // 数据库目录
    static QString m_strConfPath;        // 配置目录
    static QString m_strBackupPath;      // 配置目录
    static QString m_strRecvPath;        // 文件接收保存目录
    static QString m_strHeadPath;        // 用户头像保存(可存放数据库)

    static QString m_strIniFile;         // 配置文件

    static QString m_strUserName;       // 用户名
    static QString m_strPassword;       // 用户密码

    static int     m_nId;
    static int     m_nIdentyfi;

    //=======================函数功能部分=========================//
    // 初始化
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

    static void SaveConfig();

    /// 头像文件名（或历史遗留的完整路径）转成本地完整路径，空值回退 default.png
    static QString HeadFullPath(const QString &head);
    /// 确保服务器头像目录里有 default.png（没有就用代码画一张），返回其完整路径
    static QString EnsureDefaultHead();
    /// 生成默认头像并保存到指定路径（96x96 PNG）
    static bool CreateDefaultHead(const QString &filePath);
};

#endif // MYAPP_H
