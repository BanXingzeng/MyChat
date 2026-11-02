#ifndef AUDIORECORDDIALOG_H
#define AUDIORECORDDIALOG_H

#include <QWidget>

class QLabel;
class QTimer;

/**
 * 录音浮窗：录音期间浮在聊天窗口上方，显示计时和发送/取消按钮。
 * 点"发送"或再点一次语音按钮都会结束录音；ESC / "取消"丢弃本次录音。
 */
class AudioRecordDialog : public QWidget
{
    Q_OBJECT
public:
    explicit AudioRecordDialog(QWidget *parent = nullptr);
    ~AudioRecordDialog();

    void setElapsed(int ms);        // 更新计时显示

signals:
    void signalSend();
    void signalCancel();

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    void updateText();

    QLabel *m_labelTime = nullptr;
    QLabel *m_labelDot  = nullptr;
    QTimer *m_blinkTimer = nullptr;
    bool    m_bDotOn = true;
    int     m_elapsedMs = 0;
};

#endif // AUDIORECORDDIALOG_H
