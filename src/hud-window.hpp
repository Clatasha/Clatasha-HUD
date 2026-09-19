#pragma once

#include <QElapsedTimer>
#include <QLabel>
#include <QTimer>
#include <QWidget>

class ClatashaHudWindow final : public QWidget {
public:
    explicit ClatashaHudWindow(QWidget *parent = nullptr);

private:
    void refresh();
    static QString formatElapsed(qint64 milliseconds);

    QLabel *titleLabel_ = nullptr;
    QLabel *recordingLabel_ = nullptr;
    QLabel *streamingLabel_ = nullptr;

    QTimer refreshTimer_;
    QElapsedTimer recordingTimer_;
    QElapsedTimer streamingTimer_;

    bool recordingWasActive_ = false;
    bool streamingWasActive_ = false;
};
