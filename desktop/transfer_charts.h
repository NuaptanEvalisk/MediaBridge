// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QBitArray>
#include <QColor>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPalette>
#include <QSet>
#include <QToolTip>
#include <QVector>
#include <QWidget>
#include <algorithm>
#include <cmath>

// Lightweight native Qt visualizations, without Qt Charts or a web runtime.
// Both widgets are repainted at most once per telemetry tick, not per chunk.
class SpeedGraph final : public QWidget {
public:
    struct Sample { qint64 millis; double mibPerSecond; bool valid; };
    static constexpr qint64 kWindowMs = 5 * 60 * 1000;

    explicit SpeedGraph(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(154);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleName("Transfer speed history");
    }
    void clear() { samples_.clear(); update(); }
    int sampleCount() const { return samples_.size(); }
    void append(qint64 millis, double mibPerSecond, bool valid) {
        if (!samples_.isEmpty() && millis <= samples_.last().millis) return;
        samples_.append({millis, std::max(0.0, mibPerSecond),
                         valid && std::isfinite(mibPerSecond)});
        // Keep a fixed history window plus one point for continuity.
        const qint64 cutoff = millis - kWindowMs;
        while (samples_.size() > 2 && samples_.at(1).millis < cutoff)
            samples_.removeFirst();
        if (isVisible()) update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF plot = QRectF(rect()).adjusted(56, 10, -14, -27);
        if (plot.width() < 15 || plot.height() < 15) return;

        const QColor muted = palette().color(QPalette::Disabled, QPalette::Text);
        QColor grid = muted; grid.setAlpha(65);
        const QColor line = palette().color(QPalette::Highlight);
        painter.setPen(QPen(grid, 1));
        for (int i = 0; i <= 4; ++i) {
            const qreal y = plot.top() + i * plot.height() / 4.0;
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }
        const qint64 latest = samples_.isEmpty() ? 0 : samples_.last().millis;
        const qint64 left = latest - kWindowMs;
        const qint64 span = kWindowMs;
        double maximum = 1.0;
        for (const auto &sample : samples_)
            if (sample.valid && sample.millis >= left)
                maximum = std::max(maximum, sample.mibPerSecond);
        maximum = std::ceil(maximum * 1.15);

        painter.setPen(muted);
        const QFontMetrics fm(font());
        for (int i = 0; i <= 4; ++i) {
            const double value = maximum * (4.0 - i) / 4.0;
            const QString label = QString::number(value, 'f', maximum >= 10 ? 0 : 1);
            const QRectF labelRect(0, plot.top() + i * plot.height()/4.0 - fm.height()/2.0,
                                   plot.left() - 7, fm.height());
            painter.drawText(labelRect, Qt::AlignRight | Qt::AlignVCenter, label);
        }
        painter.drawText(QRectF(2, 0, width()-4, fm.height()),
                         Qt::AlignLeft, "MiB/s");
        painter.drawText(QRectF(plot.left(), plot.bottom()+3, 90, fm.height()),
                         Qt::AlignLeft, "−5 min");
        painter.drawText(QRectF(plot.right()-80, plot.bottom()+3, 80, fm.height()),
                         Qt::AlignRight, "Now");
        if (samples_.isEmpty()) {
            painter.drawText(plot, Qt::AlignCenter, "No transfer samples yet");
            return;
        }
        QPainterPath linePath, fillPath;
        bool inSegment = false;
        QPointF segmentStart, previous;
        auto finishSegment = [&] {
            if (!inSegment) return;
            fillPath.lineTo(previous.x(), plot.bottom());
            fillPath.lineTo(segmentStart.x(), plot.bottom());
            fillPath.closeSubpath();
            inSegment = false;
        };
        for (const auto &sample : samples_) {
            if (sample.millis < left || !sample.valid) {
                finishSegment();
                continue;
            }
            const double x = plot.left() + plot.width() *
                double(sample.millis - left) / double(span);
            const double y = plot.bottom() - plot.height() * sample.mibPerSecond / maximum;
            const QPointF point(x, std::clamp(y, plot.top(), plot.bottom()));
            if (!inSegment) {
                linePath.moveTo(point);
                fillPath.moveTo(point);
                segmentStart = point;
                inSegment = true;
            } else {
                linePath.lineTo(point);
                fillPath.lineTo(point);
            }
            previous = point;
        }
        finishSegment();
        QColor fill = line; fill.setAlpha(45);
        painter.fillPath(fillPath, fill);
        painter.setPen(QPen(line, 2));
        painter.drawPath(linePath);
    }

private:
    QVector<Sample> samples_;
};

