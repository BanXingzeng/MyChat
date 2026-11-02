#include "audiorecorddialog.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

AudioRecordDialog::AudioRecordDialog(QWidget *parent) : QWidget(parent)
{
    // Tool 窗口：不占任务栏、始终浮在聊天窗口之上
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFixedSize(280, 116);

    setStyleSheet(
        "AudioRecordDialog { background:#FFFFFF; border:1px solid #D5D5D5; border-radius:8px; }"
        "QLabel { background:transparent; border:none; }"
        "QPushButton { min-width:78px; min-height:28px; border-radius:4px; border:1px solid #C9C9C9;"
        "              background:#F7F7F7; color:#333333; }"
        "QPushButton:hover { background:#EDEDED; }"
        "QPushButton#btnSend { background:#12B7F5; border:1px solid #12B7F5; color:#FFFFFF; }"
        "QPushButton#btnSend:hover { background:#0FA5DE; }");

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 12, 16, 12);
    root->setSpacing(8);

    QHBoxLayout *top = new QHBoxLayout;
    top->setSpacing(6);

    m_labelDot = new QLabel(QStringLiteral("●"), this);
    m_labelDot->setStyleSheet("color:#E64340; font-size:14px;");

    m_labelTime = new QLabel(this);
    m_labelTime->setStyleSheet("font-size:17px; font-weight:bold; color:#333333;");

    QLabel *tip = new QLabel(tr("最长 60 秒"), this);
    tip->setStyleSheet("color:#999999; font-size:12px;");

    top->addWidget(m_labelDot);
    top->addWidget(m_labelTime);
    top->addStretch();
    top->addWidget(tip);

    QHBoxLayout *bottom = new QHBoxLayout;
    bottom->setSpacing(10);
    bottom->addStretch();

    QPushButton *btnCancel = new QPushButton(tr("取消"), this);
    QPushButton *btnSend   = new QPushButton(tr("发送"), this);
    btnSend->setObjectName("btnSend");

    bottom->addWidget(btnCancel);
    bottom->addWidget(btnSend);

    root->addLayout(top);
    root->addStretch();
    root->addLayout(bottom);

    connect(btnCancel, &QPushButton::clicked, this, &AudioRecordDialog::signalCancel);
    connect(btnSend,   &QPushButton::clicked, this, &AudioRecordDialog::signalSend);

    // 红点闪烁：提示"正在录"
    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(500);
    connect(m_blinkTimer, &QTimer::timeout, this, [this]()
    {
        m_bDotOn = !m_bDotOn;
        m_labelDot->setStyleSheet(m_bDotOn ? "color:#E64340; font-size:14px;"
                                           : "color:#F5C2C0; font-size:14px;");
    });
    m_blinkTimer->start();

    updateText();
}

AudioRecordDialog::~AudioRecordDialog()
{
}

void AudioRecordDialog::setElapsed(int ms)
{
    m_elapsedMs = qMax(0, ms);
    updateText();
}

void AudioRecordDialog::updateText()
{
    const int totalSec = m_elapsedMs / 1000;
    m_labelTime->setText(QString("%1:%2")
                         .arg(totalSec / 60, 2, 10, QChar('0'))
                         .arg(totalSec % 60, 2, 10, QChar('0')));
}

void AudioRecordDialog::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape)
    {
        emit signalCancel();
        return;
    }
    QWidget::keyPressEvent(e);
}
