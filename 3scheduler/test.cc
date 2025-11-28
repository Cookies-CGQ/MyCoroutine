#include "scheduler.h"

using namespace nsCoroutine;

static unsigned int test_number;
std::mutex mutex_cout;

void task()
{
    {
        std::lock_guard<std::mutex> lock(mutex_cout);
        std::cout << "task " << test_number++ << " is under processing in thread: " << Thread::GetThreadId() << std::endl;
    }
    sleep(1);
}

int main()
{
    std::cout << "main thread: " << Thread::GetThreadId() << std::endl;
    {
        // 可以尝试把false 变为true 此时调度器所在线程也将加入工作线程
        std::shared_ptr<Scheduler> scheduler = std::make_shared<Scheduler>(3, true, "scheduler_1");

        scheduler->start();

        sleep(2);

        std::cout << "\nbegin post\n\n";
        for (int i = 0; i < 5; i++)
        {
            //调度对象是协程
            std::shared_ptr<Fiber> fiber = std::make_shared<Fiber>(task);
            scheduler->scheduleLock(fiber);
        }

        sleep(6);

        std::cout << "\npost again\n\n";
        for (int i = 0; i < 15; i++)
        {
            //调度对象是函数s
            scheduler->scheduleLock(task);
            scheduler->scheduleLock(&task);
        }

        sleep(3);
        // scheduler如果有设置将加入工作处理
        std::cout << "stop scheduler" << std::endl;
        scheduler->stop();
    }
    return 0;
}