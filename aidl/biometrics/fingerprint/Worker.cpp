// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#include "Worker.h"

namespace lge::fingerprint {

Worker::Worker() : mThread([this] { loop(); }) {}

Worker::~Worker() {
    {
        std::lock_guard lock(mMutex);
        mStopping = true;
    }
    mCondition.notify_one();
    mThread.join();
}

void Worker::post(std::function<void()> task, std::chrono::milliseconds delay) {
    {
        std::lock_guard lock(mMutex);
        if (mStopping) return;
        mTasks.push({std::chrono::steady_clock::now() + delay, mSerial++, std::move(task)});
    }
    mCondition.notify_one();
}

void Worker::loop() {
    std::unique_lock lock(mMutex);
    while (!mStopping) {
        if (mTasks.empty()) {
            mCondition.wait(lock);
        } else if (mTasks.top().due > std::chrono::steady_clock::now()) {
            const auto due = mTasks.top().due;
            mCondition.wait_until(lock, due);
        } else {
            auto task = mTasks.top().run;
            mTasks.pop();
            lock.unlock();
            task();
            lock.lock();
        }
    }
}

}  // namespace lge::fingerprint
