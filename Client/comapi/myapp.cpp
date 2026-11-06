#include "myapp.h"
#include <QDesktopWidget>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QDebug>
#include <QSettings>
#include <QFileInfo>
#include <QByteArray>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <wincrypt.h>
#endif

// ------------------------------------------------------------------
// 本地口令保护（DPAPI）
// config.ini 里的口令不能是明文：这里用 Windows 的 CryptProtectData 加密，
// 密文与"当前 Windows 用户 + 本机"绑定，拷到别的机器/别的账户下解不开。
// 注意：这是本地存储保护，和端到端加密是两件事，不参与消息链路的密钥体系。
// ------------------------------------------------------------------
static QString protectPassword(const QString &plain)
{
    if (plain.isEmpty()) return QString();
#ifdef Q_OS_WIN
    const QByteArray raw = plain.toUtf8();
    DATA_BLOB in;
    DATA_BLOB out;
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(raw.constData()));
    in.cbData = static_cast<DWORD>(raw.size());
    out.pbData = nullptr;
    out.cbData = 0;

    if (!CryptProtectData(&in, L"MyChat login password", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        qWarning() << "[配置] 口令加密失败，本次不保存密码";
        return QString();
    }
    const QByteArray blob(reinterpret_cast<const char *>(out.pbData), static_cast<int>(out.cbData));
    LocalFree(out.pbData);
    return QString("enc:") + QString::fromLatin1(blob.toBase64());
#else
    return plain;
#endif
}

static QString unprotectPassword(const QString &stored)
{
    if (stored.isEmpty()) return QString();
    // 老配置里是明文（没有 enc: 前缀），原样返回，登录一次后会自动升级成密文
    if (!stored.startsWith("enc:")) return stored;
#ifdef Q_OS_WIN
    const QByteArray blob = QByteArray::fromBase64(stored.mid(4).toLatin1());
    DATA_BLOB in;
    DATA_BLOB out;
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(blob.constData()));
    in.cbData = static_cast<DWORD>(blob.size());
    out.pbData = nullptr;
    out.cbData = 0;

    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        qWarning() << "[配置] 口令解密失败（换了机器或账户），需要重新输入密码";
        return QString();
    }
    const QString plain = QString::fromUtf8(reinterpret_cast<const char *>(out.pbData),
                                            static_cast<int>(out.cbData));
    LocalFree(out.pbData);
    return plain;
#else
    return QString();
#endif
}

// 应用程序配置目录
QString MyApp::m_strAppPath         = "./";
QString MyApp::m_strDataPath        = "";
QString MyApp::m_strRecvPath        = "";
QString MyApp::m_strDatabasePath    = "";
QString MyApp::m_strConfPath        = "";
QString MyApp::m_strFacePath        = "";
QString MyApp::m_strHeadPath        = "";
QString MyApp::m_strSoundPath       = "";
QString MyApp::m_strRecordPath      = "";

// 配置文件
QString MyApp::m_strIniFile         = "config.ini";

// 服务器相关配置
// 出厂默认连公网服务器；config.ini 里的 [Server] ServerIP 优先于它
QString MyApp::m_strHostAddr        = "8.141.105.64";
quint16     MyApp::m_nMsgPort           = 60101;
quint16     MyApp::m_nFilePort          = 60102;
quint16     MyApp::m_nGroupPort         = 60103;

QString MyApp::m_strUserName        = "";
QString MyApp::m_strPassword        = "";   // 不内置默认密码，一律由用户输入
bool    MyApp::m_bRememberPwd       = true; // 与登录页"记住密码"的默认勾选状态一致
QString MyApp::m_strHeadFile        = "default.png";

int     MyApp::m_nId                = -1;
int     MyApp::m_nWinX              = 0;
int     MyApp::m_nWinY              = 0;

void MyApp::InitApp(const QString &appPath)
{
    m_strAppPath        = appPath + "/";

    m_strDataPath       = m_strAppPath  + "Data/";
    m_strRecvPath       = m_strDataPath + "RecvFiles/";
    m_strDatabasePath   = m_strDataPath + "Database/";
    m_strConfPath       = m_strDataPath + "Conf/";
    m_strHeadPath       = m_strDataPath + "Head/";
    m_strSoundPath      = m_strDataPath + "Sound/";
    m_strRecordPath     = m_strDataPath + "Record/";
    m_strFacePath       = m_strDataPath + "Face/";
    m_strIniFile        = m_strConfPath + "config.ini";

    // 检查目录
    CheckDirs();

    // 检测音频文件
    CheckSound();

    // 创建配置文件
    CreatorSettingFile();

    // 加载系统配置
    ReadSettingFile();

    setDefaultHead();
}

void MyApp::CreatorSettingFile()
{
    // 首次运行（或配置文件损坏）时写入一份默认配置
    QSettings settings(m_strIniFile, QSettings::IniFormat);
    if (!QFile::exists(m_strIniFile) || settings.childGroups().isEmpty()) {
        SaveConfig();
    }
}

