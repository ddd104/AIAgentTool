#pragma once

#include "CoreMinimal.h"

namespace ABTGameThread
{
    void RunSync(TFunction<void()> Fn);
}
