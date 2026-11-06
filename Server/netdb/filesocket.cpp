#include "filesocket.h"
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QHostAddress>
#include <QThread>
#include "comapi/myapp.h"
#include <QJsonDocument>
#include <QJsonObject>

FileSocket::FileSocket(QTcpSocket *socket, QObject *parent)
    : QObject(parent), QRunnable()
{
    setAutoDelete(true);

    if (socket)
    {
        setTcpSocket(socket);
    }

    m_saveDir = MyApp::m_strRecvPath;
    QDir saveDir(m_saveDir);
    if (!saveDir.exists())
    {
        saveDir.mkpath(".");
        qDebug() << "创建文件保存目录：" << m_saveDir;
    }
}

FileSocket::~FileSocket()
{
    qDebug() << "FileSocket销毁：" << m_clientKey;
}

void FileSocket::setTcpSocket(QTcpSocket *socket)
{
    QMutexLocker locker(&m_mutex);
    // 清理旧Socket
    if (m_tcpSocket)
    {
        disconnect(m_tcpSocket, nullptr, this, nullptr); // 解绑所有信号
        m_tcpSocket->deleteLater();
    }

    // 设置新Socket
    m_tcpSocket = socket;
    if (m_tcpSocket)
    {
        // 不在这里设置父对象或连接信号，避免跨线程问题。
        // socket 与本对象将在 run() 中移动到工作线程后再建立连接和设置选项。
        // 生成客户端唯一标识
        m_clientKey = QString("%1:%2")
                          .arg(m_tcpSocket->peerAddress().toString())
                          .arg(m_tcpSocket->peerPort());
    }
}

void FileSocket::setSaveDir(const QString &dir)
{
    QMutexLocker locker(&m_mutex);
    if (!dir.isEmpty())
    {
        m_saveDir = dir;
    }
    else
    {
        m_saveDir = MyApp::m_strRecvPath;
    }
    QDir saveDir(m_saveDir);
    if (!saveDir.exists())
        saveDir.mkpath(".");
}

void FileSocket::run()
{
    QMutexLocker locker(&m_mutex);
    m_isRunning = true;
    // 运行于线程池线程 —— 确保在该线程内创建或绑定 QTcpSocket，避免 QSocketNotifier 在主线程创建
    QThread *workerThread = QThread::currentThread();
    // 不要将 this 对象再次移动到线程（可能已在其他线程），仅确保底层 socket 在当前工作线程中

    // 如果用户在 incomingConnection 中传入了 socket 描述符，延迟在这里创建 QTcpSocket
    if (!m_tcpSocket && m_socketDescriptor >= 0)
    {
        m_tcpSocket = new QTcpSocket();
        if (!m_tcpSocket->setSocketDescriptor(m_socketDescriptor))
        {
            emit taskFailed(m_clientKey, QString("绑定socket描述符失败：%1").arg(m_tcpSocket->errorString()));
            m_tcpSocket->deleteLater();
            m_tcpSocket = nullptr;
            return;
        }
        // 更新客户端标识
        m_clientKey = QString("%1:%2")
                          .arg(m_tcpSocket->peerAddress().toString())
                          .arg(m_tcpSocket->peerPort());
    }

    if (!m_tcpSocket)
    {
        emit taskFailed(m_clientKey, "未提供有效的 QTcpSocket 或 描述符，任务终止");
        return;
    }

    // 如果 socket 是在其他线程创建的，移动它到当前工作线程以便后续使用
    if (m_tcpSocket->thread() != workerThread)
    {
        // 仅当 socket 没有父对象或其父对象也位于目标线程时，才移动它
        if (m_tcpSocket->parent() == nullptr || m_tcpSocket->parent()->thread() == workerThread)
        {
            m_tcpSocket->moveToThread(workerThread);
        }
        else
        {
            qDebug() << "Warning: socket has parent in another thread; skipping moveToThread";
        }
    }

    // 检查 socket 是否已连接
    if (m_tcpSocket->state() != QAbstractSocket::ConnectedState)
    {
        emit taskFailed(m_clientKey, "Socket未连接，任务终止");
        return;
    }

    // 在工作线程中建立直接连接
    connect(m_tcpSocket, &QTcpSocket::readyRead, this, &FileSocket::onReadyRead, Qt::DirectConnection);
    connect(m_tcpSocket, &QTcpSocket::disconnected, this, &FileSocket::onSocketDisconnected, Qt::DirectConnection);
    connect(m_tcpSocket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            this, &FileSocket::onSocketError, Qt::DirectConnection);

    // 设置 Socket 参数
    m_tcpSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_tcpSocket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4 * 1024);

    m_eventLoop = new QEventLoop();
    locker.unlock();

    qDebug() << "启动FileSocket任务：" << m_clientKey
             << "任务类型：接收文件"
             << "线程ID：" << QThread::currentThreadId();

    // 启动文件接收逻辑
    startRecvFile();

    // 运行事件循环
    m_eventLoop->exec();

    QMutexLocker locker2(&m_mutex);
    if (m_fileHandler)
    {
        if (m_fileHandler->isOpen())
            m_fileHandler->close();
        m_fileHandler->deleteLater();
        m_fileHandler = nullptr;
    }
    m_dataBuffer.clear();
    m_totalBytes = 0;
    m_processedBytes = 0;
    m_isRunning = false;

    // 销毁事件循环
    m_eventLoop->deleteLater();
    m_eventLoop = nullptr;

    // 清理 socket（在 socket 所在线程安全地删除）
    if (m_tcpSocket)
    {
        m_tcpSocket->disconnect();
        m_tcpSocket->close();
        m_tcpSocket->deleteLater();
        m_tcpSocket = nullptr;
    }

    qDebug() << "FileSocket任务结束：" << m_clientKey;
}

