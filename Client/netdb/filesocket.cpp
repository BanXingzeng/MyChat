#include "filesocket.h"
#include <QFileInfo>
#include <QThread>
#include <QDir>
#include <QJsonDocument>
#include <QDebug>
#include "comapi/global.h"

// 大端 8 字节整数。与 QDataStream 默认字节序一致，
// 但不受"append 到 QByteArray 后 QDataStream 位置未同步"的影响。
static QByteArray be64(qint64 v)
{
    QByteArray b(8, 0);
    const quint64 u = static_cast<quint64>(v);
    for (int i = 0; i < 8; ++i)
        b[i] = static_cast<char>((u >> (56 - 8 * i)) & 0xFF);
    return b;
}

FileSocket::FileSocket(QObject *parent) : QObject(parent)
{
    // 延迟在工作线程中创建 Socket，避免线程亲和性问题
    m_tcpSocket = nullptr;
    // 创建发送文件对象
    m_fileToSend = new QFile(this);
    // 默认保存路径（可通过setSavePath修改）
    setSavePath(MyApp::m_strRecvPath);

    // 初始化进度计时器（仅连接一次，避免重复绑定）
    m_Time = new QTimer(this);
    m_Time->setInterval(100);  // 100ms节流，平衡实时性和性能
    m_Time->setSingleShot(false);

    // 计时器超时触发进度更新
    connect(m_Time, &QTimer::timeout, this, [=](){
        if (m_isSending) {
            // 发送文件：发射已发送字节、总字节
            emit progressUpdated(m_bytesWritten, m_totalBytes);
        } else if (m_isReceiving) {
            // 接收文件：发射已接收字节、总字节
            emit progressUpdated(static_cast<quint64>(m_recvProcessedBytes),
                                 static_cast<quint64>(m_recvTotalBytes));
        }
    });
}

FileSocket::~FileSocket()
{
    // 析构时停止传输并清理资源
    stopSendFile();
    if (m_tcpSocket)
    {
        m_tcpSocket->disconnectFromHost();
        m_tcpSocket->deleteLater();
    }
}

void FileSocket::connectToFileServer(const QString &host, quint16 port)
{
    // 正在发送时禁止连接
    if (m_isSending)
    {
        emit sendFailed("正在发送文件，无法连接服务器");
        return;
    }

    // 懒加载创建Socket（确保在工作线程中创建）
    if (!m_tcpSocket)
    {
        m_tcpSocket = new QTcpSocket(this);
        // 绑定Socket信号槽（均在工作线程执行）
        connect(m_tcpSocket, &QTcpSocket::bytesWritten, this, &FileSocket::onBytesWritten);
        connect(m_tcpSocket, &QTcpSocket::readyRead, this, &FileSocket::onReadyRead);
        connect(m_tcpSocket, &QTcpSocket::connected, this, &FileSocket::onSocketConnected);
        connect(m_tcpSocket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
                this, &FileSocket::onSocketError);
        connect(m_tcpSocket, &QTcpSocket::disconnected, this, &FileSocket::stopSendFile);
    }

    // 根据Socket状态处理连接
    if (m_tcpSocket->state() == QAbstractSocket::UnconnectedState)
    {
        m_tcpSocket->connectToHost(host, port);
        qDebug() << "子线程发起文件服务器连接：" << QThread::currentThreadId() << host << port;
    }
    else if (m_tcpSocket->state() == QAbstractSocket::ConnectingState)
    {
        emit sendFailed("正在连接文件服务器，请等待");
    }
    else
    {
        emit fileServerConnected(); // 已连接，直接触发信号
    }
}

void FileSocket::setFilePath(const QString &filePath)
{
    m_filePath = filePath;
}

void FileSocket::setLoadSize(quint64 loadSize)
{
    m_loadSize = loadSize;
}

void FileSocket::setUserId(const QString &userId)
{
    m_userId = userId;
}

void FileSocket::setSavePath(const QString &saveDir)
{
    m_saveDir = saveDir;
}

