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
}