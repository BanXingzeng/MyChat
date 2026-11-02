#include "myapp.h"

#include <QApplication>
#include <QDesktopWidget>
#include <QSettings>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QDebug>
#include <QFileInfo>
#include <QImage>
#include <QPainter>

// 应用程序路径
QString MyApp::m_strAppPath         = "./";
// 数据保存路径
QString MyApp::m_strDataPath        = "";
// 数据库目录
QString MyApp::m_strDatabasePath    = "";
// 配置目录
QString MyApp::m_strConfPath        = "";
QString MyApp::m_strBackupPath      = "";
QString MyApp::m_strRecvPath        = "";
QString MyApp::m_strHeadPath        = "";

// 配置文件
QString MyApp::m_strIniFile         = "config.ini";

QString MyApp::m_strUserName        = "bxz";
QString MyApp::m_strPassword        = "111";

int     MyApp::m_nId                = -1;
int     MyApp::m_nIdentyfi          = -1;
// 初始化
void MyApp::InitApp(const QString &appPath)
{
    m_strAppPath        = appPath + "/";
    m_strDataPath       = m_strAppPath  + "Data/";
    m_strDatabasePath   = m_strDataPath + "Database/";
    m_strConfPath       = m_strDataPath + "Conf/";
    m_strBackupPath     = m_strDataPath + "Backup/";
    m_strRecvPath       = m_strDataPath + "RecvFiles/";
    m_strHeadPath       = m_strDataPath + "UserHeads/";
    m_strIniFile        = m_strConfPath + "config.ini";

    // 检查目录
    CheckDirs();

    // 创建配置文件
    CreatorSettingFile();

    // 加载系统配置
    ReadSettingFile();
}

/**
 * @brief MyApp::creatorSettingFile 创建配置文件
 */
void MyApp::CreatorSettingFile() {
    // 写入配置文件
    QSettings settings(m_strIniFile, QSettings::IniFormat);
    QString strGroups = settings.childGroups().join("");
    if (!QFile::exists(m_strIniFile) || (strGroups.isEmpty()))
    {

        /*系统设置*/
        settings.beginGroup("UserCfg");
        settings.setValue("User",   m_strUserName);
        settings.setValue("Passwd", m_strPassword);
        settings.endGroup();
        settings.sync();

    }
#ifdef Q_WS_QWS
    QProcess::execute("sync");
#endif
}

/**
 * @brief MyApp::ReadSettingFile
 * 读取系统配置信息
 */
void MyApp::ReadSettingFile()
{
    QSettings settings(m_strIniFile, QSettings::IniFormat);
    settings.beginGroup("UserCfg");
    m_strUserName = settings.value("User", "milo").toString();
    m_strPassword = settings.value("Passwd", "123456")  .toString();
    settings.endGroup();
}

/**
 * @brief MyApp::setSettingFile 写入配置文件
 * @param group                 组
 * @param key                   key
 * @param value                 值
 */
void MyApp::SetSettingFile(const QString &group, const QString &key, const QVariant &value)
{
    QSettings settings(m_strIniFile, QSettings::IniFormat);
    settings.beginGroup(group);
    settings.setValue(key, value);
    settings.sync();
}

/**
 * @brief MyApp::getSettingKeyValue 读取配置参数
 * @param group
 * @param key
 * @return
 */
QVariant MyApp::GetSettingKeyValue(const QString &group, const QString &key, const QVariant &value)
{
    QSettings settings(m_strIniFile, QSettings::IniFormat);
    settings.beginGroup(group);
    return settings.value(key, value);
}

/**
 * @brief MyApp::CheckDirs 检查目录
 */
void MyApp::CheckDirs()
{
    // 目录列表
    QStringList dirs = {
        m_strDataPath,
        m_strDatabasePath,
        m_strConfPath,
        m_strBackupPath,
        m_strRecvPath,
        m_strHeadPath
    };

    QDir dir;
    // 封装目录创建逻辑的匿名函数
    auto createDir = [&dir](const QString& dirPath) {
        if (!dir.exists(dirPath)) {
            bool createOk = dir.mkpath(dirPath);
            if (createOk) {
                qDebug() << "目录创建成功：" << dirPath;
            } else {
                qWarning() << "目录创建失败：" << dirPath;
            }
        }
    };

    // 遍历执行创建
    foreach (QString dirPath, dirs) {
        createDir(dirPath);
    }
}
// 保存配置
void MyApp::SaveConfig()
{
    QSettings settings(m_strIniFile, QSettings::IniFormat);

    /*系统设置*/
    settings.beginGroup("UserCfg");
    settings.setValue("User",   m_strUserName);
    settings.setValue("Passwd", m_strPassword);
    settings.endGroup();

    settings.sync();
}

/**
 * @brief MyApp::HeadFullPath
 * 头像文件名 -> 服务器头像目录下的完整路径（兼容历史数据里存的完整路径）
 */
QString MyApp::HeadFullPath(const QString &head)
{
    QString name = QFileInfo(head).fileName();
    if (name.isEmpty()) {
        name = QStringLiteral("default.png");
    }
    return m_strHeadPath + name;
}

/**
 * @brief MyApp::CreateDefaultHead
 * 用代码画一张默认头像（蓝底 + 白色人形），不依赖资源文件
 */
bool MyApp::CreateDefaultHead(const QString &filePath)
{
    QImage img(96, 96, QImage::Format_ARGB32);
    img.fill(Qt::transparent);

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);

    // 圆角蓝底
    painter.setBrush(QColor("#4A7ABB"));
    painter.drawRoundedRect(0, 0, 96, 96, 18, 18);

    // 头 + 肩膀
    painter.setBrush(QColor(255, 255, 255, 235));
    painter.drawEllipse(QPointF(48, 36), 15, 15);
    painter.drawRoundedRect(QRectF(23, 57, 50, 40), 18, 18);
    painter.end();

    QDir().mkpath(QFileInfo(filePath).absolutePath());
    return img.save(filePath, "PNG");
}

/**
 * @brief MyApp::EnsureDefaultHead
 * 服务器头像目录里必须有 default.png，缺失时自动生成一张
 */
QString MyApp::EnsureDefaultHead()
{
    const QString path = m_strHeadPath + QStringLiteral("default.png");
    if (!QFile::exists(path)) {
        if (CreateDefaultHead(path)) {
            qInfo() << "[头像] 已生成默认头像：" << path;
        } else {
            qWarning() << "[头像] 默认头像生成失败：" << path;
        }
    }
    return path;
}
