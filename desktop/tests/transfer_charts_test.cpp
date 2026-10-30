// SPDX-License-Identifier: GPL-3.0-only
#include "../transfer_charts.h"
#include <QApplication>
#include <QImage>
#include <QSet>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

static void checkRendered(const QImage &image) {
    QSet<QRgb> colors;
    for (int y = 0; y < image.height(); y += 3) {
        for (int x = 0; x < image.width(); x += 3) {
            colors.insert(image.pixel(x,y));
            if (colors.size() > 8) return;
        }
    }
    assert(colors.size() > 8);
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    SpeedGraph speed;
    speed.resize(760, 154);
    assert(speed.sampleCount() == 0);
    for (int i = 0; i < 800; ++i)
        speed.append(qint64(i) * 1000, 22.5 + 4.0 * std::sin(i / 18.0),
                     i % 100 != 50);
    assert(speed.sampleCount() <= 303); // bounded to five minutes
    speed.append(799000, 500.0, true); // duplicate timestamp ignored
    assert(speed.sampleCount() <= 303);
    QImage speedImage(speed.size(), QImage::Format_ARGB32_Premultiplied);
    speedImage.fill(Qt::transparent);
    speed.render(&speedImage);
    assert(!speedImage.isNull());
    checkRendered(speedImage);
    speed.clear();
    assert(speed.sampleCount() == 0);

    ChunkMap map;
    map.resize(760, 152);
    assert(map.chunkCount() == 0);
    QBitArray bits(711, false);
    bits.setBit(0);
    bits.setBit(710);
    bits.setBit(300);
    map.setChunks(bits, QSet<int>{2, 4, 6}, QSet<int>{4});
    assert(map.chunkCount() == 711);
    QImage image(map.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    map.render(&image);
    assert(!image.isNull());
    checkRendered(image);
    // A large file must be shown via aggregated chunk ranges, without
    // allocating one widget or graphics item per chunk.
    QBitArray huge(100000, true);
    huge.clearBit(1);
    map.setChunks(huge, QSet<int>{1}, {});
    map.render(&image);
    assert(map.chunkCount() == 100000);
    map.clear();
    assert(map.chunkCount() == 0);
    map.render(&image);
}
