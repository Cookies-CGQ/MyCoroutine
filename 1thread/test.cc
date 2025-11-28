#include "thread.h"
#include <unistd.h>
using namespace nsCoroutine;

void test()
{
    Thread t1([](){
        std::cout << "Hello" << std::endl;
        std::cout << Thread::GetThreadId() << std::endl;
        std::cout << Thread::GetThis() << std::endl;
        std::cout << Thread::GetName() << std::endl;
        Thread::SetName("abc");
        std::cout << Thread::GetName() << std::endl;
    }, "Func of Hello");

    sleep(2);

    std::cout << t1.getId() << std::endl;
    std::cout << t1.getName() << std::endl;

    std::cout << "------------" << std::endl;

    std::cout << Thread::GetThreadId() << std::endl;
    std::cout << Thread::GetThis() << std::endl;
    std::cout << Thread::GetName() << std::endl;
    Thread::SetName("abc");
    std::cout << Thread::GetName() << std::endl;
    
    t1.join();
}

int main()
{
    test();

    return 0;
}