void MyApp::ReadSettingFile()
{
    QSettings settings(m_strIniFile, QSettings::IniFormat);

    settings.beginGroup("Server");
    const QString ip = settings.value("ServerIP", m_strHostAddr).toString().trimmed();
    if (!ip.isEmpty()) {
        m_strHostAddr = ip;
    }
    const uint msgPort  = settings.value("MsgPort",   m_nMsgPort).toUInt();
    const uint filePort = settings.value("FilePort",  m_nFilePort).toUInt();
    const uint groupPort = settings.value("GroupPort", m_nGroupPort).toUInt();
    settings.endGroup();

    // 端口非法时保留代码里的默认值，避免读到 0 导致永远连不上
    if (msgPort > 0 && msgPort <= 65535)     m_nMsgPort   = static_cast<quint16>(msgPort);
    if (filePort > 0 && filePort <= 65535)   m_nFilePort  = static_cast<quint16>(filePort);
    if (groupPort > 0 && groupPort <= 65535) m_nGroupPort = static_cast<quint16>(groupPort);

    settings.beginGroup("UserConfig");
    m_strUserName  = settings.value("userName", m_strUserName).toString();
    m_bRememberPwd = settings.value("rememberPwd", true).toBool();
    // 只在这一项存在时才覆盖内存值：没记住密码时留着空串，重连会走"需要重新登录"
    m_strPassword  = unprotectPassword(settings.value("password", QString()).toString());
    settings.endGroup();
}

void MyApp::SetSettingFile(const QString &group, const QString &key, const QVariant &value)
{
    QSettings settings(m_strIniFile, QSettings::IniFormat);
    settings.setValue(group + "/" + key, value);
    settings.sync();
}

QVariant MyApp::GetSettingKeyValue(const QString &group, const QString &key, const QVariant &value)
{
    QSettings settings(m_strIniFile, QSettings::IniFormat);
    return settings.value(group + "/" + key, value);
}

/**
 * @brief MyApp::CheckDirs
 * 检查文件是否存在
 */
void MyApp::CheckDirs()
{
    auto createDir = [](const QString& path) {
        QDir dir(path);
        // mkpath() 自动创建多级目录，返回bool表示是否成功
        if (!dir.exists() && !dir.mkpath(".")) {
            qWarning() << "创建目录失败：" << path;
        }
    };

    // 依次检查创建各目录
    createDir(m_strDataPath);
    createDir(m_strRecvPath);
    createDir(m_strDatabasePath);
    createDir(m_strConfPath);
    createDir(m_strFacePath);
    createDir(m_strHeadPath);
    createDir(m_strSoundPath);
    createDir(m_strRecordPath);
    // 图片消息会落盘到 Data/Images（见 global.h SaveInlineImage），提前建好
    createDir(m_strDataPath + "Images/");
}

void MyApp::CheckSound()
{
    if(!QFile::exists(MyApp::m_strSoundPath + "message.wav")){
        QFile::copy(":/sound/resource/sound/message.wav", MyApp::m_strSoundPath + "message.wav");
    }
    if(!QFile::exists(MyApp::m_strSoundPath + "msg.wav")){
        QFile::copy(":/sound/resource/sound/msg.wav", MyApp::m_strSoundPath + "msg.wav");
    }
    if(!QFile::exists(MyApp::m_strSoundPath + "ringin.wav")){
        QFile::copy(":/sound/resource/sound/ringin.wav", MyApp::m_strSoundPath + "ringin.wav");
    }
    if(!QFile::exists(MyApp::m_strSoundPath + "system.wav")){
        QFile::copy(":/sound/resource/sound/system.wav", MyApp::m_strSoundPath + "system.wav");
    }
    if(!QFile::exists(MyApp::m_strSoundPath + "userlogon.wav")){
        QFile::copy(":/sound/resource/sound/userlogon.wav", MyApp::m_strSoundPath + "userlogon.wav");
    }

}

void MyApp::SaveConfig()
{
    QSettings settings(m_strIniFile,QSettings::IniFormat);

    // 保存用户信息
    // 注意：口令不在这里写，统一走 SavePassword()，否则"没勾记住密码"
    // 也会被任何一次 SaveConfig（改头像、改窗口位置…）重新写回磁盘。
    settings.beginGroup("UserConfig");
    settings.setValue("userName",m_strUserName);
    settings.setValue("rememberPwd",m_bRememberPwd);
    settings.endGroup();

    //保存服务器信息
    settings.beginGroup("Server");
    settings.setValue("ServerIP", m_strHostAddr);
    settings.setValue("MsgPort",  m_nMsgPort);
    settings.setValue("FilePort",  m_nFilePort);
    settings.setValue("GroupPort",  m_nGroupPort);
    settings.endGroup();
    settings.sync();
}

/**
 * @brief MyApp::SavePassword
 * 按"记住密码"的开关决定口令是落盘还是删掉；落盘时先经 DPAPI 加密。
 */
void MyApp::SavePassword(const QString &password)
{
    QSettings settings(m_strIniFile, QSettings::IniFormat);
    settings.beginGroup("UserConfig");
    if (!m_bRememberPwd || password.isEmpty()) {
        settings.remove("password");
    } else {
        const QString stored = protectPassword(password);
        if (stored.isEmpty()) settings.remove("password");   // 加密失败就别退化成明文
        else                  settings.setValue("password", stored);
    }
    settings.endGroup();
    settings.sync();
}

void MyApp::setDefaultHead()
{
    QString localFullPath = MyApp::m_strHeadPath + "default.png";
    if (QFile::exists(localFullPath)) {
        return;
    }
    QString resPath(":/resource/head/head-64.png");
    if (!QFile::copy(resPath, localFullPath)) {
        qWarning() << "复制失败：" << resPath << "→" << localFullPath;
    } else {
        qDebug() << "复制成功：" << resPath << "→" << localFullPath;
    }
}

/**
 * @brief MyApp::HeadFullPath
 * 头像文件名 -> 本地完整路径。
 * 兼容历史数据：早期版本在数据库里存的是完整路径（如 ./Data/Head/x.png），
 * 这种也统一按文件名还原，避免出现 "路径+路径" 的拼接错误。
 */
QString MyApp::HeadFullPath(const QString &head)
{
    QString name = QFileInfo(head).fileName();
    if (name.isEmpty()) {
        name = QStringLiteral("default.png");
    }
    return m_strHeadPath + name;
}