void FileSocket::setEncryptSession(const QByteArray &key, const QByteArray &iv,
                                   const QByteArray &metaJson)
{
    m_sendKey  = key;
    m_sendIv   = iv;
    m_sendMeta = metaJson;
}

void FileSocket::setDecryptIdentity(int userId, const QString &privPem)
{
    m_decryptUid  = userId;
    m_decryptPriv = privPem;
}

bool FileSocket::hasEncryptSession() const
{
    return m_sendKey.size() == 32 && m_sendIv.size() == 16 && !m_sendMeta.isEmpty();
}

void FileSocket::onSocketConnected()
{
    qDebug() << "文件服务器连接成功（子线程）：" << QThread::currentThreadId();
    emit fileServerConnected();
}

void FileSocket::onSocketError(QAbstractSocket::SocketError err)
{
    QString errMsg = QString("Socket错误：%1（错误码：%2）").arg(m_tcpSocket->errorString()).arg(err);
    qDebug() << errMsg;

    // 仅在传输中时触发失败信号，非传输期的错误忽略
    if (m_isSending || m_isReceiving)
    {
        emit sendFailed(errMsg);
    }
    else
    {
        qDebug() << "忽略非活动传输期间的Socket错误：" << errMsg;
    }
    m_Time->stop(); // 停止计时器
}

void FileSocket::startSendFile()
{
    // 状态校验
    if (m_isSending)
    {
        emit sendFailed("正在发送文件，拒绝重复启动");
        return;
    }
    if (!m_tcpSocket || m_tcpSocket->state() != QAbstractSocket::ConnectedState)
    {
        emit sendFailed("未连接文件服务器，无法发送文件");
        return;
    }
    if (m_filePath.isEmpty())
    {
        emit sendFailed("待发送文件路径为空");
        return;
    }

    // 重置发送状态
    resetSendState();
    m_isSending = true;
    m_isStopped = false;

    // 打开文件（只读模式）
    m_fileToSend->setFileName(m_filePath);
    if (!m_fileToSend->open(QFile::ReadOnly))
    {
        emit sendFailed(QString("文件打开失败：%1").arg(m_fileToSend->errorString()));
        m_isSending = false;
        return;
    }

    const qint64 fileSize = m_fileToSend->size();
    QFileInfo fileInfo(m_filePath);
    const QByteArray nameUtf8 = fileInfo.fileName().toUtf8();

    // 包头（大端）：
    //   totalBytes(8) | filenameLen(8) | filename | metaLen(8) | meta
    // meta 里是用接收者公钥包裹过的一次性会话密钥；服务端原样落盘，解不开。
    m_outBlock.clear();
    m_outBlock.append(be64(0));                          // 占位，稍后回写
    m_outBlock.append(be64(nameUtf8.size()));
    m_outBlock.append(nameUtf8);
    m_outBlock.append(be64(m_sendMeta.size()));
    m_outBlock.append(m_sendMeta);

    const qint64 headerSize = m_outBlock.size();
    m_totalBytes = static_cast<quint64>(fileSize) + static_cast<quint64>(headerSize);
    m_outBlock.replace(0, 8, be64(static_cast<qint64>(m_totalBytes)));

    // 打开加密流：CTR 密文与明文等长，进度和总字节数都不受影响
    if (!m_sendMeta.isEmpty())
    {
        if (!m_stream.initEncrypt(m_sendKey, m_sendIv))
        {
            emit sendFailed(QString("初始化文件加密失败：%1").arg(CryptoUtil::lastError()));
            m_fileToSend->close();
            m_isSending = false;
            return;
        }
    }

    // 初始化发送计数
    m_bytesWritten = 0;

    // 发送包头
    qint64 sendBytes = m_tcpSocket->write(m_outBlock);
    if (sendBytes == -1)
    {
        emit sendFailed("文件包头发送失败");
        m_fileToSend->close();
        m_isSending = false;
        return;
    }

    // 计算待发送的文件数据字节数（总字节 - 已发送的包头字节）
    m_bytesToWrite = m_totalBytes - sendBytes;
    m_outBlock.clear();

    // 启动进度计时器（仅启动，无需重复连接）
    m_Time->start(100);

    qDebug() << "开始发送文件：" << fileInfo.fileName()
             << "原始大小：" << fileSize
             << "加密：" << (m_sendMeta.isEmpty() ? "否" : "是（AES-256-CTR）")
             << "总字节数（含包头）：" << m_totalBytes;
}

