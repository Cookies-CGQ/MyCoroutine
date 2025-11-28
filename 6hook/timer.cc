#include "timer.h"

namespace nsCoroutine
{
    //取消一个定时器，删除该定时器的回调函数并将其从定时器管理器中移除
    bool Timer::cancel()
    {
        //管理器写互斥锁
        std::unique_lock<std::shared_mutex> write_lock(_m_manager->_m_mutex);
        //删除回调函数
        if(_m_cb == nullptr)
        {
            return false;
        }
        else
        {
            _m_cb = nullptr;
        }
        //从管理器中移除该定时器
        auto it = _m_manager->_m_timers.find(shared_from_this());
        if(it != _m_manager->_m_timers.end())
        {
            //删除定时器
            _m_manager->_m_timers.erase(it);
        }
        return true;
    }

    //刷新定时器超时时间，这个刷新操作会将定时器的下次触发延后
    //重新设置定时器，并且把定时器管理器里的删除并重新加入
    bool Timer::refresh()
    {
        std::unique_lock<std::shared_mutex> write_lock(_m_manager->_m_mutex);
        
        if(_m_cb == nullptr)
        {
            return false;
        }

        auto it = _m_manager->_m_timers.find(shared_from_this());
        //检查定时器是否存在
        if(it == _m_manager->_m_timers.end())
        {
            return false;
        }
        //删除当前定时器并更新超时时间
        _m_manager->_m_timers.erase(it);
        //std::chrono::system_clock::now()返回当前系统时间的标准方法，返回的时间是系统时间（绝对时间）
        _m_next = std::chrono::system_clock::now() + std::chrono::milliseconds(_m_ms);
        //添加新的定时器加入到定时器管理器中
        _m_manager->_m_timers.insert(shared_from_this());
        return true;
    }

    //重置定时器的超时任务，可以选择从当前时间或者上次超时时间开始计算超时时间
    bool Timer::reset(uint64_t ms, bool from_now)
    {
        //检查是否要重置
        if(ms == _m_ms && !from_now)
        {
            //代表不需要重置
            return true;
        }
        //如果不满足上面的条件，则需要重置，删除当前的定时器然后重新计算超时时间并重新插入定时器
        {
            std::unique_lock<std::shared_mutex> write_lock(_m_manager->_m_mutex);
            
            if(_m_cb == nullptr)
            {
                return false;
            }

            auto it = _m_manager->_m_timers.find(shared_from_this());
            if(it == _m_manager->_m_timers.end())
            {
                return false;
            }
            _m_manager->_m_timers.erase(it);
        }
        //重新设置
        auto start = from_now ? std::chrono::system_clock::now() : _m_next - std::chrono::milliseconds(_m_ms);
        _m_ms = ms;
        _m_next = start + std::chrono::milliseconds(_m_ms);
        _m_manager->addTimer(shared_from_this());
        return true;
    }

