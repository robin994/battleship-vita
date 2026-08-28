#include "VitaNetworkPlatform.h"

#include "../port_log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef __vita__
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/netcheck_dialog.h>
#include <psp2/pspnet_adhoc.h>
#include <psp2/pspnet_adhocctl.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>
#endif

namespace ssb64::netplay::platform {
namespace {

#ifdef __vita__
constexpr std::size_t kNetMemoryBytes = 1024U * 1024U;
alignas(64) std::array<unsigned char, kNetMemoryBytes> sNetMemory{};
bool sNetOwned = false;
bool sNetCtlOwned = false;
bool sModuleLoaded = false;
bool sAdhocModuleOwned = false;
bool sAdhocOwned = false;
bool sAdhocCtlOwned = false;
bool sAppUtilModuleOwned = false;
bool sAppUtilOwned = false;
bool sCommonDialogConfigured = false;
SceNetEtherAddr sAdhocMac{};
SceNetAdhocctlGroupName sAdhocGroupName{};
#endif
bool sInitialized = false;
bool sAdhocInitialized = false;
std::atomic<AdhocConnectionState> sAdhocConnectionState{AdhocConnectionState::Inactive};
std::atomic<int> sAdhocDialogError{0};

#ifdef __vita__
std::string FormatMac(const SceNetEtherAddr& mac) {
    char text[18]{};
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac.data[0], mac.data[1], mac.data[2], mac.data[3], mac.data[4], mac.data[5]);
    return text;
}

int EnsureCommonDialogRuntime() {
    if (!sAppUtilOwned) {
        const bool appUtilLoaded = sceSysmoduleIsLoaded(SCE_SYSMODULE_APPUTIL) >= 0;
        if (!appUtilLoaded) {
            const int moduleResult = sceSysmoduleLoadModule(SCE_SYSMODULE_APPUTIL);
            if (moduleResult < 0) {
                port_log("[NETPLAY] AppUtil sysmodule load failed code=0x%08X\n",
                         static_cast<unsigned int>(moduleResult));
                return moduleResult;
            }
            sAppUtilModuleOwned = true;
        }

        SceAppUtilInitParam initParam{};
        SceAppUtilBootParam bootParam{};
        const int appUtilResult = sceAppUtilInit(&initParam, &bootParam);
        if (appUtilResult < 0) {
            port_log("[NETPLAY] sceAppUtilInit failed code=0x%08X\n",
                     static_cast<unsigned int>(appUtilResult));
            if (sAppUtilModuleOwned) {
                sceSysmoduleUnloadModule(SCE_SYSMODULE_APPUTIL);
                sAppUtilModuleOwned = false;
            }
            return appUtilResult;
        }
        sAppUtilOwned = true;
        port_log("[NETPLAY] AppUtil runtime initialized\n");
    }

    if (!sCommonDialogConfigured) {
        SceCommonDialogConfigParam config{};
        sceCommonDialogConfigParamInit(&config);

        int language = 0;
        int enterButton = 0;
        const int languageResult = sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &language);
        const int enterResult = sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enterButton);
        if (languageResult >= 0) config.language = static_cast<decltype(config.language)>(language);
        if (enterResult >= 0) config.enterButtonAssign = static_cast<decltype(config.enterButtonAssign)>(enterButton);

        const int configResult = sceCommonDialogSetConfigParam(&config);
        if (configResult < 0) {
            port_log("[NETPLAY] sceCommonDialogSetConfigParam failed code=0x%08X lang=0x%08X enter=0x%08X\n",
                     static_cast<unsigned int>(configResult),
                     static_cast<unsigned int>(languageResult),
                     static_cast<unsigned int>(enterResult));
            return configResult;
        }
        sCommonDialogConfigured = true;
        port_log("[NETPLAY] CommonDialog runtime configured language=%d enter=%d\n",
                 language, enterButton);
    }
    return 0;
}

#endif

NetworkPlatformStatus MakeStatus(int errorCode = 0) {
    NetworkPlatformStatus status{};
    status.initialized = sInitialized;
    status.adhocInitialized = sAdhocInitialized;
    status.errorCode = errorCode;

#ifdef __vita__
    if (!sInitialized) {
        return status;
    }

    int state = SCE_NETCTL_STATE_DISCONNECTED;
    const int stateResult = sceNetCtlInetGetState(&state);
    if (stateResult < 0) {
        status.errorCode = stateResult;
        return status;
    }
    status.connected = state == SCE_NETCTL_STATE_CONNECTED;

    if (status.connected) {
        SceNetCtlInfo info{};
        const int infoResult = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);
        if (infoResult >= 0) {
            info.ip_address[sizeof(info.ip_address) - 1] = '\0';
            status.localIp = info.ip_address;
        } else {
            status.errorCode = infoResult;
        }
    }
    if (sAdhocInitialized) {
        status.localMac = FormatMac(sAdhocMac);
    }