// Each square represents a chunk, or a contiguous chunk range for large files.
// Mixed ranges show the confirmed fraction and highlight in-flight/retry work.
class ChunkMap final : public QWidget {
public:
    explicit ChunkMap(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(152);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
        setAccessibleName("Per-chunk transfer status map");
    }
    void clear() {
        completed_.clear(); active_.clear(); retry_.clear();
        update();
    }
    void setChunks(const QBitArray &completed, const QSet<int> &active,
                   const QSet<int> &retry) {
        completed_ = completed;
        active_ = active;
        retry_ = retry;
        if (isVisible()) update();
    }
    int chunkCount() const { return completed_.size(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        const auto theme = colors();
        const QFontMetrics fm(font());
        const QRect area = rect().adjusted(8, 7, -8, -39);
        if (area.width() < 10 || area.height() < 10) return;
        if (completed_.isEmpty()) {
            p.setPen(palette().color(QPalette::Disabled, QPalette::Text));
            p.drawText(area, Qt::AlignCenter, "No file selected for transfer");
            return;
        }
        const int cols = columns(area.width());
        const int rows = std::max(1, std::min(9, area.height() / 12));
        const int bins = std::min(int(completed_.size()), cols * rows);
        const int visibleRows = (bins + cols - 1) / cols;
        const int tileH = std::min(12, area.height() / visibleRows);
        const int tileW = std::max(1, area.width() / cols);
        for (int b = 0; b < bins; ++b) {
            const int begin = int(qint64(b) * completed_.size() / bins);
            const int end = int(qint64(b+1) * completed_.size() / bins);
            int done = 0;
            bool sending = false, retrying = false;
            for (int i = begin; i < end; ++i) {
                if (completed_.testBit(i)) ++done;
                if (active_.contains(i)) sending = true;
                if (retry_.contains(i)) retrying = true;
            }
            const QRect tile(area.left() + (b % cols) * tileW,
                             area.top() + (b / cols) * tileH,
                             std::max(1, tileW - 2), std::max(1, tileH - 2));
            p.fillRect(tile, theme.pending);
            if (done > 0) {
                const int width = std::max(1, tile.width() * done / (end - begin));
                p.fillRect(QRect(tile.left(),tile.top(),width,tile.height()),theme.done);
            }
            if (sending || retrying) {
                // A colored band identifies active/retrying chunks even when
                // the tile also contains already completed chunks.
                p.fillRect(QRect(tile.left(),tile.top(),tile.width(),
                                 std::min(3,tile.height())),
                           retrying ? theme.retry : theme.active);
            }
        }
        const int count = completed_.size();
        const int doneCount = completed_.count(true);
        p.setPen(palette().color(QPalette::Text));
        int activelySending = 0;
        for (int i : active_) if (!retry_.contains(i)) ++activelySending;
        p.drawText(QRect(8,height()-33,width()-16,18),
                   QString("%1 / %2 complete (%3%)  ·  %4 active  ·  %5 retrying")
                       .arg(doneCount).arg(count)
                       .arg(100.0 * doneCount / count,0,'f',1)
                       .arg(activelySending).arg(retry_.size()));
        // Compact legend in the panel footer, using the native palette.
        const int y = height()-12;
        int x = 8;
        for (const auto &item : {qMakePair(theme.done,QString("Done")),
                                 qMakePair(theme.active,QString("Active")),
                                 qMakePair(theme.retry,QString("Retry")),
                                 qMakePair(theme.pending,QString("Pending"))}) {
            p.fillRect(QRect(x,y,9,9),item.first);
            x += 13;
            p.setPen(palette().color(QPalette::Text));
            p.drawText(QRect(x,y-3,64,16),item.second);
            x += fm.horizontalAdvance(item.second)+13;
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (completed_.isEmpty()) return;
        const QRect area = rect().adjusted(8, 7, -8, -39);
        const int cols = columns(area.width());
        const int rows = std::max(1, std::min(9, area.height()/12));
        const int bins = std::min(int(completed_.size()), cols * rows);
        const int visibleRows = (bins + cols - 1) / cols;
        const int tileW = std::max(1,area.width()/cols);
        const int tileH = std::min(12,area.height()/visibleRows);
        const QPoint pos = event->pos();
        if (!area.contains(pos)) { QToolTip::hideText(); return; }
        const int col = (pos.x()-area.left()) / tileW;
        const int row = (pos.y()-area.top()) / tileH;
        const int bin = row*cols+col;
        if (bin >= bins || col >= cols) { QToolTip::hideText(); return; }
        const int begin = int(qint64(bin)*completed_.size()/bins);
        const int end = int(qint64(bin+1)*completed_.size()/bins);
        int done = 0;
        for (int i = begin; i < end; ++i) if (completed_.testBit(i)) ++done;
        QToolTip::showText(event->globalPosition().toPoint(),
            begin + 1 == end
                ? QString("Chunk %1 · %2").arg(begin+1)
                    .arg(done ? "Complete" : retry_.contains(begin) ? "Retrying"
                         : active_.contains(begin) ? "Transferring" : "Pending")
                : QString("Chunks %1–%2 · %3 / %4 complete")
                    .arg(begin+1).arg(end).arg(done).arg(end-begin), this);
    }

private:
    struct Theme { QColor done, active, retry, pending; };
    Theme colors() const {
        const auto hl = palette().color(QPalette::Highlight);
        const QColor done = QColor::fromHsv(145, 155, palette().color(QPalette::Window).lightness() < 128 ? 215 : 145);
        const QColor retry = QColor::fromHsv(35,195,215);
        QColor pending = palette().color(QPalette::Mid);
        pending.setAlpha(105);
        return {done,hl,retry,pending};
    }
    static int columns(int width) { return std::max(1,width/11); }
    QBitArray completed_;
    QSet<int> active_, retry_;
};
