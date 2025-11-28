#include "hook.h"
#include "ioManager.h"
#include "fdManager.h"
#include <iostream>
#include <dlfcn.h>
#include <cstdarg>
#include <cstring>

// 宏定义，用于声明所有需要hook的函数
// 配合 #define XX(name) name##_f = (name##_fun)dlsym(RTLD_NEXT, #name); 使用
#define HOOK_FUN(XX) \
    XX(sleep)        \
    XX(usleep)       \
    XX(nanosleep)    \
    XX(socket)       \
    XX(connect)      \
    XX(accept)       \
    XX(read)         \
    XX(readv)        \
    XX(recv)         \
    XX(recvfrom)     \
    XX(recvmsg)      \
    XX(write)        \
    XX(writev)       \
    XX(send)         \
    XX(sendto)       \
    XX(sendmsg)      \
    XX(close)        \
    XX(fcntl)        \
    XX(ioctl)        \
    XX(getsockopt)   \
    XX(setsockopt)

namespace nsCoroutine
{
    // 线程局部存储，判断这个线程是否启用hook
    static thread_local bool t_hook_enable = false;

    bool is_hook_enable()
    {
        return t_hook_enable;
    }

    void set_hook_enable(bool flag)
    {
        t_hook_enable = flag;
    }

    void hook_init()
    {
        // 用于标识，只能初始化一次
        static bool is_inited = false;
        if (is_inited)
        {
            return;
        }
        is_inited = true;

#define XX(name) name##_f = (name##_fun)dlsym(RTLD_NEXT, #name);
        HOOK_FUN(XX)
#undef XX
    }

    // 静态变量初始化将在main函数之前运行
    struct HookIniter
    {
        HookIniter()
        {
            hook_init();
        }
    };
    static HookIniter s_hook_initer;
}

// 用于跟踪定时器的状态。具体来说，它有一个cancelled成员变量，通常用于表示定时器是否已经被取消。
// 0表示未取消，非0表示已取消
struct timer_info
{
    int cancelled = 0;
};

// 用于读写函数的通用模板
template <typename OriginFun, typename... Args>
static ssize_t do_io(int fd, OriginFun fun, const char *hook_fun_name, uint32_t event, int timeout_so, Args &&...args) // 这里的&&是万能引用，用于完美转发参数
{
    // 如果全局钩子动能未启用，则直接调用原始的I/O函数
    if (!nsCoroutine::t_hook_enable)
    {
        // 完美转发参数，避免参数被复制
        return fun(fd, std::forward<Args>(args)...);
    }

    // 获取与文件描述符fd相关联的上下文ctx，如果上下文不存在，则直接调用原始的I/O函数
    std::shared_ptr<nsCoroutine::FdCtx> ctx = nsCoroutine::FdMgr::GetInstance()->get(fd);
    if (!ctx)
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 如果文件描述符已经关闭，设置errno为EBADF并返回-1
    if (ctx->isClosed())
    {
        errno = EBADF; // 表示文件描述符无效或已经关闭
        return -1;
    }

    // 如果文件描述符不是一个socket或者用户设置了非阻塞模式，则直接调用原始的I/O操作函数
    if (!ctx->isSocket() || ctx->getUserNonblock())
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 获取超时设置并初始化timer_info结构体，用于后续的超时管理和取消操作。
    uint64_t timeout = ctx->getTimeout(timeout_so);
    // 条件定时器的条件
    std::shared_ptr<timer_info> tinfo(new timer_info);

    // 调用原始的I/O操作函数，并处理超时情况；如果由于系统中断（EINTR）导致操作失败，函数会重试
retry:
    // 调用原始I/O操作函数
    ssize_t n = fun(fd, std::forward<Args>(args)...);

    // 由于系统中断（EINTR）导致操作失败，函数重试
    while (n == -1 && errno == EINTR)
    {
        n = fun(fd, std::forward<Args>(args)...);
    }

    // 如果I/O操作因为资源暂时不可用（EAGAIN）而失败，函数会添加一个事件监听器来等待资源可用；
    // 同时，如果有超时设置，还会启动一个条件计时器来取消事件
    if (n == -1 && errno == EAGAIN)
    {
        nsCoroutine::IOManager *iom = nsCoroutine::IOManager::GetThis();
        // timer
        std::shared_ptr<nsCoroutine::Timer> timer;
        std::weak_ptr<timer_info> winfo(tinfo);

        // 如果执行的read等函数在Fdmanager管理的Fdctx中fd设置了超时时间，就会走到这里，添加addconditionTimer事件
        if (timeout != (uint64_t)-1)
        {
            timer = iom->addConditionTimer(timeout, [winfo, fd, iom, event]()
             {
                auto t = winfo.lock();
                // 如果 timer_info 对象已被释放（!t），或者操作已被取消（t->cancelled 非 0），则直接返回。
                if(!t || t->cancelled) 
                {
                    return;
                }
                t->cancelled = ETIMEDOUT; //如果超时时间到达并且事件尚未被处理(即cancelled任然是0)；
                // 取消该文件描述符上的事件，并立即触发一次事件（即恢复被挂起的协程）
                iom->cancelEvent(fd, (nsCoroutine::IOManager::Event)(event)); 
            }, winfo);
        }

        // 这行代码的作用是将 fd（文件描述符）和 event（要监听的事件，如读或写事件）添加到 IOManager 中进行管理。IOManager 会监听这个文件描述符上的事件，当事件触发时，它会调度相应的协程来处理这个事件
        int rt = iom->addEvent(fd, (nsCoroutine::IOManager::Event)(event));
        if (rt == -1)
        {
            std::cout << hook_fun_name << " addEvent(" << fd << ", " << event << ")";
            // 如果 rt 为-1，说明 addEvent 失败。此时，会打印一条调试信息，并且因为添加事件失败所以要取消之前设置的定时器，避免误触发。
            if (timer)
            {
                timer->cancel();
            }
            return -1;
        }
        else
        {
            // 如果 addEvent 成功（rt 为 0），当前协程会调用 yield() 函数，将自己挂起，等待事件的触发。
            nsCoroutine::Fiber::GetThis()->yield();

            // 当协程被恢复时（例如，事件触发后），它会继续执行 yield() 之后的代码。
            // 如果之前设置了定时器（timer 不为 nullptr），则在事件处理完毕后取消该定时器。取消定时器的原因是，该定时器的唯一目的是在 I/O 操作超时时取消事件。如果事件已经正常处理完毕，那么定时器就不再需要了。
            if (timer)
            {
                timer->cancel();
            }
            // 接下来检查 tinfo->cancelled 是否等于 ETIMEDOUT。如果等于，说明该操作因超时而被取消，因此设置 errno 为 ETIMEDOUT 并返回 -1，表示操作失败。
            if (tinfo->cancelled == ETIMEDOUT)
            {
                errno = tinfo->cancelled;
                return -1;
            }
            // 如果没有超时，则跳转到 retry 标签，重新尝试这个操作。
            goto retry;
        }
    }
    return n;
}

