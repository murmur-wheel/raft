#ifndef UTIL_H
#define UTIL_H

#include "pch.h"

/**
 * 引用计数基类
 */
class RefCounted {
public:
    NOCOPYABLE_BODY(RefCounted)

    RefCounted() = default;
    virtual ~RefCounted() = default;

    int32_t inc_ref() { return ++ref_count_; }
    int32_t dec_ref() { return --ref_count_; }

private:
    std::atomic<int32_t> ref_count_ = { 1 };
};

class DefaultAllocator {
public:
    /* 分配内存 */
    void *allocate(size_t len) { return ::malloc(len); }
    void free(void *block) { ::free(block); }
};

/**
 * 引用计数智能指针
 */
template<typename T, typename Allocator = DefaultAllocator>
class Ptr {
public:
    Ptr() = default;
    Ptr(std::nullptr_t) : ptr_(nullptr) {}
    Ptr(const Ptr<T> &other) : ptr_(nullptr) { set(other.ptr_); }
    Ptr(Ptr<T> &&other) noexcept : ptr_(nullptr) {
        ptr_ = other.ptr_;
        other.ptr_ = nullptr;
    }
    ~Ptr() { set(nullptr); }

    Ptr<T> &operator=(const Ptr<T> &other) {
        set(other.ptr_);
        return *this;
    }
    Ptr<T> &operator=(Ptr<T> &&other) noexcept {
        ptr_ = other.ptr_;
        other.ptr_ = nullptr;
        return *this;
    }

    T *operator->() const { return ptr_; }

    operator bool() const { return ptr_ != nullptr; }
    bool operator==(std::nullptr_t) const { return ptr_ == nullptr; }

    void set(T *ptr) {
        if (ptr) {
            ptr->inc_ref();
        }
        if (ptr_) {
            if (ptr_->dec_ref()) {
                ptr_->~T();
                Allocator().free(ptr_);
            }
        }
        ptr_ = ptr;
    }

    static Ptr<T> from(T *ptr) {
        Ptr<T> t;
        t.ptr_ = ptr;
        return t;
    }

    template<typename... Args>
    static Ptr<T> make(Args &&... args) {
        auto block = Allocator().allocate(sizeof(T));
        return Ptr<T>::from(new (block) T(std::forward<Args>(args)...));
    }

private:
    T *ptr_ = nullptr;
};

/**
 * 定时器，由后台进程，条件变量两部分组成：
 *   后台进程：执行回调函数
 *   条件变量：超时等待，和主线程同步
 */
class Timer {
public:
    NOCOPYABLE_BODY(Timer)

    Timer() {
        // UINT32_MAX 表示一个永不可达的时间
        tp_ = system_clock::now() + milliseconds(UINT32_MAX);

        thr_ = std::thread([&] {
            while (running_) {
                do {
                    std::unique_lock<std::mutex> lock(mtx_);
                    cv_.wait_until(lock, tp_);
                } while (running_ && system_clock::now() < tp_);

                if (callback_) {
                    callback_();
                }
            }
        });
    }

    Timer(const std::function<void()> &callback, uint32_t tp) : Timer() {
        set(callback, tp);
    }

    ~Timer() {
        running_ = false;
        callback_ = {};
        cv_.notify_one();
        thr_.join();
    }

    void set(const std::function<void()> &callback, uint32_t tp) {
        /**
         * 设置回调函数和回调时间
         * 设置完成后通过信号量唤醒回调线程以应用新的超时时间
         */

        std::lock_guard<std::mutex> lock(mtx_);
        callback_ = callback;
        tp_ = system_clock::now() + milliseconds(tp);
        cv_.notify_one();
    }

    void set(uint32_t tp) { set(callback_, tp); }

    static std::unique_ptr<Timer> create(const std::function<void()> &callback,
        uint32_t tp) {
        return std::unique_ptr<Timer>(new Timer(callback, tp));
    }

private:
    std::mutex mtx_ = {};
    std::condition_variable cv_ = {};
    time_point<system_clock> tp_;
    std::function<void()> callback_ = {};
    std::thread thr_ = {};
    std::atomic<bool> running_ = { true };
};

/**
 * 消息队列
 */
template<typename _Ty>
class ConcurrentQueue : public RefCounted {
public:
    NOCOPYABLE_BODY(ConcurrentQueue)

    ConcurrentQueue() = default;
    ~ConcurrentQueue() = default;

    void enqueue(const _Ty &message) {
        /**
         * 消息入队列
         */
        {
            std::lock_guard<std::mutex> lock(mtx_);
            messages_.push(message);
        }
        cv_.notify_one();
    }

    _Ty dequeue() {
        /**
         * 消息出队，如果队列中不存在任何消息，将会阻塞直到有消息可供处理为止
         */
        std::unique_lock<std::mutex> lock(mtx_);

        if (messages_.empty()) {
            cv_.wait(lock, [&] { return !messages_.empty(); });
        }

        const auto message = messages_.front();
        messages_.pop();
        return message;
    }

    /**
     * 创建消息队列
     */
    static Ptr<ConcurrentQueue<_Ty>> create() {
        return Ptr<ConcurrentQueue<_Ty>>::from(new ConcurrentQueue<_Ty>());
    }

private:
    std::mutex mtx_ = {};
    std::queue<_Ty> messages_ = {};
    std::condition_variable cv_ = {};
};

/**
 * 线程池有两部分组成：
 *   1. 执行任务的后台线程
 *   2. 任务队列
 */
class ThreadPool {
public:
    NOCOPYABLE_BODY(ThreadPool)

    ThreadPool() {
        for (auto &thr : threads_) {
            thr = std::thread([&] {
                /**
                 * 执行任务的线程
                 */
                std::function<void()> func;
                while ((func = queue_.dequeue())) {
                    func();
                }
            });
        }
    }

    ~ThreadPool() {
        for (auto &thr : threads_) {
            queue_.enqueue({});
        }
        for (auto &thr : threads_) {
            thr.join();
        }
    }

    void execute(const std::function<void()> &func) {
        /**
         * 将任务放入消息队列中，放入队列中任务将会被后台线程执行
         */
        queue_.enqueue(func);
    }

    static ThreadPool *get() {
        static ThreadPool thr_pool;
        return &thr_pool;
    }

private:
    ConcurrentQueue<std::function<void()>> queue_;
    std::array<std::thread, 32> threads_ = {};
};

#endif
