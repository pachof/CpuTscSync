#include <Headers/kern_util.hpp>
#include "VoodooTSCSync.h"
#include <stdatomic.h>

OSDefineMetaClassAndStructors(VoodooTSCSync, IOService)

_Atomic(bool) VoodooTSCSync::tsc_synced = false;
static _Atomic(uint16_t) cores_ready = 0;
static _Atomic(uint64_t) tsc_frequency = 0;

extern "C" int cpu_number(void);


//stamp the tsc
extern "C" void stamp_tsc(void *tscp)
{
    wrmsr64(MSR_IA32_TSC, *reinterpret_cast<uint64_t*>(tscp));
}

extern "C" void stamp_tsc_new(void *)
{
    atomic_fetch_add_explicit(&cores_ready, 1, memory_order_relaxed);

    uint32_t cpu = cpu_number();
    if (cpu != 0) {
        uint64_t tsc = 0;
        while ((tsc = atomic_load_explicit(&tsc_frequency, memory_order_acquire)) == 0) {
        }
        wrmsr64(MSR_IA32_TSC, tsc);
    } else {
        uint16_t threadCount = rdmsr64(MSR_CORE_THREAD_COUNT);
        while (atomic_load_explicit(&cores_ready, memory_order_acquire) != threadCount) {
        }
        atomic_store_explicit(&tsc_frequency, rdtsc64(), memory_order_relaxed);
    }
}

extern "C" void reset_tsc_adjust(void *)
{
    wrmsr64(MSR_IA32_TSC_ADJUST, 0);
}

IOService* VoodooTSCSync::probe(IOService* provider, SInt32* score)
{
    if (!super::probe(provider, score)) return NULL;
    if (!provider) return NULL;

    OSNumber* cpuNumber = OSDynamicCast(OSNumber, provider->getProperty("IOCPUNumber"));
    if (!cpuNumber) return NULL;

    if (getKernelVersion() >= KernelVersion::Monterey) {
        // only attach to last CPU
        uint16_t threadCount = rdmsr64(MSR_CORE_THREAD_COUNT);
        if (cpuNumber->unsigned16BitValue() != 0) return NULL;
        DBGLOG("cputs", "discovered cpu with %u threads, matching last, syncing", threadCount);
        mp_rendezvous_no_intrs(reset_tsc_adjust, NULL);
        DBGLOG("cputs", "done syncing");
        tsc_synced = true;
    } else {
        // only attach to last CPU
        uint16_t threadCount = rdmsr64(MSR_CORE_THREAD_COUNT);
        if (cpuNumber->unsigned16BitValue() != threadCount-1) return NULL;
    }

    return this;
}

IOReturn VoodooTSCSync::setPowerState(unsigned long state, IOService *whatDevice )
{
    if (state == PowerStateOff)
        tsc_synced = false;
    else if (state == PowerStateOn)
        doTSC();

    return IOPMAckImplied;
}

void VoodooTSCSync::stop(IOService *provider)
{
    PMstop();
    super::stop(provider);
}

bool VoodooTSCSync::start(IOService *provider)
{
    if (!super::start(provider)) { return false; }

    // announce version
    extern kmod_info_t kmod_info;
    SYSLOG("cputs", "Version %s starting on OS X Darwin %d.%d.\n", kmod_info.version, version_major, version_minor);

    // place version/build info in ioreg properties RM,Build and RM,Version
    char buf[128];
    snprintf(buf, sizeof(buf), "%s %s", kmod_info.name, kmod_info.version);
    setProperty("RM,Version", buf);
#ifdef DEBUG
    setProperty("RM,Build", "Debug");
#else
    setProperty("RM,Build", "Release");
#endif

    PMinit();
    registerPowerDriver(this, powerStates, 2);
    provider->joinPMtree(this);
    return true;
}

// Update MSR on all processors.
void VoodooTSCSync::doTSC()
{
    if (getKernelVersion() < KernelVersion::Monterey) {
        uint64_t tsc = rdtsc64();
        DBGLOG("cputs", "current tsc from rdtsc64() is %lld. Rendezvouing..\n", tsc);
        // call the kernel function that will call this "action" on all cores/processors
        mp_rendezvous_no_intrs(stamp_tsc, &tsc);
        tsc_synced = true;
    }
}
