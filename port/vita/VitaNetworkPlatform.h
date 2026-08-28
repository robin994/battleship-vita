#pragma once

#include <string>

namespace ssb64::netplay::platform {

enum class AdhocConnectionState : int {
    Inactive = 0,
    AwaitingDialog,
    Running,
    Connected,
    Canceled,
    Error,
};

struct NetworkPlatformStatus {
    bool initialized = false;
    bool connected = false;
    bool adhocInitialized = false;
    int errorCode = 0;
    std::string localIp;
    std::string localMac;
};

// These calls are made only by NetworkManager's worker thread. They never run
// from the render/simulation path.
NetworkPlatformStatus Initialize();
NetworkPlatformStatus Query();
NetworkPlatformStatus InitializeAdhoc();
void PrepareAdhocConnectionDialog();
bool BeginAdhocConnectionDialog();
void UpdateAdhocConnectionDialog();
AdhocConnectionState GetAdhocConnectionState();
bool IsAdhocConnectionReady();
bool IsCommonDialogActive();
void ShutdownAdhoc();
void Shutdown();

} // namespace ssb64::netplay::platform
