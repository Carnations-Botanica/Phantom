//
//  kern_start.cpp
//  Phantom
//
//  Created by RoyalGraphX on 10/12/24.
//

#include "kern_start.hpp"
#include "kern_vmm.hpp"
#include "kern_kextmanager.hpp"
#include "kern_securelevel.hpp"
// #include "kern_csr.hpp"
#include "kern_ioreg.hpp"

static PHTM phtmInstance;
PHTM *PHTM::callbackPHTM;

// Definition for the global _sysctl__children address
mach_vm_address_t PHTM::gSysctlChildrenAddr = 0;

// Define and initialize the static member variables for the PHTM class.
int PHTM::darwinMajor = 0;
int PHTM::darwinMinor = 0;

// To only be modified by CarnationsInternal, to display various Internal logs and headers
const bool PHTM::IS_INTERNAL = false; // MUST CHANCE THIS TO FALSE BEFORE CREATING COMMITS

// From RestrictEvents - RestrictEvents.cpp
// https://github.com/acidanthera/RestrictEvents/blob/41eb8cb8c1caf737eb6636638a1c76d0679c6400/RestrictEvents/RestrictEvents.cpp#L191C2-L222C3
static bool readNvramVariable(const char *fullName, const char16_t *unicodeName, const EFI_GUID *guid, void *dst, size_t max) {
	// First try the os-provided NVStorage. If it is loaded, it is not safe to call EFI services.
	NVStorage storage;
	if (storage.init()) {
		uint32_t size = 0;
		auto buf = storage.read(fullName, size, NVStorage::OptRaw);
		if (buf) {
			// Do not care if the value is a little bigger.
			if (size <= max) {
				memcpy(dst, buf, size);
			}
			Buffer::deleter(buf);
		}

		storage.deinit();

		return buf && size <= max;
	}

	// Otherwise use EFI services if available.
	auto rt = EfiRuntimeServices::get(true);
	if (rt) {
		uint64_t size = max;
		uint32_t attr = 0;
		auto status = rt->getVariable(unicodeName, guid, &attr, &size, dst);

		rt->put();
		return status == EFI_SUCCESS && size <= max;
	}

	return false;
}

// Function to get _sysctl__children memory address
mach_vm_address_t PHTM::sysctlChildrenAddr(KernelPatcher &patcher) {
	
    // Resolve the _sysctl__children symbol with the given patcher
    mach_vm_address_t resolvedAddress = patcher.solveSymbol(KernelPatcher::KernelID, "_sysctl__children");

    // Check if the address was successfully resolved, else return 0
    if (resolvedAddress) {
        DBGLOG(MODULE_SYSCA, "Resolved _sysctl__children at address: 0x%llx", resolvedAddress);

        // Iterate and log OIDs for debugging (can be extensive)
        #if DEBUG
        sysctl_oid_list *sysctlChildrenList = reinterpret_cast<sysctl_oid_list *>(resolvedAddress);
        DBGLOG(MODULE_SYSCA, "Sysctl children list at address: 0x%llx", reinterpret_cast<mach_vm_address_t>(sysctlChildrenList));
        sysctl_oid *oid;
        SLIST_FOREACH(oid, sysctlChildrenList, oid_link) {
            DBGLOG(MODULE_SYSCA, "OID Name: %s, OID Number: %d", oid->oid_name, oid->oid_number);
        }
        #endif
        
        return resolvedAddress;
    } else {
        KernelPatcher::Error err = patcher.getError();
        DBGLOG(MODULE_SYSCA, "Failed to resolve _sysctl__children. (Lilu returned: %d)", err);
        patcher.clearError();
        return 0;
    }
	
}

