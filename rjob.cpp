#include "rjob.h"
#include "rjob_platform.h"

namespace rjob
{
    const uint32 workerThreadCount = 8;
    const uint32 maxFibers = 512;
    const uint32 maxJobsPerQueue = 2048;
    const uint32 jobStackSize = 32 * 1024;
    const uint32 schedulerStackSize = 1 * 1024;

    struct Job
    {
        struct JobFiber* fiber;
        JobEntry entryFn;
        void* user;
    };

    struct RunQueue
    {
        Job jobs[maxJobsPerQueue];
    };

    struct WaitingJob
    {
        Job job;
        JobPriority priority;
        JobCounter waitCounter;
    };

    struct WaitQueue
    {
        WaitingJob jobs[maxJobsPerQueue * (uint32)JobPriority::PriorityCount];
    };

    struct JobFiber
    {
        Platform::Fiber fiber;
        Job runningJob;
    };

    struct JobSystem
    {
        RunQueue runQueues[JobPriority::PriorityCount];
        WaitQueue waitQueue;

        Platform::Thread workerThreads[workerThreadCount];
        Platform::Fiber schedulerFibers[workerThreadCount];
        JobFiber fiberPool[maxFibers];
        

        volatile bool running;
        volatile uint32 runningWorkers;
    };

    JobSystem* jobSystem;

    // Always access via GetCurrentThreadIdx because compilers can optimize TLS access in a thread.
    RJOB_THREADLOCAL uint32 gCurrentThreadIdx;

    void FiberThread(uintptr fiberIdx)
    {
        while(true)
        {
            JobFiber* fiber = &jobSystem->fiberPool[fiberIdx];
            Job* job = &fiber->runningJob;
            job->entryFn(job->user);
            Platform::SwitchToFiber(jobSystem->schedulerFibers[rjob::GetCurrentThreadIndex()]);
        }
    }

    // We do scheduling in it's own fiber so that in theory, jobs will be able to pick their stack size.

    void ScheduleWork(uintptr threadIdx)
    {
        while(jobSystem->running)
        {
            // Deque Best Job
            Job job;
            JobFiber* fiber = job.fiber;
            if(fiber == nullptr)
            {
                // Find free fiber.
            }
            fiber->runningJob = job;// Redundant copy?
            Platform::SwitchToFiber(fiber->fiber);
        }
    }

    void WorkerStartup(uintptr threadIdx)
    {
        gCurrentThreadIdx = threadIdx;
        Platform::AtomicIncrement32(&jobSystem->runningWorkers);
        jobSystem->schedulerFibers[threadIdx] = Platform::BeginFiber(ScheduleWork, threadIdx);
        Platform::AtomicDecrement32(&jobSystem->runningWorkers);
    }

    static_assert(sizeof(JobSystem) <= rjob::GetRequiredMemory(), "Not enough memory for JobSystem");

    void rjob::Initialize(void* memory)
    {
        jobSystem = (JobSystem*)memory;
        for(uint32 i = 0; i < maxFibers; i++)
            jobSystem->fiberPool[i].fiber = Platform::CreateFiber(FiberThread, i, jobStackSize);
    }

    void rjob::Deinitialize()
    {
        while(jobSystem->runningWorkers)
            Platform::Yield();

        for(uint32 i = 0; i < maxFibers; i++)
            Platform::DestroyFiber(jobSystem->fiberPool[i].fiber);
    }

    void rjob::Startup(bool consumeCurrentThread)
    {
        jobSystem->running = true;
        uint32 i = 0;
        if(consumeCurrentThread)
        {
            i++;
        }
        for(; i < workerThreadCount; i++)
        {
            uint32 coreMask = 0x01 << i;
            jobSystem->workerThreads[i] = Platform::LaunchThread(WorkerStartup, i, schedulerStackSize, coreMask);
        }
        if(consumeCurrentThread)
        {
            jobSystem->workerThreads[0] = Platform::CurrentThread();
            Platform::SetThreadAffinity(jobSystem->workerThreads[0], 0x01);
            WorkerStartup(0);
        }
    }

    void rjob::Shutdown()
    {
        jobSystem->running = false;
    }

    uint32 rjob::GetCurrentThreadIndex()
    {
        return gCurrentThreadIdx;
    }
}
