#pragma once

#include <memory>
#include <shared_mutex>
#include "thread.h"

namespace nsCoroutine
{
    // FdCtx类用于管理与文件描述符相关的状态和操作
    // FdCtx类在用户态记录了fd的读写超时和非阻塞信息，其中非阻塞包括用户显示设置的非阻塞和hook内部设置的非阻塞，区分这两种非阻塞可以有效应对用户对fd设置/获取NONBLOCK模式的情形。
    class FdCtx : public std::enable_shared_from_this<FdCtx>
    {
    private:
        bool m_isInit = false; //标记文件描述符是否已初始化
        bool m_isSocket = false; //标记文件描述符是否是一个套接字
        bool m_sysNonblock = false; //标记文件描述符是否设置为系统非阻塞模式
        bool m_userNonblock = false; //标记文件描述符是否设置为用户非阻塞模式 
        bool m_isClosed = false; //标记文件描述符是否已关闭
        int m_fd; //文件描述符

        // 读事件的超时时间，默认为-1表示没有超时限制
        uint64_t m_recvTimeout = (uint64_t)-1;
        // 写事件的超时时间，默认为-1表示没有超时限制
        uint64_t m_sendTimeout = (uint64_t)-1;

    public:
        FdCtx(int fd);
        ~FdCtx();

        //初始化FdCtx对象
        bool init();
        bool isInit() const { return m_isInit; }
        bool isSocket() const { return m_isSocket; }
        bool isClosed() const { return m_isClosed; }

        // 设置和获取用户层面的非阻塞状态
        void setUserNonblock(bool v) { m_userNonblock = v; }
        bool getUserNonblock() const { return m_userNonblock; }

        // 设置和获取系统层面的非阻塞状态
        void setSysNonblock(bool v) { m_sysNonblock = v; }
        bool getSysNonblock() const { return m_sysNonblock; }

        // 设置和获取超时时间，type用于区分读事件和写事件的超时设置，v表示时间毫秒。
        void setTimeout(int type, uint64_t v);
        uint64_t getTimeout(int type);
    };

    // 用于管理FdCtx对象的集合，提供了对文件描述符上下文的访问和管理功能
    class FdManager
    {
    public:
        FdManager();
        // 获取指定文件描述符的FdCtx对象，如果auto_create为true，在不存在的时候自动创建新的FdCtx对象
        std::shared_ptr<FdCtx> get(int fd, bool auto_create = false);
        // 删除指定文件描述符的FdCtx对象
        void del(int fd);

    private:
        //用于保护对m_datas的访问，支持共享读锁和独占写锁。
        std::shared_mutex m_mutex;
        //存储所有FdCtx对象的共享指针
        std::vector<std::shared_ptr<FdCtx>> m_datas;
    };

    // 实现单例模式，确保一个类只有一个实例，并提供全局访问点
    // 使用懒汉模式 + 互斥锁维持线程安全
    template <typename T>
    class Singleton
    {
    private:
        static T *instance; //对外提供的实例
        static std::mutex mutex; //锁

    protected:
        Singleton() {}

    public:
        // 删除拷贝构造函数和赋值运算符
        Singleton(const Singleton &) = delete;
        Singleton &operator=(const Singleton &) = delete;

        static T *GetInstance()
        {
            std::lock_guard<std::mutex> lock(mutex); // 确保线程安全
            // 这里还能锁优化
            if (instance == nullptr)
            {
                instance = new T();
            }
            return instance;
        }

        static void DestroyInstance()
        {
            std::lock_guard<std::mutex> lock(mutex);
            if(instance)
            {
                delete instance;
                instance = nullptr;
            }
        }
    };

    typedef Singleton<FdManager> FdMgr;

}