// SPDX-License-Identifier: GPL-3.0-only
#include "../adaptive_cache.h"
#include <QJsonDocument>
#include <cassert>

int main() {
    constexpr qint64 now = 1793330000000LL;
    const QString a = "http://media-ingest:8081";
    const QString b = "http://other-receiver:8081";
    AdaptiveCache cache;
    assert(!cache.find(a, now));
    cache.save(a, 16, 83.5, now);
    assert(cache.find(a, now)->concurrency == 16);
    assert(cache.find(a, now)->mibPerSecond == 83.5);
    assert(!cache.find(b, now));
    assert(!cache.find(a, now + AdaptiveCache::kMaxAgeMs + 1));
    assert(!cache.find(a, now - 1)); // future timestamps are invalid
    assert(cache.find(a, now + AdaptiveCache::kMaxAgeMs));

    const auto serialized = QJsonDocument(cache.json()).toJson();
    AdaptiveCache reloaded;
    reloaded.load(QJsonDocument::fromJson(serialized).object());
    assert(reloaded.find(a, now)->concurrency == 16);
    reloaded.save(b, 24, 99, now);
    assert(reloaded.find(b, now)->concurrency == 24);
    assert(reloaded.find(a, now)->concurrency == 16);
    reloaded.remove(a);
    assert(!reloaded.find(a, now));
    assert(reloaded.find(b, now));

    cache.save(a, 3, 100, now);
    cache.save(a, 25, 100, now);
    cache.save(a, 20, -1, now);
    assert(cache.find(a, now)->concurrency == 16); // bad writes rejected
    cache.load(QJsonObject{{a, QJsonObject{{"concurrency", 25},
                                            {"mib_per_second", 100},
                                            {"saved_at_ms", double(now)}}}});
    assert(!cache.find(a, now)); // invalid persisted values rejected

    for (int i = 0; i < 20; ++i)
        reloaded.save(QString("http://host%1:8081").arg(i), 6, 20, now + i);
    assert(reloaded.json().size() <= 12);
}
