#pragma once

#include <cstdint>
#include <string>

namespace ssb64::netplay::platform {

enum class ImeState : int {
    Inactive = 0,
    Running,
    Accepted,
    Canceled,
    Error,
};

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

// System-keyboard (IME) text entry. Begin/Cancel are called from the menu task;
// Update must be pumped every frame while Running. State/Result are lock-free
// reads for the menu.
bool BeginImeDialog(const char* title, const char* initialText, uint32_t maxLength, bool numeric);
void UpdateImeDialog();
ImeState GetImeState();
bool GetImeResult(std::string& out);
void CancelImeDialog();

} // namespace ssb64::netplay::platform
