#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>

#include "kern_start.hpp"

static void pluginStart() {
    SYSLOG(MODULE_SHORT, "loaded, doing nothing (milestone 0)");
}

static const char *bootargOff[] = {
    "-rbgpuoff"
};

static const char *bootargDebug[] = {
    "-rbgpudbg"
};

static const char *bootargBeta[] = {
    "-rbgpubeta"
};

PluginConfiguration ADDPR(config) = {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Monterey, // Darwin 21 (macOS 12)
    KernelVersion::Sequoia,  // Darwin 24 (macOS 15)
    pluginStart
};