#else
    // Desktop/loopback tests use the platform-independent BSD transport and do
    // not require Vita's network module lifecycle.
    status.connected = sInitialized;
    status.localIp = "127.0.0.1";
    status.adhocInitialized = sAdhocInitialized;
    if (sAdhocInitialized) status.localMac = "02:00:00:00:00:01";
#endif
    return status;
}

} // namespace

NetworkPlatformStatus Initialize() {
    if (sInitialized) {
        return MakeStatus();
    }

#ifdef __vita__
    const int moduleResult = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (moduleResult < 0) {
        port_log("[NETPLAY] NET sysmodule init failed code=0x%08X\n",
                 static_cast<unsigned int>(moduleResult));
        return MakeStatus(moduleResult);
    }
    sModuleLoaded = true;

    SceNetInitParam initParam{};
    initParam.memory = sNetMemory.data();
    initParam.size = static_cast<int>(sNetMemory.size());
    initParam.flags = 0;

    const int netResult = sceNetInit(&initParam);
    if (netResult < 0) {
        // BattleShip currently has no other owner of sceNetInit. If that ever
        // changes, centralize ownership rather than silently terminating a
        // network stack another subsystem created.
        port_log("[NETPLAY] sceNetInit failed code=0x%08X\n",
                 static_cast<unsigned int>(netResult));
        if (sModuleLoaded) {
            sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
            sModuleLoaded = false;
        }
        return MakeStatus(netResult);
    }
    sNetOwned = true;

    const int ctlResult = sceNetCtlInit();
    if (ctlResult < 0) {
        port_log("[NETPLAY] sceNetCtlInit failed code=0x%08X\n",
                 static_cast<unsigned int>(ctlResult));
        sceNetTerm();
        sNetOwned = false;
        if (sModuleLoaded) {
            sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
            sModuleLoaded = false;
        }
        return MakeStatus(ctlResult);
    }
    sNetCtlOwned = true;
#endif

    sInitialized = true;
    NetworkPlatformStatus status = MakeStatus();
    port_log("[NETPLAY] network platform initialized connected=%d ip=%s\n",
             status.connected ? 1 : 0,
             status.localIp.empty() ? "unavailable" : status.localIp.c_str());
    return status;
}

NetworkPlatformStatus Query() {
    return MakeStatus();
}

