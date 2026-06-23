#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// Non-RT worker pool for long/blocking Supervisor-LOCAL tasks (config load,
// persistence, metrics, retries). It NEVER touches FSM state — a finished task
// posts its result back to the FSM as an event (it captures an EventQueue&), so
// the result is processed by the single serialized FSM thread (SDS §15 threading).
namespace hermes {

class WorkerPool {
public:
    void start(int n) {
        mRunning = true;
        for (int i = 0; i < n; ++i) mThreads.emplace_back([this] { loop(); });
    }
    void stop() {
        { std::lock_guard<std::mutex> g(mMutex); mRunning = false; }
        mCond.notify_all();
        for (auto& t : mThreads) if (t.joinable()) t.join();
        mThreads.clear();
    }
    void submit(std::function<void()> task) {
        { std::lock_guard<std::mutex> g(mMutex); mTasks.push(std::move(task)); }
        mCond.notify_one();
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mMutex);
                mCond.wait(lk, [&] { return !mRunning || !mTasks.empty(); });
                if (!mRunning && mTasks.empty()) return;
                task = std::move(mTasks.front());
                mTasks.pop();
            }
            task();
        }
    }
    std::mutex                        mMutex;
    std::condition_variable           mCond;
    std::queue<std::function<void()>> mTasks;
    std::vector<std::thread>          mThreads;
    bool                              mRunning = false;
};

} // namespace hermes