// Callback function to solve for and store _sysctl__children address
void PHTM::solveSysCtlChildrenAddr(void *user __unused, KernelPatcher &Patcher) {
    DBGLOG(MODULE_SSYSCTL, "PHTM::solveSysCtlChildrenAddr called successfully. Attempting to resolve and store _sysctl__children address.");
	
    PHTM::gSysctlChildrenAddr = PHTM::sysctlChildrenAddr(Patcher);
	
    if (PHTM::gSysctlChildrenAddr) {
        DBGLOG(MODULE_SSYSCTL, "Successfully resolved and stored _sysctl__children address: 0x%llx", PHTM::gSysctlChildrenAddr);
    } else {
        DBGLOG(MODULE_SSYSCTL, "Failed to resolve _sysctl__children address. PHTM::gSysctlChildrenAddr is NULL.");
		panic(MODULE_LONG, "Failed to resolve _sysctl__children address. PHTM::gSysctlChildrenAddr is NULL.");
    }

	bool initializeVMM = true;
	char revpatchValue[256] = {0};
	bool settingFound = false;
	if (PE_parse_boot_argn("revpatch", revpatchValue, sizeof(revpatchValue))) {
		DBGLOG(MODULE_INIT, "Read 'revpatch' from boot-args: %s", revpatchValue);
		settingFound = true;
	} else if (readNvramVariable("revpatch", u"revpatch", &EfiRuntimeServices::LiluVendorGuid, revpatchValue, sizeof(revpatchValue))) {
			DBGLOG(MODULE_INIT, "Read 'revpatch' from NVRAM: %s", revpatchValue);
			settingFound = true;
	}
	if (settingFound) {
		// Check if "sbvmm" is a substring of the setting.
		if (strstr(revpatchValue, "sbvmm") != nullptr) {
			initializeVMM = false;
			DBGLOG(MODULE_INIT, "Found 'sbvmm' in 'revpatch' setting, VMM module will be skipped.");
		}
	}
	
    // Begin routine selection based on kernel version.
    DBGLOG(MODULE_INIT, "Performing OS-specific reroutes...");

 	// For macOS Monterey (Darwin 21) and newer.
    if (PHTM::darwinMajor > KernelVersion::BigSur) {
        
        DBGLOG(MODULE_INIT, "Detected macOS Monterey or newer. Initializing all supported modules.");
        
        if (initializeVMM) {
            DBGLOG(MODULE_INIT, "Initializing VMM module.");
            VMM::init(Patcher);
        }
        
        DBGLOG(MODULE_INIT, "Initializing KMP module.");
        KMP::init(Patcher);
        
        DBGLOG(MODULE_INIT, "Initializing SLP module.");
        SLP::init(Patcher);
        
        DBGLOG(MODULE_INIT, "Initializing IOR module.");
        IOR::init(Patcher);

    // For supported versions up to and including Big Sur.
    } else if (PHTM::darwinMajor >= KernelVersion::HighSierra) {
        
        DBGLOG(MODULE_INIT, "Detected a supported legacy macOS version (High Sierra - Big Sur).");
		
        DBGLOG(MODULE_INIT, "Initializing KMP module.");
        KMP::init(Patcher);
        
        DBGLOG(MODULE_INIT, "Initializing SLP module.");
        SLP::init(Patcher);
        
        DBGLOG(MODULE_INIT, "Initializing IOR module.");
        IOR::init(Patcher);
        
    // Unsupported older versions.
    } else {
        DBGLOG(MODULE_ERROR, "Detected an unsupported version of macOS (older than High Sierra).");
        panic(MODULE_LONG, "Detected an unsupported version of macOS (older than High Sierra).");
    }
	
    DBGLOG(MODULE_SSYSCTL, "Finished all reroute attempts.");
}

