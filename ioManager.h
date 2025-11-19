#pragma once

#include "scheduler.h"
#include "timer.h"

namespace nsCoroutine
{
    //工作流程：
    //1、 注册事件 -> 2、等待事件 -> 3、事件触发调度回调 -> 4、注销事件回调后从epoll注销 -> 5、执行回调进入调度器中执行调度。
    class IOManager : public Scheduler, public TimerManager
    {
    public:
        //事件枚举
        enum Event
        {
            NONE = 0x0, //没有事件
            READ = 0x1, //读事件，READ == EPOLLIN == 0x1，对应epoll的EPOLLIN
            WRITE = 0x4, //写事件，WRITE == EPOLLOUT == 0x4，对应epoll的EPOLLOUT
        };
    
    private:
        //用于描述一个文件描述符fd的事件上下文
        //每一个socket fd都对应一个FdContext，包括fd的值，fd上的事件，以及fd的读写事件上下文
        struct FdContext
        {
            //描述一个具体事件的上下文，如读事件和写事件
            struct EventContext
            {
                //scheduler，关联的调度器
                Scheduler* scheduler = nullptr;
                //callback fiber，关联的回调线程（协程）
                std::shared_ptr<Fiber> fiber;
                //callback function，关联的回调函数（都会注册为协程对象）
                std::function<void()> cb;
            };
            //读事件上下文
            EventContext read;
            //写事件上下文
            EventContext write;
            //事件关联的fd值（句柄）
            int fd = 0;
            //当前注册的事件，可能是READ、WRITE、READ|WRITE，可以看成是位图
            Event events = NONE;
            //事件上下文的互斥锁
            std::mutex mutex;
            //根据事件类型获取相应的事件上下文（如读事件上下文或写事件上下文）
            EventContext& getEventContext(Event event);
            //重置事件上下文
            void resetEventContext(EventContext& ctx);
            //触发事件，根据事件类型调用对应上下文结构的调度器去调度协程或函数
            void triggerEvent(Event event);
        };

    public:
        //threads线程数量，use_caller是否将主线程或调度线程包含进行，name调度器的名字
        //允许设置线程数量、是否使用调度者线程以及名称
        IOManager(size_t threads = 1, bool use_caller = true, const std::string& name = "IOManager");
        ~IOManager();
        //事件管理方法
        //添加一个事件到文件描述符fd上，并关联一个回调函数cb
        int addEvent(int fd, Event event, std::function<void()> cb = nullptr);
        //删除文件描述符fd上的某个事件
        bool delEvent(int fd, Event event);
        //取消文件描述符上的某个事件，并触发其回调函数
        bool cancelEvent(int fd, Event event);
        //取消文件描述符fd上的所有事件，并触发所有回调函数
        bool cancelAll(int fd);
        //获取当前调度器对象
        static IOManager* GetThis();

    protected:
        //通知调度器有任务调度
        //写pipe让idle协程从epoll_wait中退出，待idle协程yield之后Scheduler::run就可以调度其他任务
        void tickle() override;
        //判断调度器是否可以停止
        //判断条件是Scheduler::stopping()外加IOManager的_m_pendingEventCount为0，表示没有IO事件可调度
        bool stopping() override;
        //实际是idle协程只负责收集所有已触发的fd的回调函数并将其加入调度器的任务队列，
        //真正的执行时机是idle协程退出后，调度器在下一轮调度时执行
        void idle() override;
        //因为Timer类的成员函数重写当有新的定时器插入到前面时的处理逻辑
        void onTimerInsertedAtFront() override;
        //调整文件描述符上下文数组的大小
        void contextResize(size_t size);

    private:
        int _m_epfd = 0; //用于epoll的文件描述符
        int _m_tickleFds[2]; //用于线程间通信的管道文件描述符，fd[0]是读端，fd[1]是写端
        std::atomic<size_t> _m_pendingEventCount = {0}; //待处理的事件数量
        std::shared_mutex _m_mutex; //读写锁
        std::vector<FdContext*> _m_fdContexts; //文件描述符上下文数组，用于存储每个文件描述符的FdContext
    };
}