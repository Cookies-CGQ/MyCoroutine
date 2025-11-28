#include <vector>
#include "scheduler.h"

static bool debug = false;

namespace nsCoroutine
{
    //用于保存当前线程的调度器对象
    static thread_local Scheduler* t_scheduler = nullptr;
    //返回调度器对象
    Scheduler* Scheduler::GetThis()
    {
        return t_scheduler;
    }
    //设置调度器对象
    void Scheduler::SetThis()
    {
        t_scheduler = this;
    }
    //构造函数
    //构造函数负责初始化调度器对象，设置线程数量、是否使用调用线程、调度器名称等参数
    //如果use_caller为true，即为主线程也要参与调度，所以要创建协程，主要原因是为了实现更高效的任务调度和管理
    Scheduler::Scheduler(size_t threads, bool use_caller, const std::string& name)
        :_m_useCaller(use_caller),_m_name(name)
    {
        //判断创建线程数量是否大于0，并且调度器对象上是否是空指针
        assert(threads > 0 && Scheduler::GetThis() == nullptr);
        //设置当前线程的调度器对象为当前对象
        Scheduler::SetThis();
        //设置当前线程的名称为调度器名称
        Thread::SetName(_m_name);
        //如果use_caller为true，即为主线程也要参与调度，所以要创建协程，主要原因是为了实现更高效的任务调度和管理
        if(use_caller)
        {
            //因为主线程也作为调度线程，所以需要创建的调度线程数量减1
            threads--;
            //创建主协程
            Fiber::GetThis();
            //创建调度协程--默认初始化为主协程，需要重新设置
            _m_schedulerFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));
            //设置调度协程
            Fiber::SetSchedulerFiber(_m_schedulerFiber.get());
            //获取主线程ID
            _m_rootThread = Thread::GetThreadId();
            //同时把主线程id加入到调度线程池里面
            _m_threadIds.push_back(_m_rootThread);
        }
        
        _m_threadCount = threads;
        if(debug)
        {
            std::cout << "Scheduler::Scheduler() success\n";
        }
    }

    //析构函数
    Scheduler::~Scheduler()
    {
        assert(stopping() == true);
        if(Scheduler::GetThis() == this)
        {
            //将其设置为nullptr防止悬空指针
            t_scheduler = nullptr;
        }
        if(debug)
        {
            std::cout << "Scheduler::~Scheduler() success\n";
        }
    }

    //start函数是启动调度器的核心方法之一，它负责初始化和启动调度器管理的所有调度线程。
    //需要注意这里执行完thread的线程创建之后，也就是执行了thread的run方法之后reset才会完成。所以此时完成创建的工作线程已经开始执行了Scheduler::run()
    void Scheduler::start()
    {
        std::lock_guard<std::mutex> lock(_m_mutex);
        if(_m_stopping)
        {
            std::cerr << "Scheduler is stopped" << std::endl;
            return;   
        }
        //确保刚启动时候没有残留的线程
        assert(_m_threads.empty());
        _m_threads.resize(_m_threadCount);
        //循环创建和启动_m_threadCount个调度线程
        for(size_t i = 0; i < _m_threadCount; i++)
        {
            _m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), _m_name + "_" + std::to_string(i)));
            _m_threadIds.push_back(_m_threads[i]->getId());
        }
        if(debug)
        {
            std::cout << "Scheduler::start() success\n";
        }
    }

    //作用：调取器的核心，负责从任务队列中取出任务并通过协程执行
    void Scheduler::run()
    {
        //获取当前线程的ID
        int thread_id = Thread::GetThreadId();
        if(debug)
        {
            std::cout << "Scheduler::run() starts in thread: " << thread_id << std::endl; 
        }

        //set_hook_enable(true); 

        //设置当前线程的调度器对象为当前对象
        Scheduler::SetThis();
        //运行在新创建的线程->需要创建主协程（如果不是主线程，创建主协程）
        if(thread_id != _m_rootThread)
        {
            //分配了线程的主协程和调度协程
            Fiber::GetThis();
        }
        //创建空闲协程，std::make_shared是C++11引入的一个函数，用于创建std::shared_ptr构造函数，std::make_shared更高效而且更安全，因为它在单个内存分配中同时分配了控制块和对象，避免了额外的内存分配和指针操作。
        //子协程
        std::shared_ptr<Fiber> idle_fiber = std::make_shared<Fiber>(std::bind(&Scheduler::idle,this));
        ScheduleTask task;

        while(true)
        {
            //取出任务
            task.reset();
            bool tickle_me = false; //是否需要唤醒其他线程

            {
                std::lock_guard<std::mutex> lock(_m_mutex);
                auto it = _m_tasks.begin();
                //1、遍历任务队列
                while(it != _m_tasks.end())
                {
                    //不能等于当前线程的ID，其目的是让其他线程也能执行
                    if(it->_thread != -1 && it->_thread != thread_id)
                    {
                        it++;
                        tickle_me = true; //说明整个任务是其他线程的，有其他线程需要唤醒
                        continue;
                    }
                    //2、取出任务
                    assert(it->_fiber || it->_cb);
                    task = *it;
                    _m_tasks.erase(it);
                    _m_activeThreadCount++;
                    //这里取到任务的线程就直接break所以并没有遍历到队尾
                    break;
                }
                //确保仍然存在未处理的任务
                tickle_me = tickle_me || (it != _m_tasks.end());
            }
            //这里虽然写了唤醒但是并没有具体的逻辑代码
            if(tickle_me)
            {
                tickle();
            }
            //3、执行任务 -- 如果调度对象是协程
            if(task._fiber)
            {
                //resume协程，resume返回时此时任务要么执行完了，要么半路yield了，总之任务完成了，活跃线程计数减一
                {
                    std::lock_guard<std::mutex> lock(task._fiber->_m_mutex);
                    if(task._fiber->getState() != Fiber::TERM)
                    {
                        task._fiber->resume();
                    }
                }
                //线程完成任务之后就不再处于活跃状态，而是进入空闲状态，因此需要将活跃线程计数减一
                _m_activeThreadCount--;
                task.reset();
            }
            //执行任务 -- 如果调度对象是函数
            else if(task._cb)
            {
                std::shared_ptr<Fiber> cb_fiber = std::make_shared<Fiber>(task._cb);

                {
                    std::lock_guard<std::mutex> lock(cb_fiber->_m_mutex);
                    cb_fiber->resume();
                }
                
                _m_activeThreadCount--;
                task.reset();
            }
            //4、没有任务，执行空闲协程
            else
            {
                //系统关闭 -> idle协程将从死循环跳出并结束 -> 此时的idle协程状态为TERM -> 再次进入将跳出并退出run()
                if(idle_fiber->getState() == Fiber::TERM)
                {
                    //如果调度器没有调度任务，那么idle协程会不断得resume/yield。不会结束进入一个忙等待，如果idle协程结束了，一定是调度器停止了，直到任务才执行上面的if/else，在这里idle_fiber就是不断和主协程进行交互的子协程
                    if(debug)
                    {
                        std::cout << "Scheduler::run() exits in thread: " << thread_id << std::endl; 
                    }
                    break;
                }
                //没有任务，执行空闲协程
                _m_idleThreadCount++;
                idle_fiber->resume();
                _m_idleThreadCount--;
            }
        }
    }

    void Scheduler::stop()
    {
        if(debug)
        {
            std::cout << "Schdeule::stop() starts in thread: " << Thread::GetThreadId() << std::endl;            
        }
        
        if(stopping())
        {
            return;
        }
        
        _m_stopping = true;
        
        assert(GetThis() == this);    

        //调用tickle()的目的唤醒空闲线程或协程，防止_m_scheduler或其他线程处于永远阻塞在等待任务的状态中
        for(size_t i = 0; i < _m_threadCount; i++)
        {
            tickle(); //唤醒空闲线程
        }

        //唤醒可能处于挂起状态，等待下一个任务的调度的协程
        if(_m_schedulerFiber)
        {
            tickle();
        }

        //当只有主线程/调度线程作为工作线程的情况，只能从stop()方法开始任务调度
        if(_m_schedulerFiber)
        {
            //开始任务调度
            _m_schedulerFiber->resume();
            if(debug)
            {
                std::cout << "_m_schedulerFiber ends in thread: " << Thread::GetThreadId() << std::endl;
            }
        }
        //获取此时的线程通过swap不会增加引用计数的方式加入到thrs，方便下面的join保持线程正常退出
        std::vector<std::shared_ptr<Thread>> thrs;

        {
            std::lock_guard<std::mutex> lock(_m_mutex);
            thrs.swap(_m_threads);
        }

        for(auto &i : thrs)
        {
            i->join();
        }
        if(debug)
        {
            std::cout << "Scheduler::stop() ends in thread: " << Thread::GetThreadId() << std::endl;
        }
    }

    void Scheduler::tickle()
    {

    }

    void Scheduler::idle()
    {
        while(!stopping())
        {
            if(debug)
            {
                std::cout << "Scheduler::idle()，sleeping in thread: " << Thread::GetThreadId() << std::endl;
                // 写错了，如果在这里才yield，空闲协程就死循环了，调度线程就会一直在这个循环中，
                // 这样就跳不出到调度协程了，这样就无法调度任务了
                // 如果主线程/调度器线程参与调度，最起码还有一个线程可以执行
                // 如果主线程/调度器线程不参与调度，那么就会一直卡住无法消化任务
                // sleep(1);
                // Fiber::GetThis()->yield();
            }
            sleep(1);
            Fiber::GetThis()->yield();
        }
    }

    bool Scheduler::stopping()
    {
        std::lock_guard<std::mutex> lock(_m_mutex);
        return _m_stopping && _m_tasks.empty() && _m_activeThreadCount == 0;
    }
}