void FileSocket::stopSendFile()
{
    // 标记停止状态
    m_isStopped = true;
    m_isSending = false;
    m_isReceiving = false;

    // 关闭打开的文件
    if (m_fileToSend && m_fileToSend->isOpen())
    {
        m_fileToSend->close();
    }
    if (m_fileToRecv && m_fileToRecv->isOpen())
    {
        m_fileToRecv->close();
    }

    // 清空缓冲区、停止计时器
    m_outBlock.clear();
    m_recvBuffer.clear();
    m_Time->stop();
}

void FileSocket::onBytesWritten(qint64 numBytes)
{
    // 已停止或未发送状态，直接返回
    if (m_isStopped || !m_isSending)
        return;

    // 更新已发送字节数
    m_bytesWritten += static_cast<quint64>(numBytes);

    // 还有待发送数据时，继续读取文件并发送
    if (m_bytesToWrite > 0)
    {
        // 计算本次读取的字节数
        qint64 readSize = static_cast<qint64>(qMin(m_bytesToWrite, m_loadSize));
        m_outBlock = m_fileToSend->read(readSize);

        // 读取失败
        if (m_outBlock.isEmpty() && !m_fileToSend->atEnd())
        {
            emit sendFailed("从文件读取数据失败");
            stopSendFile();
            return;
        }

        // 端到端加密：CTR 是流密码，逐块加密且长度不变
        if (m_stream.isActive() && !m_outBlock.isEmpty())
        {
            const QByteArray cipher = m_stream.update(m_outBlock);
            if (cipher.size() != m_outBlock.size())
            {
                emit sendFailed("文件加密异常：密文长度与明文不一致，传输中止");
                stopSendFile();
                return;
            }
            m_outBlock = cipher;
        }

        // 写入Socket发送队列
        qint64 writeBytes = m_tcpSocket->write(m_outBlock);
        if (writeBytes < 0)
        {
            emit sendFailed("文件数据写入Socket失败");
            stopSendFile();
            return;
        }

        // 更新待发送字节数
        m_bytesToWrite -= static_cast<quint64>(writeBytes);
        m_outBlock.clear();
    }

    if (m_bytesWritten >= m_totalBytes)
    {
        // 清理资源
        if (m_fileToSend->isOpen())
            m_fileToSend->close();
        m_Time->stop(); // 停止计时器
        emit sendFinished(); // 触发发送完成信号
        m_isSending = false;
        resetSendState(); // 重置状态
        qDebug() << "文件发送完成，总发送字节数：" << m_bytesWritten;
    }
}

