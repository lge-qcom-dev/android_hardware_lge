// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

namespace lge::fingerprint {

// Vendor calls and framework callbacks share a single owner thread. Delayed work never
// blocks binder dispatch, and immediate cancellation takes precedence over settling work.
class Worker {
  public:
    Worker();
    ~Worker();
    void post(std::function<void()> task, std::chrono::milliseconds delay = {});

  private:
    struct Task {
        std::chrono::steady_clock::time_point due;
        uint64_t serial;
        std::function<void()> run;
        bool operator<(const Task& other) const {
            return due == other.due ? serial > other.serial : due > other.due;
        }
    };
    void loop();
    std::mutex mMutex;
    std::condition_variable mCondition;
    std::priority_queue<Task> mTasks;
    uint64_t mSerial = 0;
    bool mStopping = false;
    std::thread mThread;
};

}  // namespace lge::fingerprint
