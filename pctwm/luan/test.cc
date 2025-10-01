
#include "model-assert.h"
#include "cds_atomic.h"
#include <thread>

atomic_int x;

void thread_0()
{

    x = 42;
    for (int i = 0; i < 2; i++)
    {
        x = 1;
    }
}

int main()
{
    x = 1;

    std::thread t0(thread_0);
    t0.join();
    MODEL_ASSERT(0);
    return 0;
}