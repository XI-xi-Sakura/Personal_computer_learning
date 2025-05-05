#pragma once
#include <iostream>
#include <semaphore.h>

namespace SemModule
{
    class Sem
    {
    public:
        Sem(int n)
        {
            sem_init(&_sem, 0, n);
        }

        void P()
        {
            sem_wait(&_sem);
        }
        void V()
        {
            sem_post(&_sem);
        }
        ~Sem()
        {
            sem_destroy(&_sem);
        }

    private:
        sem_t _sem;
    };
}