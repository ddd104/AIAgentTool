#pragma once

#include "Commandlets/Commandlet.h"
#include "ABTApplyPatchCommandlet.generated.h"

UCLASS()
class UABTApplyPatchCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UABTApplyPatchCommandlet();

    virtual int32 Main(const FString& Params) override;
};
