#pragma once

#include <iostream>
#include <memory>
#include <vector>
#include <set>
#include <shared_mutex>
#include <cassert>
#include <functional>
#include <mutex>
#include <chrono>
#include <functional>

namespace nsCoroutine 
{
    //前置声明
    class TimerManager;

    //这个public继承是用来返回智能指针timer的this值的
    class Timer : public std::enable_shared_from_this<Timer>
    {
        //设置友元类访问TimerManager的函数和成员变量
        friend class TimerManager;
    public:
        //从时间堆中删除timer
        bool cancel();
        //刷新timer
        bool refresh();
        //重设timer的超时时间，ms定时器执行间隔时间(ms)，from_now是否从当前时间开始计算
        bool reset(uint64_t ms, bool from_now);
        
    private:
        Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager);

    private:
        bool _m_recurring = false; //是否循环->意思是如果是循环的，定时器超时之后会自动再次通过当前时间+_m_ms超时时间得到绝对超时时间并插入到管理器的时间堆中；如果不循环，那就只触发一次
        uint64_t _m_ms = 0; //超时时间--相对当前时间后的ms秒
        std::chrono::time_point<std::chrono::system_clock> _m_next; //绝对超时时间，即该定时器下次触发的时间点
        std::function<void()> _m_cb; //超时触发的回调函数
        TimerManager* _m_manager = nullptr; //指向TimerManager的指针，用来管理定时器
    
    private:
        //用于比较两个Timer对象，依据是绝对超时时间，用于实现排序的比较函数
        struct Comparator
        {
            bool operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const;
        };
    };

    //定时器管理器
    class TimerManager
    {
        //设置友元类访问Timer的函数和成员变量
        friend class Timer;
    public:
        TimerManager();
        //注意析构函数需要virtual
        virtual ~TimerManager();
    
        //添加Timer
        //ms定时器执行间隔时间
        //cb定时器回调函数
        //recurring是否循环定时器
        std::shared_ptr<Timer> addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);

        //添加条件Timer
        //weak_cond条件
        //配合OnTimer使用，当条件weak_cond被触发（即为当前条件的对象存在）时，才会执行回调函数cb
        std::shared_ptr<Timer> addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring = false);
    
        //拿到堆中最近的超时时间，返回最近超时时间的绝对时间值与当前时间的差值（单位：毫秒）
        //如果有超时定时器，则返回0；暂时没有超时定时器，则返回差值；如果没有定时器，则返回无穷大
        uint64_t getNextTimer();

        //取出所有超时定时器的回调函数存储到cbs中 -- 定时器任务执行的途径
        void listExpiredCb(std::vector<std::function<void()>>& cbs);

        //判断时间堆中是否有定时器timer
        bool hasTimer();

    protected:
        //当一个最早的timer加入到堆中 -> 调用该函数
        virtual void onTimerInsertedAtFront() {};

        //添加timer
        void addTimer(std::shared_ptr<Timer> timer);

    protected:
        //当前系统时间改变时 -> 调用该函数
        bool detectClockRollover();

    private:
        std::shared_mutex _m_mutex; //互斥锁
        //时间堆，存储所有Timer对象，并排序
        std::set<std::shared_ptr<Timer>, Timer::Comparator> _m_timers;
        //_m_tickled是一个标志，用于指示是否需要在定时器插入到时间堆的前端时触发额外的处理操作，例如唤醒一个等待的线程或进行其他管理操作
        //在下次getNextTimer()执行前，onTimerInsertedAtFront()是否已经被触发了 -> 在此过程中onTimerInsertedAtFront()只执行一次，防止重复调用
        //_m_tickled 变量在定时器管理器TimerManager中，主要用来避免在同一个定时周期内重复触发“前端插入唤醒”操作，是一种性能优化和防止无效唤醒的机制。
        //想象一下这个没有 _m_tickled 的场景：
        //线程A向空的定时器堆插入一个10秒后触发的定时器，它成为最早到期项，于是调用 onTimerInsertedAtFront() 唤醒可能正在休眠的I/O调度器。
        //线程B紧接着插入一个5秒后触发的、更早的定时器。它同样发现自己是堆首，于是再次尝试唤醒I/O调度器。
        //如果I/O调度器已经被第一次唤醒但还未执行到 getNextTimer() 来重新计算等待时间，第二次唤醒可能就是不必要的，甚至可能导致“惊群效应”。
        bool _m_tickled = false;
        //上次  “检查系统时间是否回退”  的绝对时间
        std::chrono::time_point<std::chrono::system_clock> _m_previouseTimer;
    };
}