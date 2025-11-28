#pragma once
#include <iostream>
#include <memory>
#include <atomic>
#include <functional>
#include <cassert>
#include <ucontext.h>
#include <unistd.h>
#include <mutex>

//Fiber类提供协程的基本功能，包括创建、管理、切换、销毁协程\
它使用ucontext结果（主要是使用非对称协程）保存和恢复协程的上下文，并通过std::function来存储协程的执行逻辑

namespace nsCoroutine
{
    // 非对称有独立栈协程
    // 这里的继承使用enable_shared_from_this，是为了在Fiber内部可以通过shared_from-this() 获取到自身的shared_ptr实例，
    // 从而在需要时可以将自身的shared_ptr实例传递给其他地方，而不是裸指针（一个shared_ptr控制块管理，如果直接使用裸指针创建shared_ptr实例会导致多个控制块管理，导致计数混乱，多次释放资源的问题）。
    class Fiber : public std::enable_shared_from_this<Fiber>
    {
    public:
        // 定义协程状态
        // READY <=> RUNNING -> TERM
        enum State
        {
            READY,   // 协程处于就绪状态
            RUNNING, // 协程处于运行状态
            TERM     // 协程处于结束状态
        };

    private:
        // 私有Riber()，只能被GetThis调用，用于创建主协程
        //当第一次调用GetThis时，会创建主协程
        Fiber();

    public:
        //用于创建子协程
        // 用于创建指定回调函数、栈大小和run_in_scheduler本协程是否参与调度器调度，默认为true
        Fiber(std::function<void()> cb, size_t stacksize = 0, bool run_in_scheduler = true);
        ~Fiber();

    public:
        // 重置协程状态和入口函数，复用栈空间，不重新创建协程
        // 减少反复申请空间的开销
        void reset(std::function<void()> cb);
        // 恢复协程执行
        void resume();
        // 将执行权还给调度协程
        void yield();
        // 获取协程唯一标识
        uint64_t getId() const
        {
            return _m_id;
        }
        // 获取协程状态
        State getState() const
        {
            return _m_state;
        }

    public:
        // 设置当前运行的协程
        static void SetThis(Fiber *f);
        // 获取当前运行的协程的shared_ptr实例，兼具第一次调用时创建主协程的功能
        static std::shared_ptr<Fiber> GetThis();
        // 设置调度协程，默认是主协程，即主协程也可以是调度协程
        static void SetSchedulerFiber(Fiber *f);
        // 获取当前运行的协程的ID
        static uint64_t GetFiberId();
        // 协程的主函数，入口点
        static void MainFunc();

    public:
        std::mutex _m_mutex;

    private:
        // 协程唯一标识符 -- 使用自己的定义的全局ID生成器
        uint64_t _m_id = 0;
        // 栈大小--主协程不需要
        uint32_t _m_stacksize = 0;
        // 协程状态--初始化为READY
        State _m_state = READY;
        // 协程上下文
        ucontext_t _m_ctx;
        // 协程栈的指针--主协程不需要
        void *_m_stack = nullptr;
        // 协程的回调函数--主协程不需要
        std::function<void()> _m_cb;
        // 标志是否将执行器交给调度协程--主协程不需要
        bool _m_runInScheduler;
    };
}