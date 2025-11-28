#include <iostream>
#include <unistd.h>
#include <sys/syscall.h>

#include "thread.h"

namespace nsCoroutine
{
    // 线程信息--利用thread_local实现线程局部存储
    // 当前线程的Thread对象指针
    static thread_local Thread *t_thread = nullptr;
    // 当前线程的名字
    static thread_local std::string t_thread_name = "UNKNOWN";

    // 获取系统分配的线程id
    pid_t Thread::GetThreadId()
    {
        // syscall(SYS_gettid)是一个系统调用，用于获取线程在整个内核的唯一ID(TID)。
        // 在linux中，线程其实也是一种轻量级线程，也就是说进程里面是由一个或者多个线程组成的。
        // 也就是说一个进程，可以看成一个线程组，每个线程都有一个唯一的TID。
        // 主线程的TID就是进程的PID，都是用pid_t类型表示。
        // 也就是说，一个线程维护两个pid_t，一个是线程组id，一个是线程id；线程组id==线程id的线程是主线程。
        return syscall(SYS_gettid);
    }

    Thread *Thread::GetThis()
    {
        return t_thread;
    }

    const std::string &Thread::GetName()
    {
        return t_thread_name;
    }

    void Thread::SetName(const std::string &name)
    {
        if (t_thread)
        {
            t_thread->_m_name = name;
        }
        t_thread_name = name;
    }

    Thread::Thread(std::function<void()> cb, const std::string &name)
        : _m_cb(cb), _m_name(name)
    {
        int rt = pthread_create(&_m_thread, nullptr, &Thread::run, this);
        if (rt)
        {
            std::cerr << "pthread_create thread fail, rt=" << rt << " name=" << name << std::endl;
            throw std::logic_error("pthread_create fail");
        }
        // 主线程等待子线程函数完成初始化
        _m_semaphore.wait();
    }

    Thread::~Thread()
    {
        if (_m_thread)
        {
            // 对象销毁时，如果线程还存在，就分离，后续线程结束自己销毁
            // 例如子线程脱离主线程的管理
            pthread_detach(_m_thread);
            _m_thread = 0;
        }
    }

    void Thread::join()
    {
        if (_m_thread)
        {
            int rt = pthread_join(_m_thread, nullptr);
            if (rt)
            {
                std::cerr << "pthread_join thread fail, rt=" << rt << " name=" << _m_name << std::endl;
                throw std::logic_error("pthread_join error");
            }
            _m_thread = 0;
        }
    }

    void *Thread::run(void *arg)
    {
        Thread *thread = (Thread *)arg;

        t_thread = thread;
        t_thread_name = thread->_m_name;
        thread->_m_id = GetThreadId();
        // 设置线程名字，最多15个字符
        pthread_setname_np(pthread_self(), thread->_m_name.substr(0, 15).c_str());

        std::function<void()> cb;
        cb.swap(thread->_m_cb); // swap可以减少_m_cb中智能指针的引用计数
        // 初始化完成，这里确保了主线程创建出来一个工作线程，提供给协程使用，否则可能出现协程在未初始化的线程上使用。
        thread->_m_semaphore.signal();
        // 真正执行线程函数
        cb();
        return nullptr;
    }
}