#pragma once

#include <iostream>
#include <thread>
#include <functional>
#include <string>
#include <condition_variable>

namespace nsCoroutine
{
    // 信号量，用于线程方法间的同步
    class Semaphore
    {
    private:
        std::mutex _mtx;
        std::condition_variable _cv;
        int _count;

    public:
        // 信号量初始化为0
        explicit Semaphore(int count = 0) // explicit关键字防止隐式转换
            : _count(count)
        {
        }

        // P操作，-1
        void wait()
        {
            std::unique_lock<std::mutex> lock(_mtx);
            while (_count == 0) // 循环防止虚假唤醒
            {
                _cv.wait(lock);
            }
            --_count;
        }

        // V操作，+1
        void signal()
        {
            std::unique_lock<std::mutex> lock(_mtx);
            ++_count;
            _cv.notify_one(); // 唤醒一个等待线程
        }
    };

    // 创建并管理底层线程，为协程提供运行环境，同时通过线程局部存储和同步机制，为协程调度提供必要支持，确保协程可以在合适的线程上被正确的调度和执行。
    class Thread
    {
    public:
        Thread(std::function<void()> cb, const std::string &name);
        ~Thread();
        // 获取线程的id
        pid_t getId() const
        {
            return _m_id;
        }
        // 获取线程的名字
        const std::string &getName() const
        {
            return _m_name;
        }
        // 等待线程结束
        void join();

    public:
        // 下列方法为静态方法，可直接通过类名调用，配合线程局部存储使用

        // 获取系统分配的线程id
        static pid_t GetThreadId();
        // 获取当前所在的线程
        static Thread *GetThis();
        // 获取当前线程的名字
        static const std::string &GetName();
        // 设置当前线程的名字
        static void SetName(const std::string &name);

    private:
        // 线程函数
        static void *run(void *arg);

    private:
        pid_t _m_id = -1;        // 内核全局线程id
        pthread_t _m_thread = 0; // POSIX线程id

        // 线程需要运行的函数
        std::function<void()> _m_cb;
        std::string _m_name; // 线程名字

        // 信号量--来完成线程的同步创建
        Semaphore _m_semaphore;
    };
}