void FileSocket::onReadyRead()
{
    // 读取Socket数据到接收缓冲区
    m_recvBuffer += m_tcpSocket->readAll();

    // 解析包头（未开始接收时）
    if (!m_isReceiving)
    {
        const int kBase = static_cast<int>(sizeof(qint64) * 2);   // totalBytes + filenameLen

        // 包头不完整（至少需要8+8=16字节），等待后续数据
        if (m_recvBuffer.size() < kBase)
            return;

        QDataStream in(&m_recvBuffer, QIODevice::ReadOnly);
        in.setVersion(QDataStream::Qt_5_12);
        in >> m_recvTotalBytes; // 总字节数
        qint64 filenameLen = 0;
        in >> filenameLen;

        if (filenameLen <= 0 || filenameLen > 8192)
        {
            emit sendFailed("文件包头异常：文件名长度非法");
            stopSendFile();
            return;
        }

        // 文件名之后是 metaLen：>0 表示内容经过端到端加密，0 表示明文
        const int nameEnd = kBase + static_cast<int>(filenameLen);
        if (m_recvBuffer.size() < nameEnd + static_cast<int>(sizeof(qint64)))
            return;

        QDataStream inMeta(&m_recvBuffer, QIODevice::ReadOnly);
        inMeta.setVersion(QDataStream::Qt_5_12);
        inMeta.skipRawData(nameEnd);
        qint64 metaLen = 0;
        inMeta >> metaLen;

        if (metaLen < 0 || metaLen > 64 * 1024)
        {
            emit sendFailed("文件包头异常：加密头长度非法");
            stopSendFile();
            return;
        }

        const int contentPos = nameEnd + static_cast<int>(sizeof(qint64)) + static_cast<int>(metaLen);
        if (m_recvBuffer.size() < contentPos)
            return;

        const QByteArray nameBytes = m_recvBuffer.mid(kBase, static_cast<int>(filenameLen));
        const QByteArray metaBytes = m_recvBuffer.mid(nameEnd + static_cast<int>(sizeof(qint64)),
                                                      static_cast<int>(metaLen));
        QString fileName = QString::fromUtf8(nameBytes);
        if (fileName.isEmpty())
        {
            emit sendFailed("解析到空的文件名，接收文件失败");
            stopSendFile();
            return;
        }

        // 有加密头就必须先解出属于自己的会话密钥，否则后面全是乱码
        if (!metaBytes.isEmpty() && !initRecvCrypto(metaBytes))
        {
            stopSendFile();
            return;
        }

        // 处理保存路径（避免文件重名）
        QString savePath = m_saveDir;
        if (savePath.isEmpty())
            savePath = QDir::homePath() + "/";
        // 确保保存目录存在
        QDir saveDir(savePath);
        if (!saveDir.exists())
            saveDir.mkpath(savePath);

        QString finalPath = savePath + fileName;
        // 重名处理：自动加后缀 (1)/(2)
        QFileInfo fi(finalPath);
        QString baseName = fi.baseName();
        QString suffix = fi.suffix();
        int idx = 1;
        while (QFile::exists(finalPath))
        {
            if (suffix.isEmpty())
                finalPath = savePath + baseName + QString("(%1)").arg(idx);
            else
                finalPath = savePath + baseName + QString("(%1).%2").arg(idx).arg(suffix);
            idx++;
        }

        // 创建接收文件对象
        m_fileToRecv = new QFile(finalPath, this);
        if (!m_fileToRecv->open(QFile::WriteOnly))
        {
            emit sendFailed(QString("无法创建接收文件：%1").arg(m_fileToRecv->errorString()));
            delete m_fileToRecv;
            m_fileToRecv = nullptr;
            return;
        }

        // 初始化接收状态
        m_isReceiving = true;
        // 已处理字节数 = 包头 + 加密头
        m_recvProcessedBytes = contentPos;
        // 移除缓冲区中的包头数据，仅保留文件内容（密文）
        m_recvBuffer = m_recvBuffer.mid(contentPos);
        // 启动进度计时器
        m_Time->start(100);

        qDebug() << "开始接收文件：" << fileName
                 << "总字节数：" << m_recvTotalBytes
                 << "加密：" << (metaBytes.isEmpty() ? "否" : "是（AES-256-CTR）")
                 << "保存路径：" << finalPath;
    }

    // 写入文件内容（已解析包头后）
    if (m_isReceiving && m_fileToRecv)
    {
        // 解密这一批数据（CTR 密文与明文等长，逐块解密）
        QByteArray plain = m_recvBuffer;
        if (m_stream.isActive() && !m_recvBuffer.isEmpty())
        {
            plain = m_stream.update(m_recvBuffer);
            if (plain.isEmpty())
            {
                emit sendFailed(QString("文件解密失败，接收中止：%1").arg(CryptoUtil::lastError()));
                m_fileToRecv->close();
                m_fileToRecv->deleteLater();
                m_fileToRecv = nullptr;
                m_recvBuffer.clear();
                m_isReceiving = false;
                m_Time->stop();
                return;
            }
        }
        m_recvBuffer.clear();

        // 将明文写入文件
        qint64 writeLen = m_fileToRecv->write(plain);
        if (writeLen < 0)
        {
            emit sendFailed(QString("接收文件写入失败：%1").arg(m_fileToRecv->errorString()));
            // 清理资源
            m_fileToRecv->close();
            m_fileToRecv->deleteLater();
            m_fileToRecv = nullptr;
            m_isReceiving = false;
            m_Time->stop();
            return;
        }

        // 更新已处理字节数
        m_recvProcessedBytes += writeLen;

        // 接收完成判断
        if (m_recvProcessedBytes >= m_recvTotalBytes)
        {
            // CTR 收尾（正常情况下无残余，这里保证上下文被释放）
            m_stream.finish();

            QString savePath = m_fileToRecv->fileName();
            // 清理资源
            m_fileToRecv->close();
            m_fileToRecv->deleteLater();
            m_fileToRecv = nullptr;
            m_isReceiving = false;
            m_Time->stop(); // 停止计时器

            emit recvFinished(savePath); // 触发接收完成信号
            resetSendState(); // 重置状态
            qDebug() << "文件接收完成，保存路径：" << savePath;
        }
    }
}

