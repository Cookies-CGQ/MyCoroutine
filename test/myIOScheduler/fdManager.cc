#include "fdManager.h"
#include "hook.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nsCoroutine
{
    // 显示实例化，FdManager类有一个全局唯一的单例实例
    template class Singleton<FdManager>;

    // 初始化
    template <typename T>
    T *Singleton<T>::instance = nullptr;

    template <typename T>
    std::mutex Singleton<T>::mutex;

    FdCtx::FdCtx(int fd) : m_fd(fd)
    {
        init();
    }

    FdCtx::~FdCtx()
    {
    }

    bool FdCtx::init()
    {
        if (m_isInit)
        {
            return true;
        }

        struct stat statbuf;
        // fstat 函数用于获取与文件描述符 m_fd 关联的文件状态信息存放到 statbuf 中。如果 fstat() 返回 -1，表示文件描述符无效或出现错误。
        if (-1 == fstat(m_fd, &statbuf))
        {
            m_isInit = false;
            m_isSocket = false;
        }
        else
        {
            m_isInit = true;
            // S_ISSOCK(statbuf.st_mode) SISSOCK是一个宏，用于检查'st_mode'中的位，以确定文件是否是一个套接字(socket)。该宏定义在<sys/stat.h>头文件中。
            m_isSocket = S_ISSOCK(statbuf.st_mode);
        }

        // 如果是套接字就设置为非阻塞
        if (m_isSocket)
        {
            // 获取文件描述符的状态
            int flags = fcntl_f(m_fd, F_GETFL, 0);
            if (!(flags & O_NONBLOCK))
            {
                // 检查当前标志中是否设置了非阻塞标志，如果没有就设置
                fcntl_f(m_fd, F_SETFL, flags | O_NONBLOCK);
            }
            // hook 非阻塞设置成功
            m_sysNonblock = true;
        }
        else
        {
            // 如果不是套接字就没必要设置非阻塞
            m_sysNonblock = false;
        }

        return m_isInit;
    }

    //type指定超时类型的标志。可能的值包括 SO_RCVTIMEO 和 SO_SNDTIMEO，分别用于接收超时和发送超时。v代表设置的超时时间，单位是毫秒或者其他。
    void FdCtx::setTimeout(int type, uint64_t v)
    {
        if (type == SO_RCVTIMEO)
        {
            m_recvTimeout = v;
        }
        else if(type == SO_SNDTIMEO)
        {
            m_sendTimeout = v;
        }
        //type无效
        else
        {
            m_recvTimeout = -1;
            m_sendTimeout = -1;
            std::cout << "type error" << std::endl;
        }
    }

    uint64_t FdCtx::getTimeout(int type)
    {
        if (type == SO_RCVTIMEO)
        {
            return m_recvTimeout;
        }
        else if(type == SO_SNDTIMEO)
        {
            return m_sendTimeout;
        }
        //type无效
        else
        {
            return -1;
            std::cout << "type error" << std::endl;
        }
    }

    FdManager::FdManager()
    {
        m_datas.resize(64);
    }

    std::shared_ptr<FdCtx> FdManager::get(int fd, bool auto_create)
    {
        if (fd == -1)
        {
            return nullptr;
        }

        // 读锁
        std::shared_lock<std::shared_mutex> read_lock(m_mutex);
        if (m_datas.size() <= fd)
        {
            if (auto_create == false)
            {
                return nullptr;
            }
        }
        else
        {
            if (m_datas[fd] || !auto_create)
            {
                return m_datas[fd];
            }
        }
        read_lock.unlock();
        
        // 写锁
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);

        if (m_datas.size() <= fd)
        {
            m_datas.resize(fd * 1.5);
        }

        m_datas[fd] = std::make_shared<FdCtx>(fd);
        return m_datas[fd];
    }

    // 删除指定文件描述符的FdCtx对象
    void FdManager::del(int fd)
    {
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        if (m_datas.size() <= fd)
        {
            return;
        }
        // reset()用于是否std::shared_ptr所管理的对象，并将智能指针重新置为nullptr(即空指针)，如果此时执行reset的智能指针是最后一个，那么其对象会被销毁。
        // 智能指针share调用reset()减少其对对象的引用计数，当引用计数为0销毁对象。
        m_datas[fd].reset();
    }
}