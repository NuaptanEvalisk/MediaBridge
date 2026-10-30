// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "adaptive_parallelism.h"
#include <QJsonObject>
#include <QString>
#include <QtGlobal>
#include <cmath>
#include <optional>

// Cache completed optimization runs alongside desktop settings. Keys are
// endpoint-specific and expire after 30 days.
class AdaptiveCache {
public:
    static constexpr qint64 kMaxAgeMs = 30LL * 24 * 60 * 60 * 1000;

    struct Entry {
        int concurrency;
        double mibPerSecond;
        qint64 savedAtMs;
    };

    void load(const QJsonObject &value) { entries_ = value; }
    QJsonObject json() const { return entries_; }

    std::optional<Entry> find(const QString &endpoint, qint64 nowMs) const {
        if (endpoint.isEmpty()) return std::nullopt;
        const auto entry = entries_.value(endpoint).toObject();
        const int concurrency = entry.value("concurrency").toInt();
        const double speed = entry.value("mib_per_second").toDouble(-1);
        const double timestamp = entry.value("saved_at_ms").toDouble(-1);
        if (concurrency < AdaptiveParallelism::kInitial ||
            concurrency > AdaptiveParallelism::kMaximum ||
            !std::isfinite(speed) || speed <= 0 ||
            !std::isfinite(timestamp) || timestamp <= 0 ||
            timestamp > double(nowMs) || double(nowMs) - timestamp > kMaxAgeMs)
            return std::nullopt;
        return Entry{concurrency, speed, qint64(timestamp)};
    }

    void save(const QString &endpoint, int concurrency, double mibPerSecond,
              qint64 nowMs) {
        if (endpoint.isEmpty() ||
            concurrency < AdaptiveParallelism::kInitial ||
            concurrency > AdaptiveParallelism::kMaximum ||
            !std::isfinite(mibPerSecond) || mibPerSecond <= 0 || nowMs <= 0)
            return;
        entries_.insert(endpoint, QJsonObject{
            {"concurrency", concurrency},
            {"mib_per_second", mibPerSecond},
            {"saved_at_ms", double(nowMs)}
        });
        // Bound settings growth even when many test receivers are used.
        while (entries_.size() > 12) {
            QString oldest;
            double oldestTime = std::numeric_limits<double>::infinity();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                const double time = it.value().toObject().value("saved_at_ms").toDouble(0);
                if (time < oldestTime) {
                    oldestTime = time;
                    oldest = it.key();
                }
            }
            entries_.remove(oldest);
        }
    }

    void remove(const QString &endpoint) { entries_.remove(endpoint); }

private:
    QJsonObject entries_;
};