// Main PHTM Routine function
void PHTM::init() {
	
	// EXPRESSED PERMISSIONS Header BEGIN
	// DO NOT MODIFY. NO EXPRESSED PERMISSION IS GIVEN BY CARNATIONS BOTANICA TO DO SO. NO EXCEPTIONS.
	// DO NOT MODIFY THE MARKED HEADER SECTION FOR EXPRESSED PERMISSIONS.
	
	// Do NOT include Phantom in your EFIs and share them.
	// Do NOT redistribute Phantom in binary form.
	// In any way, shape, or form. All users must source Phantom on their own, and apply the kernel extension with ProperTree manually.
	// If you are found to contain Phantom within your repository which provides an OpenCore EFI, you are subject to a DMCA request for violating
	// this incredibly clear warning and demonstration that we do not condone bypassing these efforts to limit the kind of users who can use Phantom.
	
	// For contributors, you can freely fork and work on improving Phantom without worry and with CI enabled. This header is still not
	// allowed for modification, even on forks with CI enabled, which allow for circumventing this header by providing an alternative
	// binary without said header. Again, your repository *will* be DMCA'd. You have been expressed no permission multiple times, and
	// we are allowed to protect our work as we see fit. Thank you for understanding and agreeing to these terms if you are using Phantom in any way,
	// shape or form, as per assumptions that you, are not Carnations Botanica, or CarnationsInternal. These clauses apply ontop of the LICENSE seen.
	// EXPRESSED PERMISSIONS Header END
    
    // Start off the routine
    callbackPHTM = this;
    PHTM::darwinMajor = getKernelVersion();
    PHTM::darwinMinor = getKernelMinorVersion();
    const char* phtmVersionNumber = PHTM_VERSION;
    DBGLOG(MODULE_INIT, "Hello World from Phantom!");
    DBGLOG(MODULE_INFO, "Current Build Version running: %s", phtmVersionNumber);
    DBGLOG(MODULE_INFO, "Copyright © 2024, 2025 Carnations Botanica. All rights reserved.");
    if (PHTM::darwinMajor > 0) {
        DBGLOG(MODULE_INFO, "Current Darwin Kernel version: %d.%d", PHTM::darwinMajor, PHTM::darwinMinor);
    } else {
        DBGLOG(MODULE_ERROR, "WARNING: Failed to retrieve Darwin Kernel version.");
    }
    
    // Internal Header BEGIN
    if (PHTM::IS_INTERNAL) {
        DBGLOG(MODULE_WARN, "");
        DBGLOG(MODULE_WARN, "==================================================================");
		DBGLOG(MODULE_WARN, "This build of %s is for CarnationsInternal usage only!", MODULE_LONG);
        DBGLOG(MODULE_WARN, "If you received a copy of this binary as a tester, DO NOT SHARE.");
        DBGLOG(MODULE_WARN, "==================================================================");
        DBGLOG(MODULE_WARN, "");
    }
    // Internal Header END
	
    // Begin based on kernel version detected.
	// This is messy because internally, we're debugging each version independently
	if (PHTM::darwinMajor >= KernelVersion::Tahoe) {
        DBGLOG(MODULE_INIT, "Detected macOS Tahoe (16.x) or newer.");
		DBGLOG(MODULE_WARN, "This version has not been verified to work with Phantom!");
		DBGLOG(MODULE_INIT, "Registering PHTM::solveSysCtlChildrenAddr with onPatcherLoadForce anyway.");
    	lilu.onPatcherLoadForce(&PHTM::solveSysCtlChildrenAddr);
    } else if (PHTM::darwinMajor >= KernelVersion::Sequoia) {
        DBGLOG(MODULE_INIT, "Detected macOS Sequoia (15.x).");
    	DBGLOG(MODULE_INIT, "Registering PHTM::solveSysCtlChildrenAddr with onPatcherLoadForce.");
    	lilu.onPatcherLoadForce(&PHTM::solveSysCtlChildrenAddr);
    } else if (PHTM::darwinMajor >= KernelVersion::Sonoma) {
        DBGLOG(MODULE_INIT, "Detected macOS Sonoma (14.x).");
		DBGLOG(MODULE_INIT, "Registering PHTM::solveSysCtlChildrenAddr with onPatcherLoadForce.");
		lilu.onPatcherLoadForce(&PHTM::solveSysCtlChildrenAddr);
    } else if (PHTM::darwinMajor >= KernelVersion::Ventura) {
        DBGLOG(MODULE_INIT, "Detected macOS Ventura (13.x).");
		DBGLOG(MODULE_INIT, "Registering PHTM::solveSysCtlChildrenAddr with onPatcherLoadForce.");
		lilu.onPatcherLoadForce(&PHTM::solveSysCtlChildrenAddr);
    } else if (PHTM::darwinMajor >= KernelVersion::Monterey) {
        DBGLOG(MODULE_INIT, "Detected macOS Monterey (12.x).");
		DBGLOG(MODULE_INIT, "Registering PHTM::solveSysCtlChildrenAddr with onPatcherLoadForce.");
		lilu.onPatcherLoadForce(&PHTM::solveSysCtlChildrenAddr);
    } else if (PHTM::darwinMajor >= KernelVersion::BigSur) {
        DBGLOG(MODULE_INIT, "Detected macOS Big Sur (11.x).");
		DBGLOG(MODULE_INIT, "Registering PHTM::solveSysCtlChildrenAddr with onPatcherLoadForce.");
		lilu.onPatcherLoadForce(&PHTM::solveSysCtlChildrenAddr);
    } else if (PHTM::darwinMajor >= KernelVersion::Catalina) {
        DBGLOG(MODULE_INIT, "Detected macOS Catalina (10.15.x).");
		DBGLOG(MODULE_INIT, "Registering PHTM::solveSysCtlChildrenAddr with onPatcherLoadForce.");
		lilu.onPatcherLoadForce(&PHTM::solveSysCtlChildrenAddr);
    } else if (PHTM::darwinMajor >= KernelVersion::Mojave) {
        DBGLOG(MODULE_INIT, "Detected macOS Mojave (10.14.x).");
		DBGLOG(MODULE_INIT, "Registering PHTM::solveSysCtlChildrenAddr with onPatcherLoadForce.");
		lilu.onPatcherLoadForce(&PHTM::solveSysCtlChildrenAddr);
    } else if (PHTM::darwinMajor >= KernelVersion::HighSierra) {
        DBGLOG(MODULE_INIT, "Detected macOS High Sierra (10.13.x).");
		DBGLOG(MODULE_INIT, "Registering PHTM::solveSysCtlChildrenAddr with onPatcherLoadForce.");
		lilu.onPatcherLoadForce(&PHTM::solveSysCtlChildrenAddr);
    } else {
        DBGLOG(MODULE_ERROR, "Detected an unsupported version of macOS (older than High Sierra).");
        panic(MODULE_LONG, "Detected an unsupported version of macOS (older than High Sierra).");
    }

}

// We use phtmState to determine PHTM behaviour
void PHTM::deinit() {
    
    DBGLOG(MODULE_ERROR, "This kernel extension cannot be disabled this way!");
    SYSLOG(MODULE_ERROR, "This kernel extension cannot be disabled this way!");
    
}

const char *bootargOff[] {
    "-phtmoff"
};

const char *bootargDebug[] {
    "-phtmdbg"
};

const char *bootargBeta[] {
    "-phtmbeta"
};

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal |
    LiluAPI::AllowSafeMode |
    LiluAPI::AllowInstallerRecovery,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
	KernelVersion::HighSierra,
	KernelVersion::Tahoe,
    []() {
        
        // Start the main PHTM routine
        phtmInstance.init();
        
    }
};
