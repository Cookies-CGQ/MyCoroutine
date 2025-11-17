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
        bool _m_recurring = false; //是否循环
        uint64_t _m_ms = 0; //超时时间
        std::chrono::time_point<std::chrono::system_clock> _m_next; //绝对超时时间，即该定时器下次触发的时间点
        std::function<void()> _m_cb; //超时触发的回调函数
        TimerManager* _m_manager = nullptr; //指向TimerManager的指针，用来管理定时器
    
    private:
        //用于比较两个Timer对象，依据是绝对超时时间，用于实现最小堆的比较函数
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

        //添加条件timer
        //weak_cond条件
        std::shared_ptr<Timer> addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring = false);
    
        //拿到堆中最近的超时时间
        uint64_t getNextTimer();

        //取出所有超时定时器的超时时间
        void listExpiredCb(std::vector<std::function<void()>>& cbs);

        //堆中是否有定时器timer
        bool hasTimer();

    protected:
        //当一个最早的timer加入到堆中 -> 调用该函数
        virtual void onTimerInsertedAtFront();

        //添加timer
        void addTimer(std::shared_ptr<Timer> timer);

    protected:
        //当前系统时间改变时 -> 调用该函数
        bool detectClockRollover();

    private:
        std::shared_mutex _m_mutex; //互斥锁
        //时间堆，存储所有Timer对象，并排序
        std::set<std::shared_ptr<Timer>, Timer::Comparator> _m_timers;
        //在下次getNextTimer()执行前，onTimerInsertedAtFront()是否已经被触发了 -> 在此过程中onTimerInsertedAtFront()只执行一次，防止重发调用
        bool _m_tickled = false;
        //上次检查系统时间是否回退的绝对时间
        std::chrono::time_point<std::chrono::system_clock> _m_previouseTime;
    };
}