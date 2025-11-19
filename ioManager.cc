#include <stdexcept>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>

#include "ioManager.h"

namespace nsCoroutine
{
    //获取当前线程的调度器对象，然后将其动态转换成IOManager*类型，
    //如果转换成功，表示当前线程的调度器对象确实是一个IOManager对象，
    //否则，如果是转化的是指针类型返回nullptr。引用类型抛出std::bad_cast异常
    IOManager* IOManager::GetThis()
    {
        //dynamic_cast 是 C++ 中用于在继承层次结构间进行安全类型转换的运算符，它在运行时检查转换的有效性。
        //如果转换成功，返回转换后的指针，否则返回 nullptr。
        return dynamic_cast<IOManager*>(Scheduler::GetThis());
    }

    //返回对应事件上下文的引用
    IOManager::FdContext::EventContext& IOManager::FdContext::getEventContext(Event event)
    {
        //判断事件是读事件，或者是写事件
        assert(event == READ || event == WRITE);
        switch(event)
        {
            case READ:
                return read;
            case WRITE:
                return write;
        }
        //std::invalid_argument异常表示传入的参数无效，一般是因为传入了非法的参数。
        throw std::invalid_argument("Unsupported event type");
    }

    //重置EventContext事件的上下文，将其恢复到初始或者空的状态，
    //主要作用是清理并重置传入的EvnetContext对象，使其不再与任何调度器、线程或回调函数相关联
    void IOManager::FdContext::resetEventContext(EventContext& ctx)
    {
        ctx.scheduler = nullptr;
        ctx.fiber.reset();
        ctx.cb = nullptr;
    }

    //负责在指定IO事件被触发，执行相应的回调函数或线程，并且在执行完之后情况相关的事件上下文
    //通过判断触发指定的event事件在Fdcontext中的events中存在对应的读或写或读写组合，
    //没有assert就抛出异常了，有就从取反从events中删除，
    //然后获取相应的EventContext具体的读或写事件对应的上下文将fd绑定的具体读或写任务的回调协程或回调函数，放入到任务队列中等待调度器调度。
    void IOManager::FdContext::triggerEvent(Event event)
    {
        //确保event是events中的有指定的事件，否则程序中断
        assert(events & event);
        //delete event
        //清理该事件，表示不再关注，也就是说，注册IO事件是一次性的
        //如果想持续关注某个Socket fd的读写事件，那么每次触发事件后都要重新添加
        events = (Event)(events & ~event); //清理该事件

        //trigge
        EventContext& ctx = getEventContext(event);
        //把真正要执行的函数放入到调度器的任务队列中等线程取出任务后，协程执行，执行完成后返回主协程继续，执行run方法取任务执行任务（不过可能是不同的线程的协程执行了）
        if(ctx.cb)
        {
            // call ScheduleTask(std::function<void()>* f, int thr)
            ctx.scheduler->scheduleLock(&ctx.cb);
        }
        else
        {
            // call ScheduleTask(std::shared_ptr<Fiber>* f, int thr)
            ctx.scheduler->scheduleLock(&ctx.fiber);
        }
        //清理事件上下文
        resetEventContext(ctx);
        return;
    }

    //IOManager的构造函数和析构函数
    IOManager::IOManager(size_t threads, bool use_caller, const std::string& name)
        :Scheduler::Scheduler(threads, use_caller, name),
         TimerManager::TimerManager()
    {
        //创建epoll句柄
        //实际上epoll_create的这个参数在现代Linux内核已经被忽略，传参只要大于0即可
        _m_epfd = epoll_create(5000);
        assert(_m_epfd > 0); //错误就终止程序，fd都是>=0的
        
        //创建管道的函数，规定了_m_tickleFds[0]是读端，_m_tickleFds[1]是写端
        int rt = pipe(_m_tickleFds);
        //rt == 0 表示成功创建管道
        assert(rt == 0); //创建管道失败就终止程序

        //将管道的监听注册到epoll上
        epoll_event event;
        event.events = EPOLLIN | EPOLLET; // 设置标记位，并且采用边缘触发和读事件
        event.data.fd = _m_tickleFds[0];

        //修改管道文件描述符以非阻塞的方式，配合边缘触发
        rt = fcntl(_m_tickleFds[0], F_SETFL, O_NONBLOCK);
        assert(rt == 0); //fcntl失败就终止程序

        //将_m_tickleFds[0]作为读事件放入到event监听集合中
        rt = epoll_ctl(_m_epfd, EPOLL_CTL_ADD, _m_tickleFds[0], &event);
        assert(rt == 0); //注册监听失败就终止程序

        //初始化一个包含32个文件描述符上下文的数组
        contextResize(32);

        //启动Scheduler，开启线程池，准备处理任务。
        start();
    }
    
