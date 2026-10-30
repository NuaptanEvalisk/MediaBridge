// SPDX-License-Identifier: GPL-3.0-only
#include "../adaptive_parallelism.h"
#include <cassert>
#include <cmath>

static AdaptiveParallelism::Result measure(AdaptiveParallelism& controller,
                                           std::int64_t start, std::int64_t bytes,
                                           int mibPerSecond) {
    constexpr std::int64_t mib = 1024 * 1024;
    controller.sample(start, bytes, 500);
    controller.sample(start + 3000, bytes + mib * mibPerSecond * 3, 500);
    return controller.sample(start + 13000, bytes + mib * mibPerSecond * 13, 500);
}

int main() {
    using Mode = AdaptiveParallelism::Decision;
    AdaptiveParallelism controller;
    assert(controller.current() == 4);
    auto result = measure(controller, 0, 0, 8);
    assert(result.decision == Mode::Increase && controller.current() == 6);
    result = measure(controller, 15000, 150 * 1024 * 1024, 12);
    assert(result.decision == Mode::Increase && controller.current() == 8);
    result = measure(controller, 30000, 350 * 1024 * 1024, 12);
    assert(result.decision == Mode::Settled && controller.current() == 6);
    assert(controller.best() == 6 && controller.settled());
    assert(controller.sample(45000, 999999999, 500).decision == Mode::None);

    controller.reset();
    measure(controller, 0, 0, 10);
    result = measure(controller, 15000, 150 * 1024 * 1024, 7);
    assert(result.decision == Mode::Settled && controller.current() == 4);
    assert(controller.best() == 4);

    controller.reset();
    measure(controller, 0, 0, 10);
    result = controller.networkFailure();
    assert(result.decision == Mode::Backoff && controller.current() == 4);

    controller.reset();
    assert(controller.networkFailure().decision == Mode::None);
    assert(!controller.settled());
    assert(measure(controller, 20000, 0, 10).decision == Mode::Increase);

    controller.reset();
    std::int64_t time = 0, bytes = 0;
    for (int rate = 10; rate < 20; ++rate) {
        result = measure(controller, time, bytes, rate * 2);
        time += 15000;
        bytes += 400 * 1024 * 1024;
        if (controller.settled()) break;
    }
    assert(controller.current() == 24 && controller.settled());

    controller.reset();
    controller.sample(0, 0, 500);
    controller.sample(3000, 20 * 1024 * 1024, 500);
    result = controller.sample(13000, 150 * 1024 * 1024, 2);
    assert(result.decision == Mode::None);
    assert(controller.current() == 4);

    controller.reset();
    controller.sample(0, 0, 500);
    controller.sample(3000, 20 * 1024 * 1024, 500);
    controller.interrupt();
    result = controller.sample(300000, 100 * 1024 * 1024, 500);
    assert(result.decision == Mode::None);
    assert(controller.current() == 4);
}
