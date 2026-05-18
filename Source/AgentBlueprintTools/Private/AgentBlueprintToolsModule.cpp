#include "AgentBlueprintToolsModule.h"
#include "Bridge/ABTLocalBridgeServer.h"

#define LOCTEXT_NAMESPACE "FAgentBlueprintToolsModule"

void FAgentBlueprintToolsModule::StartupModule()
{
    UE_LOG(LogTemp, Display, TEXT("FAgentBlueprintToolsModule::StartupModule called"));

#if WITH_EDITOR
    BridgeServer = MakeUnique<FABTLocalBridgeServer>();
    UE_LOG(LogTemp, Display, TEXT("FAgentBlueprintToolsModule::StartupModule calling FABTLocalBridgeServer::Start"));
    BridgeServer->Start();
    UE_LOG(LogTemp, Display, TEXT("FAgentBlueprintToolsModule::StartupModule finished FABTLocalBridgeServer::Start"));
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