extern "C"
{

// XX(sleep) -> sleep_fun sleep_f = nullptr;
#define XX(name) name##_fun name##_f = nullptr;
    HOOK_FUN(XX)
#undef XX

    // only use at task fiber
    unsigned int sleep(unsigned int seconds)
    {
        //如果hook没有启动，则调用原始的系统调用
        if (!nsCoroutine::t_hook_enable)
        {
            return sleep_f(seconds);
        }

        //获取当前正在执行的协程（Fiber），并将其保存到fiber变量中
        std::shared_ptr<nsCoroutine::Fiber> fiber = nsCoroutine::Fiber::GetThis();
        nsCoroutine::IOManager *iom = nsCoroutine::IOManager::GetThis();
        // 添加一个定时器，在指定的时间后触发一个回调函数
        // 这个回调函数会将当前协程（fiber）添加到 IOManager 的调度队列中，等待被调度执行
        iom->addTimer(seconds * 1000, [fiber, iom]()
                      { iom->scheduleLock(fiber, -1); });
        // 挂起当前协程，等待被调度执行
        fiber->yield();
        return 0;
    }

    int usleep(useconds_t usec)
    {
        if (!nsCoroutine::t_hook_enable)
        {
            return usleep_f(usec);
        }

        std::shared_ptr<nsCoroutine::Fiber> fiber = nsCoroutine::Fiber::GetThis();
        nsCoroutine::IOManager *iom = nsCoroutine::IOManager::GetThis();
        
        iom->addTimer(usec / 1000, [fiber, iom]()
                      { iom->scheduleLock(fiber); });

        fiber->yield();
        return 0;
    }

    int nanosleep(const struct timespec *req, struct timespec *rem)
    {
        if (!nsCoroutine::t_hook_enable)
        {
            return nanosleep_f(req, rem);
        }

        int timeout_ms = req->tv_sec * 1000 + req->tv_nsec / 1000 / 1000;

        std::shared_ptr<nsCoroutine::Fiber> fiber = nsCoroutine::Fiber::GetThis();
        nsCoroutine::IOManager *iom = nsCoroutine::IOManager::GetThis();

        iom->addTimer(timeout_ms, [fiber, iom]()
                      { iom->scheduleLock(fiber, -1); });

        fiber->yield();
        return 0;
    }

    int socket(int domain, int type, int protocol)
    {
        if (!nsCoroutine::t_hook_enable)
        {
            return socket_f(domain, type, protocol);
        }
        //如果钩子启用了，则通过调用原始的 socket 函数创建套接字，并将返回的文件描述符存储在 fd 变量中。
        int fd = socket_f(domain, type, protocol);
        // fd无效
        if (fd == -1)
        {
            std::cerr << "socket() failed:" << strerror(errno) << std::endl;
            return fd;
        }
        //如果socket创建成功会利用Fdmanager的文件描述符管理类来进行管理，判断是否在其管理的文件描述符中，如果不在扩展存储文件描述数组大小，并且利用FDctx进行初始化判断是不是套接字，是不是系统非阻塞模式。
        nsCoroutine::FdMgr::GetInstance()->get(fd, true);
        return fd;
    }

    // 系统调用没有这个函数，这里是用于在连接超时情况下处理非阻塞套接字连接的实现。它首先尝试使用钩子功能来捕获并管理连接请求的行为，然后使用IOManager和TImer来管理超时机制，可以发现具体的逻辑实现上和do_io类似。
    // 注意：如果没有启用hook或者不是一个套接字、用户启用了非阻塞。都去调用connect系统调用，因为connect_with_timeout本身就在connect系统调用基础上调用的。
    int connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen, uint64_t timeout_ms)
    {
        if (!nsCoroutine::t_hook_enable)
        {
            return connect_f(fd, addr, addrlen);
        }

        std::shared_ptr<nsCoroutine::FdCtx> ctx = nsCoroutine::FdMgr::GetInstance()->get(fd);
        if (!ctx || ctx->isClosed())
        {
            errno = EBADF;
            return -1;
        }

        if (!ctx->isSocket())
        {
            return connect_f(fd, addr, addrlen);
        }

        if (ctx->getUserNonblock())
        {

            return connect_f(fd, addr, addrlen);
        }

        // 尝试进行connect操作
        int n = connect_f(fd, addr, addrlen);
        if (n == 0) //说明连接成功，直接返回结果
        {
            return 0;
        }
        else if (n != -1 || errno != EINPROGRESS) //说明连接请求处于等待状态，直接返回结果
        {
            return n;
        }

        // 写事件就绪 -> 连接成功
        nsCoroutine::IOManager *iom = nsCoroutine::IOManager::GetThis();
        std::shared_ptr<nsCoroutine::Timer> timer;
        std::shared_ptr<timer_info> tinfo(new timer_info);
        std::weak_ptr<timer_info> winfo(tinfo);

        if (timeout_ms != (uint64_t)-1)
        {
            timer = iom->addConditionTimer(timeout_ms, [winfo, fd, iom]()
            {
                auto t = winfo.lock();
                if(!t || t->cancelled) 
                {
                    return;
                }
                t->cancelled = ETIMEDOUT;
                iom->cancelEvent(fd, nsCoroutine::IOManager::WRITE); 
            }, winfo);
        }

        int rt = iom->addEvent(fd, nsCoroutine::IOManager::WRITE);
        if (rt == 0)
        {
            nsCoroutine::Fiber::GetThis()->yield();

            if (timer)
            {
                timer->cancel();
            }

            if (tinfo->cancelled)
            {
                errno = tinfo->cancelled;
                return -1;
            }
        }
        else
        {
            if (timer)
            {
                timer->cancel();
            }
            std::cerr << "connect addEvent(" << fd << ", WRITE) error";
        }

        // 检查连接是否成功
        int error = 0;
        socklen_t len = sizeof(int);
        if (-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len)) //通过getsockopt获取连接状态，检查套接字实际错误状态，来判断是否成功或失败
        {
            return -1;
        }
        if (!error) //如果没有错误，返回0表示成功
        {
            return 0;
        }
        else //如果有错误，返回-1表示失败并设置errno为实际错误码
        {
            errno = error;
            return -1;
        }
    }

    // connect_with_timeout函数实际上是在原始connect系统调用基础上，增加了超时控制的逻辑。
    // 在超时时间为-1时，表示不启用超时功能，也就是不会调用addconditiontimer函数放入到超时时间堆中，等待超时唤醒tickle触发IOManager::idle函数中epoll，而是就只是监听这个事件，这个事件没到就一直阻塞直到成功或失败。
    static uint64_t s_connect_timeout = -1; //s_connect_timeout 是一个 static 变量，表示默认的连接超时时间，类型为 uint64_t，可以存储 64 位无符号整数。//-1 通常用于表示一个无效或未设置的值。由于它是无符号整数，-1 实际上会被解释为 UINT64_MAX，表示没有超时限制。
    int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
    {
        return connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
    }

    //用于处理套接字接收连接的操作，同时支持超时连接控制，和recv等函数一样使用了do_io的模板，实现了非阻塞accpet的操作，并且如果成功接收了一个新的连接，则将新的文件描述符fd添加到文件描述符管理器(FdManager)中进行跟踪管理。
    int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
    {
        int fd = do_io(sockfd, accept_f, "accept", nsCoroutine::IOManager::READ, SO_RCVTIMEO, addr, addrlen);
        if (fd >= 0)
        {
            nsCoroutine::FdMgr::GetInstance()->get(fd, true);
        }
        return fd;
    }

    ssize_t read(int fd, void *buf, size_t count)
    {
        return do_io(fd, read_f, "read", nsCoroutine::IOManager::READ, SO_RCVTIMEO, buf, count);
    }

    ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
    {
        return do_io(fd, readv_f, "readv", nsCoroutine::IOManager::READ, SO_RCVTIMEO, iov, iovcnt);
    }

    ssize_t recv(int sockfd, void *buf, size_t len, int flags)
    {
        return do_io(sockfd, recv_f, "recv", nsCoroutine::IOManager::READ, SO_RCVTIMEO, buf, len, flags);
    }

    ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
    {
        return do_io(sockfd, recvfrom_f, "recvfrom", nsCoroutine::IOManager::READ, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);
    }

    ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
    {
        return do_io(sockfd, recvmsg_f, "recvmsg", nsCoroutine::IOManager::READ, SO_RCVTIMEO, msg, flags);
    }

    ssize_t write(int fd, const void *buf, size_t count)
    {
        return do_io(fd, write_f, "write", nsCoroutine::IOManager::WRITE, SO_SNDTIMEO, buf, count);
    }

    ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
    {
        return do_io(fd, writev_f, "writev", nsCoroutine::IOManager::WRITE, SO_SNDTIMEO, iov, iovcnt);
    }

    ssize_t send(int sockfd, const void *buf, size_t len, int flags)
    {
        return do_io(sockfd, send_f, "send", nsCoroutine::IOManager::WRITE, SO_SNDTIMEO, buf, len, flags);
    }

    ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
    {
        return do_io(sockfd, sendto_f, "sendto", nsCoroutine::IOManager::WRITE, SO_SNDTIMEO, buf, len, flags, dest_addr, addrlen);
    }

    ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
    {
        return do_io(sockfd, sendmsg_f, "sendmsg", nsCoroutine::IOManager::WRITE, SO_SNDTIMEO, msg, flags);
    }

    // 将所有文件描述符的事件处理了调用IOManager的callAll函数将fd上的读写事件全部处理，最后从FdManger文件描述符管理中移除该fd。
    int close(int fd)
    {
        if (!nsCoroutine::t_hook_enable)
        {
            return close_f(fd);
        }

        std::shared_ptr<nsCoroutine::FdCtx> ctx = nsCoroutine::FdMgr::GetInstance()->get(fd);

        if (ctx)
        {
            auto iom = nsCoroutine::IOManager::GetThis();
            if (iom)
            {
                iom->cancelAll(fd);
            }
            // del fdctx
            nsCoroutine::FdMgr::GetInstance()->del(fd);
        }
        return close_f(fd);
    }

    int fcntl(int fd, int cmd, ... /* arg */)
    {
        va_list va; // to access a list of mutable parameters

        va_start(va, cmd);//使其指向第一个可变参数（在 cmd 之后的参数）。
        switch (cmd)
        {
        case F_SETFL://用于设置文件描述符的状态标志（例如，设置非阻塞模式）。
        {
            int arg = va_arg(va, int); // Access the next int argument
            va_end(va);
            std::shared_ptr<nsCoroutine::FdCtx> ctx = nsCoroutine::FdMgr::GetInstance()->get(fd);
            //如果ctx无效，或者文件描述符关闭不是一个套接字就调用原始调用
            if (!ctx || ctx->isClosed() || !ctx->isSocket())
            {
                return fcntl_f(fd, cmd, arg);
            }
            // 用户是否设定了非阻塞
            ctx->setUserNonblock(arg & O_NONBLOCK);
            // 最后是否阻塞根据系统设置决定
            if (ctx->getSysNonblock())
            {
                arg |= O_NONBLOCK; 
            }
            else
            {
                arg &= ~O_NONBLOCK;
            }
            return fcntl_f(fd, cmd, arg);
        }
        break;

        case F_GETFL:
        {
            va_end(va);
            int arg = fcntl_f(fd, cmd);//调用原始的 fcntl 函数获取文件描述符的当前状态标志。
            std::shared_ptr<nsCoroutine::FdCtx> ctx = nsCoroutine::FdMgr::GetInstance()->get(fd);
            //如果上下文无效、文件描述符已关闭或不是套接字，则直接返回状态标志。
            if (!ctx || ctx->isClosed() || !ctx->isSocket())
            {
                return arg;
            }
            // 这里是呈现给用户 显示的为用户设定的值
            // 但是底层还是根据系统设置决定的
            if (ctx->getUserNonblock())
            {
                return arg | O_NONBLOCK;
            }
            else
            {
                return arg & ~O_NONBLOCK;
            }
        }
        break;

        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
        {
            int arg = va_arg(va, int);//从va获取标志位
            va_end(va);//清理va
            return fcntl_f(fd, cmd, arg);//调用原始调用
        }
        break;

        case F_GETFD:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
        {
            va_end(va);//清理va变量
            return fcntl_f(fd, cmd);//返回原始调用的结果
        }
        break;

        case F_SETLK://设置文件锁，如果不能立即获得锁，则返回失败。
        case F_SETLKW://设置文件锁，且如果不能立即获得锁，则阻塞等待。
        case F_GETLK://获取文件锁的状态。如果文件描述符 fd 关联的文件已经被锁定，那么该命令会填充 flock 结构体，指示锁的状态。
        {
            //从可变参数列表中获取 struct flock* 类型的指针，这个指针指向一个 flock 结构体，包含锁定操作相关的信息（如锁的类型、偏移量、锁的长度等）。
            struct flock *arg = va_arg(va, struct flock *);
            va_end(va);
            return fcntl_f(fd, cmd, arg);
        }
        break;

        case F_GETOWN_EX://获取文件描述符 fd 所属的所有者信息。这通常用于与信号处理相关的操作，尤其是在异步 I/O 操作中。
        case F_SETOWN_EX://设置文件描述符 fd 的所有者信息。
        {
            struct f_owner_exlock *arg = va_arg(va, struct f_owner_exlock *);
            va_end(va);
            return fcntl_f(fd, cmd, arg);
        }
        break;

        default:
            va_end(va);
            return fcntl_f(fd, cmd);
        }
    }

    // 实际处理了文件描述符(fd)上的ioctl系统调用，并在特定条件下对FIONBIO(用于设置非阻塞模式)进行了特殊处理。
    int ioctl(int fd, unsigned long request, ...)
    {
        va_list va;//va持有处理可变参数的状态信息
        va_start(va, request);//给va初始化让它指向可变参数的第一个参数位置。
        void *arg = va_arg(va, void *);//将va的指向参数的以void*类型取出存放到arg中
        va_end(va);//用于结束对 va_list 变量的操作。清理va占用的资源

        if (FIONBIO == request)//用于设置非阻塞模式的命令
        {
            bool user_nonblock = !!*(int *)arg;//当前 ioctl 调用是为了设置或清除非阻塞模式。
            std::shared_ptr<nsCoroutine::FdCtx> ctx = nsCoroutine::FdMgr::GetInstance()->get(fd);
            //检查获取的上下文对象是否有效（即 ctx 是否为空）。如果上下文对象无效、文件描述符已关闭或不是一个套接字，则直接调用原始的 ioctl 函数，返回处理结果。
            if (!ctx || ctx->isClosed() || !ctx->isSocket())
            {
                return ioctl_f(fd, request, arg);
            }
            //如果上下文对象有效，调用其 setUserNonblock 方法，将非阻塞模式设置为 user_nonblock 指定的值。这将更新文件描述符的非阻塞状态。
            ctx->setUserNonblock(user_nonblock);
        }
        return ioctl_f(fd, request, arg);
    }

    int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
    {
        return getsockopt_f(sockfd, level, optname, optval, optlen);
    }

    int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
    {
        if (!nsCoroutine::t_hook_enable)
        {
            return setsockopt_f(sockfd, level, optname, optval, optlen);
        }

        if (level == SOL_SOCKET)
        {
            if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO)
            {
                std::shared_ptr<nsCoroutine::FdCtx> ctx = nsCoroutine::FdMgr::GetInstance()->get(sockfd);
                if (ctx)
                {
                    const timeval *v = (const timeval *)optval;
                    ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
                }
            }
        }
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }
}