NetworkPlatformStatus InitializeAdhoc() {
    NetworkPlatformStatus base = Initialize();
    if (!base.initialized || sAdhocInitialized) {
        return MakeStatus(base.errorCode);
    }

#ifdef __vita__
    const bool adhocModuleAlreadyLoaded =
        sceSysmoduleIsLoaded(SCE_SYSMODULE_PSPNET_ADHOC) >= 0;
    if (!adhocModuleAlreadyLoaded) {
        port_log("[NETPLAY] loading PSPNet AdHoc sysmodule\n");
        const int moduleResult = sceSysmoduleLoadModule(SCE_SYSMODULE_PSPNET_ADHOC);
        if (moduleResult < 0) {
            port_log("[NETPLAY] PSPNet AdHoc sysmodule load failed code=0x%08X\n",
                     static_cast<unsigned int>(moduleResult));
            return MakeStatus(moduleResult);
        }
        sAdhocModuleOwned = true;
    }
    port_log("[NETPLAY] PSPNet AdHoc sysmodule ready owned=%d\n",
             sAdhocModuleOwned ? 1 : 0);

    port_log("[NETPLAY] calling sceNetAdhocInit\n");
    int result = sceNetAdhocInit();
    if (result < 0 && result != SCE_ERROR_NET_ADHOC_ALREADY_INITIALIZED) {
        port_log("[NETPLAY] sceNetAdhocInit failed code=0x%08X\n",
                 static_cast<unsigned int>(result));
        if (sAdhocModuleOwned) {
            sceSysmoduleUnloadModule(SCE_SYSMODULE_PSPNET_ADHOC);
            sAdhocModuleOwned = false;
        }
        return MakeStatus(result);
    }
    sAdhocOwned = result >= 0;

    SceNetAdhocctlAdhocId adhocId{};
    // PSP AdHoc on Vita is negotiated through NetCheckDialog. vitaQuake uses
    // the RESERVED type for this compatibility stack; use the same contract
    // so the AdHoc ID also matches the dialog's NP communication ID.
    adhocId.type = SCE_NET_ADHOCCTL_ADHOCTYPE_RESERVED;
    static constexpr char kProductId[] = "SSB64VITA"; // exactly 9 bytes
    std::memcpy(adhocId.data, kProductId, SCE_NET_ADHOCCTL_ADHOCID_LEN);

    result = sceNetAdhocctlInit(&adhocId);
    if (result < 0 && result != SCE_ERROR_NET_ADHOCCTL_ALREADY_INITIALIZED) {
        port_log("[NETPLAY] sceNetAdhocctlInit failed code=0x%08X\n",
                 static_cast<unsigned int>(result));
        if (sAdhocOwned) {
            sceNetAdhocTerm();
            sAdhocOwned = false;
        }
        if (sAdhocModuleOwned) {
            sceSysmoduleUnloadModule(SCE_SYSMODULE_PSPNET_ADHOC);
            sAdhocModuleOwned = false;
        }
        return MakeStatus(result);
    }
    sAdhocCtlOwned = result >= 0;

    result = sceNetAdhocctlGetEtherAddr(&sAdhocMac);
    if (result < 0) {
        port_log("[NETPLAY] sceNetAdhocctlGetEtherAddr failed code=0x%08X\n",
                 static_cast<unsigned int>(result));
        if (sAdhocCtlOwned) {
            sceNetAdhocctlTerm();
            sAdhocCtlOwned = false;
        }
        if (sAdhocOwned) {
            sceNetAdhocTerm();
            sAdhocOwned = false;
        }
        if (sAdhocModuleOwned) {
            sceSysmoduleUnloadModule(SCE_SYSMODULE_PSPNET_ADHOC);
            sAdhocModuleOwned = false;
        }
        return MakeStatus(result);
    }
#endif

    sAdhocInitialized = true;
    // The native PSP AdHoc transport is ready as soon as adhoc + adhocctl are
    // initialized and the local MAC is available. vitaQuake's AdHoc backend
    // opens PDP sockets directly from this state; NetCheckDialog is not a
    // prerequisite for PDP/PTP traffic and is not available in every runtime.
    sAdhocConnectionState.store(AdhocConnectionState::Connected, std::memory_order_release);
    sAdhocDialogError.store(0, std::memory_order_release);
    NetworkPlatformStatus status = MakeStatus();
    port_log("[NETPLAY] AdHoc platform initialized mac=%s\n",
             status.localMac.empty() ? "unavailable" : status.localMac.c_str());
    return status;
}

void PrepareAdhocConnectionDialog() {
    if (!sAdhocInitialized) return;
    const AdhocConnectionState state = sAdhocConnectionState.load(std::memory_order_acquire);
    if (state == AdhocConnectionState::Inactive || state == AdhocConnectionState::Canceled ||
        state == AdhocConnectionState::Error) {
        sAdhocDialogError.store(0, std::memory_order_release);
        sAdhocConnectionState.store(AdhocConnectionState::AwaitingDialog, std::memory_order_release);
    }
}

bool BeginAdhocConnectionDialog() {
    if (!sAdhocInitialized ||
        sAdhocConnectionState.load(std::memory_order_acquire) != AdhocConnectionState::AwaitingDialog) {
        return false;
    }

#ifdef __vita__
    const int runtimeResult = EnsureCommonDialogRuntime();
    if (runtimeResult < 0) {
        sAdhocDialogError.store(runtimeResult, std::memory_order_release);
        sAdhocConnectionState.store(AdhocConnectionState::Error, std::memory_order_release);
        return false;
    }

    SceNetCheckDialogParam param;
    sceNetCheckDialogParamInit(&param);
    std::memset(&sAdhocGroupName, 0, sizeof(sAdhocGroupName));
    param.groupName = &sAdhocGroupName;
    static constexpr char kProductId[] = "SSB64VITA";
    std::memcpy(param.npCommunicationId.data, kProductId, sizeof(param.npCommunicationId.data));
    param.npCommunicationId.term = '\0';
    param.npCommunicationId.num = 0;
    param.mode = SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_CONN;
    param.timeoutUs = 0;

    const int result = sceNetCheckDialogInit(&param);
    if (result < 0) {
        sAdhocDialogError.store(result, std::memory_order_release);
        sAdhocConnectionState.store(AdhocConnectionState::Error, std::memory_order_release);
        port_log("[NETPLAY] NetCheck PSP AdHoc dialog init failed code=0x%08X\n",
                 static_cast<unsigned int>(result));
        return false;
    }
    port_log("[NETPLAY] NetCheck PSP AdHoc dialog started id=SSB64VITA\n");
#else
    port_log("[NETPLAY] desktop AdHoc connection emulation ready\n");
#endif

    sAdhocConnectionState.store(
#ifdef __vita__
        AdhocConnectionState::Running,
#else
        AdhocConnectionState::Connected,
#endif
        std::memory_order_release);
    return true;
}

