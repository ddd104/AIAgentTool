#include "Utils/ABTGameThread.h"
#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"

namespace ABTGameThread
{
    void RunSync(TFunction<void()> Fn)
    {
        if (IsInGameThread())
        {
            Fn();
            return;
        }

        FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool(true);
        AsyncTask(ENamedThreads::GameThread, [DoneEvent, Fn = MoveTemp(Fn)]() mutable
        {
            Fn();
            DoneEvent->Trigger();
        });
        DoneEvent->Wait();
        FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
    }
}