    //析构函数
    IOManager::~IOManager()
    {
        //关闭scheduler类中的线程池，让任务全部执行完之后线程安全退出
        stop();
        //关闭相关fd
        close(_m_epfd);
        close(_m_tickleFds[0]);
        close(_m_tickleFds[1]);
        //将Fdcontext里的所有文件描述符一个个关闭
        for(size_t i = 0; i < _m_fdContexts.size(); i++)
        {
            if(_m_fdContexts[i])
            {
                //关闭文件描述符
                close(_m_fdContexts[i]->fd);
                delete _m_fdContexts[i];
            }
        }
    }

    //调整_m_fdContexts数组的大小，并为新的文件描述符(fd)创建并初始化相应的Fdcontext对象
    void IOManager::contextResize(size_t size)
    {
        //调整_m_fdContexts数组的大小
        _m_fdContexts.resize(size);
        //遍历_m_fdContexts数组，为新的文件描述符(fd)创建并初始化相应的Fdcontext对象
        for(size_t i = 0; i < _m_fdContexts.size(); i++)
        {
            if(_m_fdContexts[i] == nullptr)
            {
                _m_fdContexts[i] = new FdContext();
                _m_fdContexts[i]->fd = i; //将文件描述符的编号赋值给fd
            }
        }
    }

    //为上面contextResize函数分配好的fd，添加一个event事件，并在事件触发时执行指定的回调函数（cb）或回调协程具体的触发是在triggerEvent
    int IOManager::addEvent(int fd, Event event, std::function<void()> cb)
    {
        //查找FdContext对象
        FdContext* fd_ctx = nullptr;
        
        std::shared_lock<std::shared_mutex> read_lock(_m_mutex);
        if((int)_m_fdContexts.size() > fd) //如果说传入的fd在数组里面则查找然后初始化FdContext的对象
        {
            fd_ctx = _m_fdContexts[fd];
            read_lock.unlock();
        }
        else //不存在则重新分配数组的size来初始化FdContext对象
        {   
            read_lock.unlock();
            std::unique_lock<std::shared_mutex> write_lock(_m_mutex);
            contextResize(fd * 1.5);
            fd_ctx = _m_fdContexts[fd];
        }
        //一旦找到或者创建FdContext的对象后，加上互斥锁，确保FdContext的状态不会被其他线程修改
        std::lock_guard<std::mutex> lock(fd_ctx->mutex);

        //判断事件是否已经存在？是就返回-1，因为相同的事件不能重复添加
        if(fd_ctx->events & event)
        {
            return -1;
        }

        //如果已经存在就fd_ctx->events本身已经有读或写，那就是修改已经有的事件，如果不存在就是none事件的情况，就添加事件
        int op = fd_ctx->events == NONE ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
        epoll_event epevnet;
        epevnet.events = EPOLLET | fd_ctx->events | event;
        epevnet.data.ptr = fd_ctx; //ptr是epoll_event的指针，用于指向自定义类型
        //函数将事件添加到epoll中，如果添加失败，打印错误信息并返回-1
        int rt = epoll_ctl(_m_epfd, op, fd, &epevnet);
        if(rt)
        {
            std::cerr << "addEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
            return -1;
        }

        //原子计数器，待处理的事件i++；
        ++_m_pendingEventCount;

        //更新FdContext的events成员，记录当前的所有事件，注意events可以监听读和写的组合，
        //如果fd_ctx->events本身是none事件，则直接赋值是fd_ctx->events = event
        fd_ctx->events = (Event)(fd_ctx->events | event);

        //设置事件上下文
        FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
        //确保EventContext中没有其他正在执行的调度器、协程或回调函数
        assert(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);
        event_ctx.scheduler = Scheduler::GetThis(); //保存当前的调度器对象
        //如果提供了回到函数cb，则将其保存到EventContext中；否则，将当前正在运行的协程保存到EventContext中，并确保协程的状态是正在运行的
        if(cb)
        {
            event_ctx.cb.swap(cb);
        }
        else
        {
            event_ctx.fiber = Fiber::GetThis(); //需要确保存在主协程
            assert(event_ctx.fiber->getState() == Fiber::State::RUNNING);
        }

        return 0;
    }