/**
 * @brief 用文件头里的 meta 解出本机的会话密钥
 * meta 结构：{"v":1,"alg":"AES-256-CTR","eks":{"<uid>":"<base64 RSA-OAEP 包裹>"}}
 * 每个接收者各有一份用自己公钥包裹的 key||iv，服务端解不开，只有对应私钥能取到。
 */
bool FileSocket::initRecvCrypto(const QByteArray &metaBytes)
{
    if (m_decryptPriv.isEmpty() || m_decryptUid <= 0)
    {
        emit sendFailed("该文件已端到端加密，但本机密钥未就绪，无法解密");
        return false;
    }

    const QJsonObject meta = QJsonDocument::fromJson(metaBytes).object();
    const QJsonObject eks = meta.value("eks").toObject();
    const QString b64 = eks.value(QString::number(m_decryptUid)).toString();
    if (b64.isEmpty())
    {
        emit sendFailed("该文件的会话密钥不包含本机份额，无法解密（可能不是发给你的文件）");
        return false;
    }

    QByteArray keyIv;
    if (!CryptoUtil::unwrap(m_decryptPriv, QByteArray::fromBase64(b64.toLatin1()), &keyIv)
        || keyIv.size() != 48)
    {
        emit sendFailed(QString("文件会话密钥解封失败：%1").arg(CryptoUtil::lastError()));
        return false;
    }

    if (!m_stream.initDecrypt(keyIv.left(32), keyIv.mid(32, 16)))
    {
        emit sendFailed(QString("初始化文件解密失败：%1").arg(CryptoUtil::lastError()));
        return false;
    }

    qDebug() << "[加密] 已解出文件会话密钥，开始流式解密";
    return true;
}

void FileSocket::requestFile(const QString &remoteFileName)
{
    // 校验连接状态
    if (!m_tcpSocket || m_tcpSocket->state() != QAbstractSocket::ConnectedState)
    {
        emit sendFailed("未连接文件服务器，无法请求下载文件");
        return;
    }

    // 构造下载请求JSON
    QJsonObject reqObj;
    reqObj.insert("action", "getfile");
    reqObj.insert("file", remoteFileName);
    reqObj.insert("user", m_userId); // 可选：携带用户ID
    QJsonDocument doc(reqObj);
    QByteArray reqData = doc.toJson(QJsonDocument::Compact);

    // 发送下载请求
    m_tcpSocket->write(reqData);
    qDebug() << "向文件服务器发送下载请求：" << remoteFileName;
}

void FileSocket::resetSendState()
{
    // 重置所有计数和缓冲区
    m_totalBytes = 0;
    m_bytesWritten = 0;
    m_bytesToWrite = 0;
    m_outBlock.clear();
    m_recvTotalBytes = 0;
    m_recvProcessedBytes = 0;
    m_recvBuffer.clear();

    // 释放加解密上下文（CTR 收尾无残余数据，这里只为释放资源）
    m_stream.finish();

    // 重置状态标记
    m_isSending = false;
    m_isReceiving = false;
    m_isStopped = false;
}