    //构造函数
    Timer::Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager)
        :_m_recurring(recurring), _m_ms(ms), _m_cb(cb), _m_manager(manager)
    {
        auto now = std::chrono::system_clock::now();    //当前时间
        _m_next = now + std::chrono::milliseconds(ms);  //计算绝对时间 = 当前时间now + _m_ms
    }

    bool Timer::Comparator::operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const
    {
        assert(lhs != nullptr && rhs != nullptr);
        return lhs->_m_next < rhs->_m_next;
    }

    //定时器管理器构造函数和析构函数
    TimerManager::TimerManager()
    {
        //初始化当前系统事件，为后续检查系统时间错误进行校对
        _m_previouseTimer = std::chrono::system_clock::now();
    }
    TimerManager::~TimerManager()
    {

    }

    //添加新的定时器到定时器管理器中，并在必要时唤醒管理中的线程，准确的来说是在ioscheduler类的阻塞中的epoll，以确保定时器能够及时触发后回调函数
    std::shared_ptr<Timer> TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring)
    {
        std::shared_ptr<Timer> timer(new Timer(ms, cb, recurring, this));
        addTimer(timer);
        return timer;
    }

    // lock + tickle()
    void TimerManager::addTimer(std::shared_ptr<Timer> timer)
    {
        //标识插入的是否是最早超时的定时器
        bool at_front = false;
        
        {
            std::unique_lock<std::shared_mutex> write_lock(_m_mutex);
            //将定时器插入到_m_timers集合中，由于_m_timers是一个std::set，插入时会自动按定时器的超时时间排序
            auto it = _m_timers.insert(timer).first; 
            //判断插入的定时器是否是集合超时时间中最早的定时器
            at_front = (it == _m_timers.begin()) && !_m_tickled;

            //只要有一个线程唤醒并运行getNextTime()，就只触发一次tickle事件
            if(at_front)
            {
                //标识有一个新的最早定时器被插入了。防止重复唤醒
                _m_tickled = true;
            }
        }

        if(at_front)
        {
            //唤醒
            //虚函数具体执行在ioscheduler
            onTimerInsertedAtFront();
        }
    }

    static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb)
    {
        //确保当前条件的对象仍然存在
        std::shared_ptr<void> tmp = weak_cond.lock();
        if(tmp)
        {
            cb();
        }
    }

    std::shared_ptr<Timer> TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring)
    {
        //将OnTimer的真正指向交给了第一个addtimer，然后创建timer对象
        return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
    }

    //获取下一次超时时间
    uint64_t TimerManager::getNextTimer()
    {
        //读锁
        std::shared_lock<std::shared_mutex> read_lock(_m_mutex);
        //设置为false的意义就在于能继续在addtimer重新触发插入定时器时如果是最早的超时定时器，能正常触发 at_fornt；
        _m_tickled = false;
        if(_m_timers.empty())
        {
            //返回最大值
            return ~0ull;
        }
        
        auto now = std::chrono::system_clock::now();
        //获取最小时间堆中的第一个超时定时器判断超时
        auto time = (*_m_timers.begin())->_m_next;

        //判断当前时间是否已经超过了下一个定时器的超时时间
        if(now >= time)
        {
            //已经有timer超时了
            return 0;
        }
        else
        {
            //计算从当前时间到下一个定时器超时时间的时间差，结果是一个std::chrono::milliseconds对象
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(time - now);
            //将时间差转换为毫秒，并返回这个值      
            return static_cast<uint64_t>(duration.count());
        }
    }

    //处理所有已经超时的定时器，并将它们的回调函数收到cbs向量中，同时这个函数还会处理定时器的循环逻辑
    //具体的流程先获取到当前的时间，用写锁限制了资源的访问，通过detectClockRollover判断有没有出现系统时间回滚的情况，如果此时时间堆不为空，
    //并且发生了rollover系统回退或者时间堆中第一个定时器小于等于当前时间，都需要对其从时间堆中移除，把cb函数对象（定时任务）交给cbs函数对象存储数组进行存储，
    //如果定时器是循环则需要重新设置定时器并添加，否则情况temp中的cb（定时任务）设置为nullptr就行了。
    void TimerManager::listExpiredCb(std::vector<std::function<void()>>& cbs)
    {
        auto now = std::chrono::system_clock::now();
        std::unique_lock<std::shared_mutex> write_lock(_m_mutex);
        //判断是否出现系统时间错误
        bool rollover = detectClockRollover();
        //回退->清理所有timer || 超时->清理超时timer，如果rollover为false就没发生系统时间回退
        while(!_m_timers.empty() && (rollover || (*_m_timers.begin())->_m_next <= now)) //如果时间回滚发生或者定时器的超时时间早于或等于当前时间，则需要处理这些定时器。为什么说早于或等于都要处理，因为超时时间都是基于now后的
        {
            std::shared_ptr<Timer> temp = *_m_timers.begin();
            _m_timers.erase(_m_timers.begin());

            cbs.push_back(temp->_m_cb);
            //如果定时器是循环的，_m_next属性设置为当前时间加上定时器的间隔(_m_ms)，然后重新插入到定时器集合中
            if(temp->_m_recurring)
            {
                //重新加入时间堆
                temp->_m_next = now + std::chrono::milliseconds(temp->_m_ms);
                _m_timers.insert(temp);
            }
            else
            {
                //清理cb
                temp->_m_cb = nullptr;
            }
        }
    }

    //检测系统时间是否发生了回滚（即时间是否倒退）
    bool TimerManager::detectClockRollover()
    {
        bool rollover = false;
        //注意：这里检测的是，是否倒退
        //当前时间now与上次记录的时间_m_previouseTimer进行比较，如果now小于_m_previouseTimer减去一个小时的时间量（60 * 60 * 1000毫秒）。
        //当前时间now小于这个时间值，说明系统时间回滚了，因此将rollover设置为true。
        auto now = std::chrono::system_clock::now();
        if(now < (_m_previouseTimer - std::chrono::milliseconds(60 * 60 * 1000)))
        {
            //系统时间回滚了
            rollover = true;
        }
        //每次检测都会更新_m_previouseTimer，无论是否发生回滚，以便下次检测时进行比较
        _m_previouseTimer = now;
        return rollover;
    }

    //检查超时时间堆是否为空
    bool TimerManager::hasTimer()
    {
        std::shared_lock<std::shared_mutex> read_lock(_m_mutex);
        return !_m_timers.empty();
    }
}