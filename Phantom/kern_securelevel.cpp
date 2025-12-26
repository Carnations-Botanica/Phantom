//
//  kern_securelevel.cpp
//  Phantom
//
//  Created by RoyalGraphX on 6/5/25.
//

#include "kern_securelevel.hpp"

// Pointer to original declaration
sysctl_handler_t SLP::originalSecureLevelHandler = nullptr;

// Phantom's custom sysctl securelevel function, this one returns 1 always, to say yes we're enabled
int phtm_sysctl_securelevel(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req) {
	
    proc_t currentProcess = current_proc();
    pid_t procPid = proc_pid(currentProcess);
    char procName[MAX_PROC_NAME_LEN];
    proc_name(procPid, procName, sizeof(procName));
    int spoofed_securelevel = 1;
    
    DBGLOG(MODULE_KSL, "Process '%s' (PID: %d) accessed kern.securelevel. Spoofing value to %d.", procName, procPid, spoofed_securelevel);
    return SYSCTL_OUT(req, &spoofed_securelevel, sizeof(spoofed_securelevel));
    
}

// Function to reroute kern.securelevel to our custom one
bool reRouteSecureLevel(KernelPatcher &patcher) {
        
    // ensure that sysctlChildrenAddress exists before continuing
    if (!PHTM::gSysctlChildrenAddr) {
		DBGLOG(MODULE_ERROR, "Failed to resolve _sysctl__children passed to function reRouteSecureLevel.");
        return false;
	} else {
		DBGLOG(MODULE_RRSL, "Got address 0x%llx for _sysctl__children passed to function reRouteSecureLevel.", PHTM::gSysctlChildrenAddr);
	}

    sysctl_oid_list *sysctlChildren = reinterpret_cast<sysctl_oid_list *>(PHTM::gSysctlChildrenAddr);
    sysctl_oid *kernNode = nullptr;
    SLIST_FOREACH(kernNode, sysctlChildren, oid_link) {
        if (strcmp(kernNode->oid_name, "kern") == 0) {
            DBGLOG(MODULE_RRSL, "Found 'kern' node for securelevel.");
            break;
        }
    }

    if (!kernNode) {
        DBGLOG(MODULE_RRSL, "Failed to locate 'kern' node in sysctl tree for securelevel.");
        return false;
    }

    sysctl_oid_list *kernChildren = reinterpret_cast<sysctl_oid_list *>(kernNode->oid_arg1);
    sysctl_oid *securelevelNode = nullptr;
    SLIST_FOREACH(securelevelNode, kernChildren, oid_link) {
        if (strcmp(securelevelNode->oid_name, "securelevel") == 0) {
            DBGLOG(MODULE_RRSL, "Found 'securelevel' node.");
            break;
        }
    }

    if (!securelevelNode) {
        DBGLOG(MODULE_RRSL, "Failed to locate 'securelevel' sysctl entry.");
        return false;
    }

	SLP::originalSecureLevelHandler = securelevelNode->oid_handler;
    DBGLOG(MODULE_RRSL, "Successfully saved original 'securelevel' sysctl handler.");
	
	// On macOS Big Sur (Darwin 20) and newer (?), we must disable kernel write protection.
    // Not too sure when this began to be a requirement, but let's do it for Big Sur+ for now.
    // Various reports indicate it can work on Big Sur without, but on some hardware, may panic.
	if (getKernelVersion() >= KernelVersion::BigSur) {
        DBGLOG(MODULE_RRSL, "Big Sur or newer detected. Disabling kernel write protection...");
        PANIC_COND(MachInfo::setKernelWriting(true, patcher.kernelWriteLock) != KERN_SUCCESS, MODULE_SHORT, "Failed to disable kernel write protection.");
    }
    
    // Reroute the handler to our custom function
    securelevelNode->oid_handler = phtm_sysctl_securelevel;
	
	// Re-enable kernel write protection if we disabled it.
    if (getKernelVersion() >= KernelVersion::BigSur) {
        DBGLOG(MODULE_RRSL, "Re-enabling kernel write protection.");
        MachInfo::setKernelWriting(false, patcher.kernelWriteLock);
    }
	
	DBGLOG(MODULE_RRSL, "Successfully rerouted 'securelevel' sysctl handler.");
    return true;
	
}

// Function for the KMP init routine
void SLP::init(KernelPatcher &Patcher) {
    DBGLOG(MODULE_SLP, "SLP::init() called. SLP module is starting.");
	
	if (!PHTM::gSysctlChildrenAddr) {
        DBGLOG(MODULE_ERROR, "PHTM::gSysctlChildrenAddr is not set. Cannot perform SLP rerouting.");
        return;
    }
	
    // Perform rerouting, as Patcher is available and known (hopefully by now, yes it is)
    if (!reRouteSecureLevel(Patcher)) {
		DBGLOG(MODULE_ERROR, "Failed to reroute kern.securelevel.");
		panic(MODULE_LONG, "Failed to reroute kern.securelevel.");
    } else {
        DBGLOG(MODULE_INFO, "kern.securelevel rerouted successfully.");
    }

}
