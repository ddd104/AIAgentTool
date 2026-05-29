#include "AgentBlueprintToolsModule.h"
#include "Bridge/ABTLocalBridgeServer.h"

#define LOCTEXT_NAMESPACE "FAgentBlueprintToolsModule"

void FAgentBlueprintToolsModule::StartupModule()
{
#if WITH_EDITOR
    if (IsRunningCommandlet())
    {
        return;
    }

    BridgeServer = MakeUnique<FABTLocalBridgeServer>();
    BridgeServer->Start();
#endif
}

void FAgentBlueprintToolsModule::ShutdownModule()
{
#if WITH_EDITOR
    if (BridgeServer)
    {
        BridgeServer->Stop();
        BridgeServer.Reset();
    }
#endif
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAgentBlueprintToolsModule, AgentBlueprintTools)
