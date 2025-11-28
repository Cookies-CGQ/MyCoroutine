#pragma once

#include <mutex>
#include <vector> 
#include <string>
#include "fiber.h"
#include "thread.h"

namespace nsCoroutine
{
    class Scheduler
    {
    public:
        //threads指定线程池的线程数量，use_caller指定是否将主线程作为工作线程，name调度器的名称
        Scheduler(size_t threads = 1, bool use_caller = true, const std::string& name = "Scheduler");
        //防止出现资源泄漏，基类指针删除派生类对象时不完全销毁的问题
        virtual ~Scheduler();
        //获取调度器的名字
        const std::string& getName() const
        {
            return _m_name;
        }
    
    public:
        //获取当前线程正在运行的调度器 -- 线程局部存储
        static Scheduler* GetThis();

    protected:
        //设置当前线程正在运行的调度器 -- 线程局部存储
        void SetThis();

    private:
        //任务
        struct ScheduleTask
        {
            std::shared_ptr<Fiber> _fiber; //执行任务的协程对象 -- 调度对象是协程
            std::function<void()> _cb;     //执行任务的函数指针 -- 调度对象是函数
            int _thread; //指定任务需要运行的线程id
            
            ScheduleTask()
            {
                _fiber = nullptr;
                _cb = nullptr;
                _thread = -1;
            }

            //协程+线程
            ScheduleTask(std::shared_ptr<Fiber> f, int thr)
            {
                _fiber = f;
                _thread = thr;
            }

            //协程+线程
            ScheduleTask(std::shared_ptr<Fiber>* f, int thr)
            {
                //将内容转移也就是指针内部的转移和上面的赋值不同，引用计数不会增加
                _fiber.swap(*f);
                _thread = thr;
            }

            //函数+线程
            ScheduleTask(std::function<void()> f, int thr)
            {
                _cb = f;
                _thread = thr;
            }

            //函数+线程
            ScheduleTask(std::function<void()>* f, int thr)
            {
                _cb.swap(*f);
                _thread = thr;
            }

            //重置任务对象
            void reset()
            {
                _fiber = nullptr;
                _cb = nullptr;
                _thread = -1;
            }
        };

    public:
        //添加任务到任务队列
        //FiberOrCb调度任务类型，可以是协程对象或函数指针
        template<class FiberOrCb>
        void scheduleLock(FiberOrCb fc, int thread = -1)
        {
            //用于标记任务队列是否为空，从而判断是否需要唤醒线程。
            bool need_tickle;
            
            {
                std::lock_guard<std::mutex> lock(_m_mutex);
                //empty -> 所有线程都是空闲的，需要唤醒线程
                need_tickle = _m_tasks.empty();
                //创建Task的任务对象
                ScheduleTask task(fc, thread);
                //存在就加入
                if(task._fiber || task._cb)
                {
                    _m_tasks.push_back(task);
                }
            }

            //如果检查出了队列为空，就唤醒线程
            if(need_tickle)
            {
                tickle();
            }
        }

        //启动线程池，启动调度器
        virtual void start();
        //关闭线程池，停止调度器，等所有调度任务都执行完后再返回。
        virtual void stop();
    
    protected:
        //唤醒线程 -- 提醒有新任务需要执行
        virtual void tickle();
        //线程函数 -- 给调用线程提供事件循环
        virtual void run();
        //空闲协程入口函数，无任务调度时执行idle协程
        virtual void idle();
        //是否可以关闭
        virtual bool stopping();
        //返回是否有空闲线程
        //当调度协程进入idle时空闲线程数+1，从idle协程返回时空闲线程数-1
        bool hasIdleThreads() {return _m_idleThreadCount>0;}

    private:
        //调度器名称
        std::string _m_name;
        //互斥锁 -> 保护任务队列
        std::mutex _m_mutex;
        //线程池，存初始化好的线程
        std::vector<std::shared_ptr<Thread>> _m_threads;
        //存储工作线程的线程id
        std::vector<int> _m_threadIds;
        //任务队列
        std::vector<ScheduleTask> _m_tasks;
        //需要额外创建的线程数 -- 不包含主线程（调度器线程）
        size_t _m_threadCount = 0;
        //活跃线程数
        std::atomic<size_t> _m_activeThreadCount = {0};
        //空闲线程数
        std::atomic<size_t> _m_idleThreadCount = {0};
        //主线程是否参与调度
        bool _m_useCaller;
        //如果主线程参与调度->需要额外创建调度协程
        std::shared_ptr<Fiber> _m_schedulerFiber;
        //如果主线程参与调度->记录主线程的线程id
        int _m_rootThread = -1;
        //是否正在关闭
        bool _m_stopping = false;
    };
}