// ========== 接收文件 ==========
void FileSocket::startRecvFile()
{
    QMutexLocker locker(&m_mutex);
    if (!m_isRunning || !m_tcpSocket)
        return;

    // 重置接收状态
    m_dataBuffer.clear();
    m_totalBytes = 0;
    m_processedBytes = 0;
    m_finalRecvPath.clear();

    qDebug() << "开始接收文件：" << m_clientKey;
}

void FileSocket::stopRecvFile()
{
    QMutexLocker locker(&m_mutex);
    m_isRunning = false;
    if (m_fileHandler && m_fileHandler->isOpen())
    {
        m_fileHandler->close();
    }
    m_dataBuffer.clear();
    if (m_eventLoop)
        m_eventLoop->quit(); // 退出事件循环
}

void FileSocket::onReadyRead()
{
    QMutexLocker locker(&m_mutex);
    if (!m_isRunning || !m_tcpSocket)
        return;

    // 读取数据到缓冲区
    m_dataBuffer += m_tcpSocket->readAll();
    if (m_dataBuffer.isEmpty())
        return;

    // 尝试识别是否为下载请求（JSON），例如: {"action":"getfile","file":"name"}
    if (m_totalBytes == 0 && m_dataBuffer.size() < 4096 && m_dataBuffer.startsWith('{'))
    {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(m_dataBuffer, &err);
        if (!doc.isNull() && err.error == QJsonParseError::NoError && doc.isObject())
        {
            QJsonObject obj = doc.object();
            if (obj.value("action").toString() == QLatin1String("getfile"))
            {
                QString reqFile = obj.value("file").toString();
                if (reqFile.isEmpty())
                {
                    emit taskFailed(m_clientKey, "下载请求未包含文件名");
                    return;
                }

                QString fullPath = QDir(m_saveDir).filePath(reqFile);
                if (!QFile::exists(fullPath))
                {
                    emit taskFailed(m_clientKey, QString("请求文件不存在：%1").arg(fullPath));
                    return;
                }

                QFile sendFile(fullPath);
                if (!sendFile.open(QFile::ReadOnly))
                {
                    emit taskFailed(m_clientKey, QString("打开文件失败：%1").arg(sendFile.errorString()));
                    return;
                }

                // 构造并发送包头 + 文件数据（同步写入）
                QByteArray outBlock;
                QDataStream sendOut(&outBlock, QIODevice::WriteOnly);
                sendOut.setVersion(QDataStream::Qt_5_12);
                QByteArray nameUtf8 = reqFile.toUtf8();
                // 写两个 qint64 占位，然后 append 原始 UTF-8 文件名字节
                sendOut << qint64(0) << qint64(0);
                outBlock.append(nameUtf8);
                qint64 headerSize = outBlock.size();
                qint64 total = headerSize + sendFile.size();
                sendOut.device()->seek(0);
                sendOut << total;
                sendOut << qint64(nameUtf8.size());

                qint64 written = m_tcpSocket->write(outBlock);
                if (written == -1)
                {
                    emit taskFailed(m_clientKey, "发送文件头失败");
                    sendFile.close();
                    return;
                }

                // 发送文件内容 分片发送，避免一次性占用过多内存
                const qint64 chunk = 4 * 1024;
                while (!sendFile.atEnd())
                {
                    QByteArray chunkData = sendFile.read(chunk);
                    if (chunkData.isEmpty())
                        break;
                    qint64 w = m_tcpSocket->write(chunkData);
                    if (w < 0)
                    {
                        emit taskFailed(m_clientKey, "发送文件内容失败");
                        sendFile.close();
                        return;
                    }
                    if (!m_tcpSocket->waitForBytesWritten(30000))
                    {
                        emit taskFailed(m_clientKey, "发送超时或连接中断");
                        sendFile.close();
                        return;
                    }
                }

                sendFile.close();
                emit recvFinished(m_clientKey, fullPath);
                return;
            }
        }
        // 如果不是 JSON 请求，继续按文件接收协议处理
    }

    QDataStream in(&m_dataBuffer, QIODevice::ReadOnly);
    in.setVersion(QDataStream::Qt_5_12);

    // 解析包头
    if (m_totalBytes == 0)
    {
        // 包头至少需要 8字节（总字节数） + 8字节（文件名长度） = 16字节
        if (m_dataBuffer.size() < sizeof(qint64) * 2)
        {
            qDebug() << "包头未接收完整，当前缓冲区大小：" << m_dataBuffer.size();
            return;
        }

        // 解析总字节数和文件名长度
        in >> m_totalBytes; // 客户端发送的总字节数（
        qint64 fileNameLen = 0;
        in >> fileNameLen; // 文件名长度（按 UTF-8 字节数）

        // 检查文件名是否完整
        if (m_dataBuffer.size() < sizeof(qint64) * 2 + fileNameLen)
        {
            qDebug() << "文件名未接收完整，等待后续数据";
            return;
        }

        // 解析文件名（读取原始 UTF-8 字节）
        QByteArray nameBytes = m_dataBuffer.mid(static_cast<int>(sizeof(qint64) * 2), static_cast<int>(fileNameLen));
        QString fileName = QString::fromUtf8(nameBytes);
        if (fileName.isEmpty())
        {
            emit taskFailed(m_clientKey, "解析到空文件名，终止接收");
            stopRecvFile();
            return;
        }

        // 生成最终保存路径
        m_finalRecvPath = m_saveDir + fileName;
        QFileInfo fileInfo(m_finalRecvPath);
        QString baseName = fileInfo.baseName();
        QString suffix = fileInfo.suffix();
        int count = 1;
        // 如果文件已存在，自动加后缀
        while (QFile::exists(m_finalRecvPath))
        {
            if (suffix.isEmpty())
            {
                m_finalRecvPath = m_saveDir + baseName + QString("(%1)").arg(count);
            }
            else
            {
                m_finalRecvPath = m_saveDir + baseName + QString("(%1).").arg(count) + suffix;
            }
            count++;
        }

        // 创建文件对象
        m_fileHandler = new QFile(m_finalRecvPath);
        if (!m_fileHandler->open(QFile::WriteOnly))
        {
            emit taskFailed(m_clientKey, QString("文件创建失败：%1").arg(m_fileHandler->errorString()));
            stopRecvFile();
            return;
        }

        // 计算已接收的包头字节数，剩余为文件内容
        m_processedBytes = sizeof(qint64) * 2 + fileNameLen;
        // 移除缓冲区中的包头数据，保留文件内容
        m_dataBuffer = m_dataBuffer.mid(m_processedBytes);

        qDebug() << "解析包头完成：总字节数=" << m_totalBytes << "文件名=" << fileName << "保存路径=" << m_finalRecvPath;
    }

    // 接收文件内容
    if (m_processedBytes < m_totalBytes && m_fileHandler->isOpen())
    {
        // 计算剩余需要接收的字节数
        quint64 remainBytes = m_totalBytes - m_processedBytes;
        // 本次写入的字节数（取缓冲区大小和剩余字节数的最小值）
        quint64 writeBytes = qMin(static_cast<quint64>(m_dataBuffer.size()), remainBytes);

        // 写入文件
        qint64 actualWrite = m_fileHandler->write(m_dataBuffer.left(writeBytes));
        if (actualWrite < 0)
        {
            emit taskFailed(m_clientKey, QString("文件写入失败：%1").arg(m_fileHandler->errorString()));
            stopRecvFile();
            return;
        }

        // 更新接收状态
        m_processedBytes += actualWrite;
        // 移除已写入的缓冲区数据
        m_dataBuffer = m_dataBuffer.mid(actualWrite);

        qDebug() << "接收文件中：" << m_clientKey << "已接收=" << m_processedBytes << "/" << m_totalBytes;

        // 判断是否接收完成
        if (m_processedBytes >= m_totalBytes)
        {
            m_fileHandler->close();
            m_fileHandler->deleteLater();
            m_fileHandler = nullptr;

            emit recvFinished(m_clientKey, m_finalRecvPath);
            qDebug() << "文件接收完成：" << m_clientKey << "保存路径=" << m_finalRecvPath;

            // 重置状态 + 退出事件循环
            m_isRunning = false;
            m_totalBytes = 0;
            m_processedBytes = 0;
            if (m_eventLoop)
                m_eventLoop->quit();
        }
    }
}

void FileSocket::onSocketDisconnected()
{
    qDebug() << "客户端断开连接：" << m_clientKey;
    if (m_processedBytes > 0 && m_processedBytes < m_totalBytes)
    {
        emit taskFailed(m_clientKey, "客户端提前断开连接，文件接收中断");
    }
    stopRecvFile();
}

void FileSocket::onSocketError(QAbstractSocket::SocketError err)
{
    QString errMsg = QString("Socket错误：%1（%2）").arg(m_tcpSocket->errorString()).arg(err);
    qDebug() << m_clientKey << errMsg;
    emit taskFailed(m_clientKey, errMsg);
    stopRecvFile();
}
