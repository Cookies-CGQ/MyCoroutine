#include "fiber.h"

static bool debug = false;

namespace nsCoroutine
{
    //利用线程局部存储来保存当前线程上的协程控制信息

    //正在运行的协程
    static thread_local Fiber* t_fiber = nullptr;
    //主协程
    static thread_local std::shared_ptr<Fiber> t_thread_fiber = nullptr;
    //调度协程
    static thread_local Fiber* t_scheduler_fiber = nullptr;
    //全局协程ID计数器，()初始化与{}初始化，{}防止窄化类型
    static std::atomic<uint64_t> s_fiber_id{0};
    //s_fiber_count: 活跃协程数量计数器
    static std::atomic<uint64_t> s_fiber_count{0};
    //子协程栈默认大小
    const size_t DEFAULT_STACK_SIZE = 128 * 1024;

    //设置当前运行的协程
    void Fiber::SetThis(Fiber* f) 
    {
        t_fiber = f;
    }

    //首次运行该函数创建主协程
    std::shared_ptr<Fiber> Fiber::GetThis()
    {
        //如果当前线程有正在运行的协程，则直接返回
        if(t_fiber)
        {
            return t_fiber->shared_from_this(); //返回当前运行的协程
        }
        
        //如果当前线程没有正在运行的协程，则创建主协程--创建主协程使用Fiber()
        std::shared_ptr<Fiber> main_fiber(new Fiber());
        t_thread_fiber = main_fiber;
        t_scheduler_fiber = main_fiber.get(); //除非主动设置，要不然主协程默认是调度协程
        
        //判断t_fiber是否等于main_fiber，是就继续执行，否则程序终止
        assert(t_fiber == main_fiber.get());

        return t_fiber->shared_from_this();
    }

    //设置当前的调度协程
    void Fiber::SetSchedulerFiber(Fiber* f)
    {
        t_scheduler_fiber = f;
    }

    //获取当前运行的协程ID
    uint64_t Fiber::GetFiberId()
    {
        if(t_fiber)
        {
            return t_fiber->getId();
        }
        return (uint64_t)-1; //转换成UINT64_MAX，以便与其他类型比较
    }

    //作用：创建主协程，设置状态，初始化上下文，分配ID
    //在GetThis中使用了无参的Fiber来构造t_fiber
    Fiber::Fiber()
    {
        SetThis(this);
        //主协程刚创建就得设置为RUNNING状态，因为线程任何时刻都需要一个执行体--协程
        _m_state = RUNNING;

        if(getcontext(&_m_ctx))
        {
            std::cerr << "Fiber() failed\n";
            pthread_exit(nullptr);
        }
        _m_id = s_fiber_id++;
        s_fiber_count++;
        if(debug)
        {
            std::cout << "Fiber(): main id = " << _m_id << std::endl;
        }
    }

    //作用：创建子协程，初始化回调函数，栈的大小和状态。分配栈空间，并通过make修改上下文。
    //当set或者swap激活ucontext_t _m_ctx上下文时候会执行make第二个参数的函数
    Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler)
        :_m_cb(cb), _m_runInScheduler(run_in_scheduler)
    {
        _m_state = READY;

        //分配协程栈空间
        _m_stacksize = stacksize ? stacksize : DEFAULT_STACK_SIZE;
        _m_stack= malloc(_m_stacksize);

        if(getcontext(&_m_ctx))
        {
            std::cerr << "Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) failed\n";
            pthread_exit(nullptr);
        }

        //这里没有设置后继，是因为在运行完mainfunc后协程退出，会调用一次yield返回主协程
        _m_ctx.uc_link = nullptr;
        _m_ctx.uc_stack.ss_sp = _m_stack;
        _m_ctx.uc_stack.ss_size = _m_stacksize;
        makecontext(&_m_ctx, &Fiber::MainFunc, 0);

        _m_id = s_fiber_id++;
        s_fiber_count++;
        if(debug)
        {
            std::cout << "Fiber(): child id = " << _m_id << std::endl;
        }
    }

    Fiber::~Fiber()
    {
        s_fiber_count--;
        if(_m_stack)
        {
            free(_m_stack);
        }
        if(debug)
        {
            std::cout << "~Fiber(): id = " << _m_id << std::endl;
        }
    }

    //作用：重置协程的回调函数，并重新设置上下文，使用与将协程从TERM状态重置READY
    void Fiber::reset(std::function<void()> cb)
    {
        assert(_m_stack != nullptr && _m_state == TERM);

        _m_state = READY;
        _m_cb = cb;

        if(getcontext(&_m_ctx))
        {
            std::cerr << "reset() failed\n";
            pthread_exit(nullptr);
        }

        _m_ctx.uc_link = nullptr;
        _m_ctx.uc_stack.ss_sp = _m_stack;
        _m_ctx.uc_stack.ss_size = _m_stacksize;
        makecontext(&_m_ctx, &Fiber::MainFunc, 0);
    }

    void Fiber::resume()
    {
        assert(_m_state == READY);
        _m_state = RUNNING;
        //这里的切换就相当于非对称协程函数那个当a执行完后会将执行权交给b
        if(_m_runInScheduler)
        {
            //设置当前线程的工作协程
            SetThis(this);
            if(swapcontext(&(t_scheduler_fiber->_m_ctx), &_m_ctx))
            {
                std::cerr << "resume() to t_scheduler_fiber failed\n";
                pthread_exit(nullptr);
            }
        }
        else
        {
            SetThis(this);
            if(swapcontext(&(t_thread_fiber->_m_ctx), &_m_ctx))
            {
                std::cerr << "resume() to t_thread_fiber failed\n";
                pthread_exit(nullptr);
            }   
        }
    }

    void Fiber::yield()
    {
        assert(_m_state == RUNNING || _m_state == TERM);

        if(_m_state != TERM)
        {
            _m_state = READY;
        }

        if(_m_runInScheduler)
        {
            SetThis(t_scheduler_fiber);
            if(swapcontext(&_m_ctx, &(t_scheduler_fiber->_m_ctx)))
            {
                std::cerr << "yield() to t_scheduler_fiber failed\n";
                pthread_exit(nullptr);
            }
        }
        else
        {
            SetThis(t_thread_fiber.get());
            if(swapcontext(&_m_ctx, &(t_thread_fiber->_m_ctx)))
            {
                std::cerr << "yield() to t_thread_fiber failed\n";
                pthread_exit(nullptr);
            }
        }
    }

    void Fiber::MainFunc()
    {
        //GetThis()的shared_from_this()方法让引用计数加1
        std::shared_ptr<Fiber> curr = GetThis();
        assert(curr != nullptr);

        curr->_m_cb();
        //这里的一个细节就是，重置的cb回调函数就希望它指向nullptr，因为方便其他线程再次调用这个协程对象。
        curr->_m_cb = nullptr;
        curr->_m_state = TERM;

        //运行完毕 -> 让出执行权
        auto raw_ptr = curr.get();
        curr.reset(); //计数-1
        raw_ptr->yield();
    }
}