    //从IOManager中删除某个文件描述符（fd）的特定事件（event）
    bool IOManager::delEvent(int fd, Event event)
    {
        //查找是否存在该文件描述符的FdContext对象
        FdContext* fd_ctx = nullptr;
        std::shared_lock<std::shared_mutex> read_lock(_m_mutex);
        if((int)_m_fdContexts.size() > fd)
        {
            fd_ctx = _m_fdContexts[fd];
            read_lock.unlock();
        }
        else
        {
            read_lock.unlock();
            return false;
        }
        //找到后添加互斥锁
        std::lock_guard<std::mutex> lock(fd_ctx->mutex);

        //判断事件是否存在，不存在就返回false
        if(!(fd_ctx->events & event))
        {
            return false;
        }

        Event new_events = (Event)(fd_ctx->events & ~event); //清理该事件
        int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL; //如果还有事件，则修改，否则删除
        epoll_event epevnet;
        epevnet.events = EPOLLET | new_events;
        epevnet.data.ptr = fd_ctx;
        
        int rt = epoll_ctl(_m_epfd, op, fd, &epevnet);
        if(rt)
        {
            std::cerr << "delEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
            return -1;
        }

        --_m_pendingEventCount; //待处理的事件i--；
        //更新FdContext的events成员，记录当前的所有事件
        fd_ctx->events = new_events;
        //清理事件上下文
        FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
        fd_ctx->resetEventContext(event_ctx);

        return true;
    }

    //取消特定文件描述符上的指定事件（如读事件或写事件），并触发该事件的回调函数
    //相比delEvent不同在于删除事件后，还需要将删除的事件直接交给trigger函数放入到协程调度器中进行触发
    bool IOManager::cancelEvent(int fd, Event event)
    {
        FdContext* fd_ctx = nullptr;
        std::shared_lock<std::shared_mutex> read_lock(_m_mutex);
        if((int)_m_fdContexts.size() > fd)
        {
            fd_ctx = _m_fdContexts[fd];
            read_lock.unlock();
        }
        else
        {
            read_lock.unlock();
            return false;
        }

        std::lock_guard<std::mutex> lock(fd_ctx->mutex);

        if(!(fd_ctx->events & event))
        {
            return false;
        }

        //删除事件
        Event new_events = (Event)(fd_ctx->events & ~event);
        int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        epoll_event epevnet;
        epevnet.events = EPOLLET | new_events;
        epevnet.data.ptr = fd_ctx;

        int rt = epoll_ctl(_m_epfd, op, fd, &epevnet);
        if(rt)
        {
            std::cerr << "cancelEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
            return -1;
        }
        
        --_m_pendingEventCount;

        fd_ctx->triggerEvent(event);
        
        return true;
    }

    //取消指定文件描述符上的所有事件，并且触发这些事件的回调
    //和cancle的不同，这个函数是完全将fd上的事件从epoll中移除，并且挨个得触发响应的读事件或写事件确保，事件都能被执行。
    bool IOManager::cancelAll(int fd)
    {
        FdContext* fd_ctx = nullptr;
        std::shared_lock<std::shared_mutex> read_lock(_m_mutex);
        if((int)_m_fdContexts.size() > fd)
        {
            fd_ctx = _m_fdContexts[fd];
            read_lock.unlock();
        }
        else
        {
            read_lock.unlock();
            return false;
        }

        std::lock_guard<std::mutex> lock(fd_ctx->mutex);

        if(!fd_ctx->events)
        {
            return false;
        }

        int op = EPOLL_CTL_DEL;
        epoll_event epevnet;
        epevnet.events = 0;
        epevnet.data.ptr = fd_ctx;

        int rt = epoll_ctl(_m_epfd, op, fd, &epevnet);
        if(rt)
        {
            std::cerr << "cancelAll::epoll_ctl failed: " << strerror(errno) << std::endl;
            return -1;
        }

        if(fd_ctx->events & READ)
        {
            fd_ctx->triggerEvent(READ);
            --_m_pendingEventCount;
        }

        if(fd_ctx->events & WRITE)
        {
            fd_ctx->triggerEvent(WRITE);
            --_m_pendingEventCount;
        }

        assert(fd_ctx->events == 0);
        
        return true;
    }

    //重写scheduler的tickle()
    //作用是检测到有空闲线程时，通过写入一个字符到管道中(_m_tickleFds[1])，唤醒那些等待任务的线程
    void IOManager::tickle()
    {
        if(!hasIdleThreads()) //在scheduler检查当前是否有线程处于空闲状态，如果没有空闲线程，函数直接返回，不执行后续操作
        {
            return;
        }
        //如果有空闲线程，函数会向管道_m_tickleFds[1]中写入一个字符"T"，这个写操作的目的是向等待在_m_tickleFds[0](管道另一端)的线程发送一个信号，通知它有新任务可以处理了。
        int rt = write(_m_tickleFds[1], "T", 1);
        assert(rt == 1);
    }

    //
}