void UpdateAdhocConnectionDialog() {
    if (sAdhocConnectionState.load(std::memory_order_acquire) != AdhocConnectionState::Running) return;

#ifdef __vita__
    const SceCommonDialogStatus status = sceNetCheckDialogGetStatus();
    // Match Sony's common-dialog lifecycle used by vitaQuake: NONE can be
    // observed briefly around initialization, and only FINISHED is terminal.
    if (status != SCE_COMMON_DIALOG_STATUS_FINISHED) return;

    SceNetCheckDialogResult result{};
    const int getResult = sceNetCheckDialogGetResult(&result);
    const int termResult = sceNetCheckDialogTerm();
    if (getResult < 0) {
        sAdhocDialogError.store(getResult, std::memory_order_release);
        sAdhocConnectionState.store(AdhocConnectionState::Error, std::memory_order_release);
        port_log("[NETPLAY] NetCheck PSP AdHoc result failed code=0x%08X term=0x%08X\n",
                 static_cast<unsigned int>(getResult), static_cast<unsigned int>(termResult));
        return;
    }

    if (result.result != SCE_COMMON_DIALOG_RESULT_OK) {
        sAdhocConnectionState.store(AdhocConnectionState::Canceled, std::memory_order_release);
        port_log("[NETPLAY] NetCheck PSP AdHoc canceled result=%d term=0x%08X\n",
                 static_cast<int>(result.result), static_cast<unsigned int>(termResult));
        return;
    }

    SceNetAdhocctlParameter connection{};
    const int parameterResult = sceNetAdhocctlGetParameter(&connection);
    if (parameterResult >= 0) {
        char group[SCE_NET_ADHOCCTL_GROUPNAME_LEN + 1]{};
        std::memcpy(group, connection.groupName.data, SCE_NET_ADHOCCTL_GROUPNAME_LEN);
        port_log("[NETPLAY] NetCheck PSP AdHoc connected group=%s channel=%d bssid=%02X:%02X:%02X:%02X:%02X:%02X\n",
                 group[0] != '\0' ? group : "<auto>", connection.channel,
                 connection.bssid.data[0], connection.bssid.data[1], connection.bssid.data[2],
                 connection.bssid.data[3], connection.bssid.data[4], connection.bssid.data[5]);
    } else {
        port_log("[NETPLAY] NetCheck PSP AdHoc connected parameter_query=0x%08X\n",
                 static_cast<unsigned int>(parameterResult));
    }
#endif

    sAdhocDialogError.store(0, std::memory_order_release);
    sAdhocConnectionState.store(AdhocConnectionState::Connected, std::memory_order_release);
}

AdhocConnectionState GetAdhocConnectionState() {
    return sAdhocConnectionState.load(std::memory_order_acquire);
}

bool IsAdhocConnectionReady() {
    return GetAdhocConnectionState() == AdhocConnectionState::Connected;
}

bool IsCommonDialogActive() {
    return GetAdhocConnectionState() == AdhocConnectionState::Running;
}

void ShutdownAdhoc() {
    if (!sAdhocInitialized) return;
#ifdef __vita__
    if (sAdhocCtlOwned) {
        sceNetAdhocctlTerm();
        sAdhocCtlOwned = false;
    }
    if (sAdhocOwned) {
        sceNetAdhocTerm();
        sAdhocOwned = false;
    }
    if (sAdhocModuleOwned) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_PSPNET_ADHOC);
        sAdhocModuleOwned = false;
    }
    sAdhocMac = {};
    sAdhocGroupName = {};
#endif
    sAdhocInitialized = false;
    sAdhocDialogError.store(0, std::memory_order_release);
    sAdhocConnectionState.store(AdhocConnectionState::Inactive, std::memory_order_release);
    port_log("[NETPLAY] AdHoc platform shutdown\n");
}

void Shutdown() {
    if (!sInitialized) {
        return;
    }

#ifdef __vita__
    ShutdownAdhoc();
    if (sNetCtlOwned) {
        sceNetCtlTerm();
        sNetCtlOwned = false;
    }
    if (sNetOwned) {
        sceNetTerm();
        sNetOwned = false;
    }
    if (sModuleLoaded) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
        sModuleLoaded = false;
    }
    if (sAppUtilOwned) {
        sceAppUtilShutdown();
        sAppUtilOwned = false;
    }
    if (sAppUtilModuleOwned) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_APPUTIL);
        sAppUtilModuleOwned = false;
    }
    sCommonDialogConfigured = false;
#endif

#ifndef __vita__
    ShutdownAdhoc();
#endif

    sInitialized = false;
    port_log("[NETPLAY] network platform shutdown\n");
}

} // namespace ssb64::netplay::platform
