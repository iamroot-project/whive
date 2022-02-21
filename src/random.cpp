// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <random.h>

#include <compat/cpuid.h>
#include <crypto/sha256.h>
#include <crypto/sha512.h>
#include <support/cleanse.h>
#ifdef WIN32
#include <compat.h> // for Windows API
#include <wincrypt.h>
#endif
#include <logging.h>  // for LogPrintf()
#include <sync.h>     // for Mutex
#include <util/time.h> // for GetTimeMicros()

#include <stdlib.h>
#include <thread>

#include <randomenv.h>

#include <support/allocators/secure.h>

#ifndef WIN32
#include <fcntl.h>
#include <sys/time.h>
#endif

#ifdef HAVE_SYS_GETRANDOM
#include <sys/syscall.h>
#include <linux/random.h>
#endif
#if defined(HAVE_GETENTROPY) || (defined(HAVE_GETENTROPY_RAND) && defined(MAC_OSX))
#include <unistd.h>
#endif
#if defined(HAVE_GETENTROPY_RAND) && defined(MAC_OSX)
#include <sys/random.h>
#endif
#ifdef HAVE_SYSCTL_ARND
#include <utilstrencodings.h> // for ARRAYLEN
#include <sys/sysctl.h>
#endif

[[noreturn]] static void RandFailure()
{
    LogPrintf("Failed to read randomness, aborting\n");
    std::abort();
}

static inline int64_t GetPerformanceCounter()
{
    // Read the hardware time stamp counter when available.
    // See https://en.wikipedia.org/wiki/Time_Stamp_Counter for more information.
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    return __rdtsc();
#elif !defined(_MSC_VER) && defined(__i386__)
    uint64_t r = 0;
    __asm__ volatile ("rdtsc" : "=A"(r)); // Constrain the r variable to the eax:edx pair.
    return r;
#elif !defined(_MSC_VER) && (defined(__x86_64__) || defined(__amd64__))
    uint64_t r1 = 0, r2 = 0;
    __asm__ volatile ("rdtsc" : "=a"(r1), "=d"(r2)); // Constrain r1 to rax and r2 to rdx.
    return (r2 << 32) | r1;
#else
    // Fall back to using C++11 clock (usually microsecond or nanosecond precision)
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
#endif
}

#ifdef HAVE_GETCPUID
static bool g_rdrand_supported = false;
static bool g_rdseed_supported = false;
static constexpr uint32_t CPUID_F1_ECX_RDRAND = 0x40000000;
static constexpr uint32_t CPUID_F7_EBX_RDSEED = 0x00040000;
#ifdef bit_RDRND
static_assert(CPUID_F1_ECX_RDRAND == bit_RDRND, "Unexpected value for bit_RDRND");
#endif
#ifdef bit_RDSEED
static_assert(CPUID_F7_EBX_RDSEED == bit_RDSEED, "Unexpected value for bit_RDSEED");
#endif

#if defined(__x86_64__) || defined(__amd64__) || defined(__i386__)
static std::atomic<bool> hwrand_initialized{false};
static bool rdrand_supported = false;
static constexpr uint32_t CPUID_F1_ECX_RDRAND = 0x40000000;
static void RDRandInit()
{
    uint32_t eax, ebx, ecx, edx;
    GetCPUID(1, 0, eax, ebx, ecx, edx);
    if (ecx & CPUID_F1_ECX_RDRAND) {
        g_rdrand_supported = true;
    }
    GetCPUID(7, 0, eax, ebx, ecx, edx);
    if (ebx & CPUID_F7_EBX_RDSEED) {
        g_rdseed_supported = true;
    }
}

static void ReportHardwareRand()
{
    // This must be done in a separate function, as InitHardwareRand() may be indirectly called
    // from global constructors, before logging is initialized.
    if (g_rdseed_supported) {
        LogPrintf("Using RdSeed as additional entropy source\n");
    }
    if (g_rdrand_supported) {
        LogPrintf("Using RdRand as an additional entropy source\n");
        rdrand_supported = true;
    }
    hwrand_initialized.store(true);
}
#else
#error "RdRand is only supported on x86 and x86_64"
#endif
}

/** Read 64 bits of entropy using rdseed.
 *
 * Must only be called when RdSeed is supported.
 */
static uint64_t GetRdSeed() noexcept
{
    // RdSeed may fail when the HW RNG is overloaded. Loop indefinitely until enough entropy is gathered,
    // but pause after every failure.
#ifdef __i386__
    uint8_t ok;
    uint32_t r1, r2;
    do {
        __asm__ volatile (".byte 0x0f, 0xc7, 0xf8; setc %1" : "=a"(r1), "=q"(ok) :: "cc"); // rdseed %eax
        if (ok) break;
        __asm__ volatile ("pause");
    } while(true);
    do {
        __asm__ volatile (".byte 0x0f, 0xc7, 0xf8; setc %1" : "=a"(r2), "=q"(ok) :: "cc"); // rdseed %eax
        if (ok) break;
        __asm__ volatile ("pause");
    } while(true);
    return (((uint64_t)r2) << 32) | r1;
#elif defined(__x86_64__) || defined(__amd64__)
    uint8_t ok;
    uint64_t r1;
    do {
        __asm__ volatile (".byte 0x48, 0x0f, 0xc7, 0xf8; setc %1" : "=a"(r1), "=q"(ok) :: "cc"); // rdseed %rax
        if (ok) break;
        __asm__ volatile ("pause");
    } while(true);
    return r1;
#else
#error "RdSeed is only supported on x86 and x86_64"
#endif
}

#else
/* Access to other hardware random number generators could be added here later,
 * assuming it is sufficiently fast (in the order of a few hundred CPU cycles).
 * Slower sources should probably be invoked separately, and/or only from
 * RandAddPeriodic (which is called once a minute).
 */
static void InitHardwareRand() {}
static void ReportHardwareRand() {}
#endif

static bool GetHWRand(unsigned char* ent32) {
#if defined(__x86_64__) || defined(__amd64__) || defined(__i386__)
    assert(hwrand_initialized.load(std::memory_order_relaxed));
    if (rdrand_supported) {
        uint8_t ok;
        // Not all assemblers support the rdrand instruction, write it in hex.
#ifdef __i386__
        for (int iter = 0; iter < 4; ++iter) {
            uint32_t r1, r2;
            __asm__ volatile (".byte 0x0f, 0xc7, 0xf0;" // rdrand %eax
                              ".byte 0x0f, 0xc7, 0xf2;" // rdrand %edx
                              "setc %2" :
                              "=a"(r1), "=d"(r2), "=q"(ok) :: "cc");
            if (!ok) return false;
            WriteLE32(ent32 + 8 * iter, r1);
            WriteLE32(ent32 + 8 * iter + 4, r2);
        }
#else
        uint64_t r1, r2, r3, r4;
        __asm__ volatile (".byte 0x48, 0x0f, 0xc7, 0xf0, " // rdrand %rax
                                "0x48, 0x0f, 0xc7, 0xf3, " // rdrand %rbx
                                "0x48, 0x0f, 0xc7, 0xf1, " // rdrand %rcx
                                "0x48, 0x0f, 0xc7, 0xf2; " // rdrand %rdx
                          "setc %4" :
                          "=a"(r1), "=b"(r2), "=c"(r3), "=d"(r4), "=q"(ok) :: "cc");
        if (!ok) return false;
        WriteLE64(ent32, r1);
        WriteLE64(ent32 + 8, r2);
        WriteLE64(ent32 + 16, r3);
        WriteLE64(ent32 + 24, r4);
#endif
        return true;
    }
#endif
    return false;
}

void RandAddSeed()
{
    // Seed with CPU performance counter
    int64_t nCounter = GetPerformanceCounter();
    RAND_add(&nCounter, sizeof(nCounter), 1.5);
    memory_cleanse((void*)&nCounter, sizeof(nCounter));
}

#ifndef WIN32
/** Fallback: get 32 bytes of system entropy from /dev/urandom. The most
 * compatible way to get cryptographic randomness on UNIX-ish platforms.
 */
static void GetDevURandom(unsigned char *ent32)
{
    int f = open("/dev/urandom", O_RDONLY);
    if (f == -1) {
        RandFailure();
    }
    int have = 0;
    do {
        ssize_t n = read(f, ent32 + have, NUM_OS_RANDOM_BYTES - have);
        if (n <= 0 || n + have > NUM_OS_RANDOM_BYTES) {
            close(f);
            RandFailure();
        }
        have += n;
    } while (have < NUM_OS_RANDOM_BYTES);
    close(f);
}
#endif

/** Get 32 bytes of system entropy. */
void GetOSRand(unsigned char *ent32)
{
#if defined(WIN32)
    HCRYPTPROV hProvider;
    int ret = CryptAcquireContextW(&hProvider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
    if (!ret) {
        RandFailure();
    }
    ret = CryptGenRandom(hProvider, NUM_OS_RANDOM_BYTES, ent32);
    if (!ret) {
        RandFailure();
    }
    CryptReleaseContext(hProvider, 0);
#elif defined(HAVE_SYS_GETRANDOM)
    /* Linux. From the getrandom(2) man page:
     * "If the urandom source has been initialized, reads of up to 256 bytes
     * will always return as many bytes as requested and will not be
     * interrupted by signals."
     */
    int rv = syscall(SYS_getrandom, ent32, NUM_OS_RANDOM_BYTES, 0);
    if (rv != NUM_OS_RANDOM_BYTES) {
        if (rv < 0 && errno == ENOSYS) {
            /* Fallback for kernel <3.17: the return value will be -1 and errno
             * ENOSYS if the syscall is not available, in that case fall back
             * to /dev/urandom.
             */
            GetDevURandom(ent32);
        } else {
            RandFailure();
        }
    }
#elif defined(HAVE_GETENTROPY) && defined(__OpenBSD__)
    /* On OpenBSD this can return up to 256 bytes of entropy, will return an
     * error if more are requested.
     * The call cannot return less than the requested number of bytes.
       getentropy is explicitly limited to openbsd here, as a similar (but not
       the same) function may exist on other platforms via glibc.
     */
    if (getentropy(ent32, NUM_OS_RANDOM_BYTES) != 0) {
        RandFailure();
    }
#elif defined(HAVE_GETENTROPY_RAND) && defined(MAC_OSX)
    /* getentropy() is available on macOS 10.12 and later.
     */
    if (getentropy(ent32, NUM_OS_RANDOM_BYTES) != 0) {
        RandFailure();
    }
#elif defined(HAVE_SYSCTL_ARND)
    /* FreeBSD, NetBSD and similar. It is possible for the call to return less
     * bytes than requested, so need to read in a loop.
     */
    static int name[2] = {CTL_KERN, KERN_ARND};
    int have = 0;
    do {
        size_t len = NUM_OS_RANDOM_BYTES - have;
        if (sysctl(name, ARRAYLEN(name), ent32 + have, &len, nullptr, 0) != 0) {
            RandFailure();
        }
        have += len;
    } while (have < NUM_OS_RANDOM_BYTES);
#else
    /* Fall back to /dev/urandom if there is no specific method implemented to
     * get system entropy for this OS.
     */
    GetDevURandom(ent32);
#endif
}

namespace {

class RNGState {
    Mutex m_mutex;
    /* The RNG state consists of 256 bits of entropy, taken from the output of
     * one operation's SHA512 output, and fed as input to the next one.
     * Carrying 256 bits of entropy should be sufficient to guarantee
     * unpredictability as long as any entropy source was ever unpredictable
     * to an attacker. To protect against situations where an attacker might
     * observe the RNG's state, fresh entropy is always mixed when
     * GetStrongRandBytes is called.
     */
    unsigned char m_state[32] GUARDED_BY(m_mutex) = {0};
    uint64_t m_counter GUARDED_BY(m_mutex) = 0;
    bool m_strongly_seeded GUARDED_BY(m_mutex) = false;

    Mutex m_events_mutex;
    CSHA256 m_events_hasher GUARDED_BY(m_events_mutex);

public:
    RNGState() noexcept
    {
        InitHardwareRand();
    }

    ~RNGState()
    {
    }

    void AddEvent(uint32_t event_info) noexcept
    {
        LOCK(m_events_mutex);

        m_events_hasher.Write((const unsigned char *)&event_info, sizeof(event_info));
        // Get the low four bytes of the performance counter. This translates to roughly the
        // subsecond part.
        uint32_t perfcounter = (GetPerformanceCounter() & 0xffffffff);
        m_events_hasher.Write((const unsigned char*)&perfcounter, sizeof(perfcounter));
    }

    /**
     * Feed (the hash of) all events added through AddEvent() to hasher.
     */
    void SeedEvents(CSHA512& hasher) noexcept
    {
        // We use only SHA256 for the events hashing to get the ASM speedups we have for SHA256,
        // since we want it to be fast as network peers may be able to trigger it repeatedly.
        LOCK(m_events_mutex);

        unsigned char events_hash[32];
        m_events_hasher.Finalize(events_hash);
        hasher.Write(events_hash, 32);

        // Re-initialize the hasher with the finalized state to use later.
        m_events_hasher.Reset();
        m_events_hasher.Write(events_hash, 32);
    }

    /** Extract up to 32 bytes of entropy from the RNG state, mixing in new entropy from hasher.
     *
     * If this function has never been called with strong_seed = true, false is returned.
     */
    bool MixExtract(unsigned char* out, size_t num, CSHA512&& hasher, bool strong_seed) noexcept
    {
        assert(num <= 32);
        unsigned char buf[64];
        static_assert(sizeof(buf) == CSHA512::OUTPUT_SIZE, "Buffer needs to have hasher's output size");
        bool ret;
        {
            LOCK(m_mutex);
            ret = (m_strongly_seeded |= strong_seed);
            // Write the current state of the RNG into the hasher
            hasher.Write(m_state, 32);
            // Write a new counter number into the state
            hasher.Write((const unsigned char*)&m_counter, sizeof(m_counter));
            ++m_counter;
            // Finalize the hasher
            hasher.Finalize(buf);
            // Store the last 32 bytes of the hash output as new RNG state.
            memcpy(m_state, buf + 32, 32);
        }
        // If desired, copy (up to) the first 32 bytes of the hash output as output.
        if (num) {
            assert(out != nullptr);
            memcpy(out, buf, num);
        }
        // Best effort cleanup of internal state
        hasher.Reset();
        memory_cleanse(buf, 64);
        return ret;
    }
};

RNGState& GetRNGState() noexcept
{
    // This C++11 idiom relies on the guarantee that static variable are initialized
    // on first call, even when multiple parallel calls are permitted.
    static std::vector<RNGState, secure_allocator<RNGState>> g_rng(1);
    return g_rng[0];
}
}

/* A note on the use of noexcept in the seeding functions below:
 *
 * None of the RNG code should ever throw any exception.
 */

void RandAddSeedSleep()
{
    int64_t nPerfCounter1 = GetPerformanceCounter();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    int64_t nPerfCounter2 = GetPerformanceCounter();

    // Combine with and update state
    AddDataToRng(&nPerfCounter1, sizeof(nPerfCounter1));
    AddDataToRng(&nPerfCounter2, sizeof(nPerfCounter2));

    memory_cleanse(&nPerfCounter1, sizeof(nPerfCounter1));
    memory_cleanse(&nPerfCounter2, sizeof(nPerfCounter2));
}


    // Stack pointer to indirectly commit to thread/callstack
    const unsigned char* ptr = buffer;
    hasher.Write((const unsigned char*)&ptr, sizeof(ptr));

    // Hardware randomness is very fast when available; use it always.
    SeedHardwareFast(hasher);

    // High-precision timestamp
    SeedTimestamp(hasher);
}

static void SeedSlow(CSHA512& hasher, RNGState& rng) noexcept
{
    unsigned char buffer[32];

    // Everything that the 'fast' seeder includes
    SeedFast(hasher);

    // OS randomness
    GetOSRand(buffer);
    hasher.Write(buffer, sizeof(buffer));

    // Add the events hasher into the mix
    rng.SeedEvents(hasher);

    // High-precision timestamp.
    //
    // Note that we also commit to a timestamp in the Fast seeder, so we indirectly commit to a
    // benchmark of all the entropy gathering sources in this function).
    SeedTimestamp(hasher);
}

/** Extract entropy from rng, strengthen it, and feed it into hasher. */
static void SeedStrengthen(CSHA512& hasher, RNGState& rng, int microseconds) noexcept
{
    // Generate 32 bytes of entropy from the RNG, and a copy of the entropy already in hasher.
    unsigned char strengthen_seed[32];
    rng.MixExtract(strengthen_seed, sizeof(strengthen_seed), CSHA512(hasher), false);
    // Strengthen the seed, and feed it into hasher.
    Strengthen(strengthen_seed, microseconds, hasher);
}

static void SeedPeriodic(CSHA512& hasher, RNGState& rng) noexcept
{
    // Everything that the 'fast' seeder includes
    SeedFast(hasher);

    // High-precision timestamp
    SeedTimestamp(hasher);

    // Add the events hasher into the mix
    rng.SeedEvents(hasher);

    // Dynamic environment data (performance monitoring, ...)
    auto old_size = hasher.Size();
    RandAddDynamicEnv(hasher);
    LogPrint(BCLog::RAND, "Feeding %i bytes of dynamic environment data into RNG\n", hasher.Size() - old_size);

    // Strengthen for 10 ms
    SeedStrengthen(hasher, rng, 10000);
}

static void SeedStartup(CSHA512& hasher, RNGState& rng) noexcept
{
    // Gather 256 bits of hardware randomness, if available
    SeedHardwareSlow(hasher);

    // Everything that the 'slow' seeder includes.
    SeedSlow(hasher, rng);

    // Dynamic environment data (performance monitoring, ...)
    auto old_size = hasher.Size();
    RandAddDynamicEnv(hasher);

    // Static environment data
    RandAddStaticEnv(hasher);
    LogPrint(BCLog::RAND, "Feeding %i bytes of environment data into RNG\n", hasher.Size() - old_size);

    // Strengthen for 100 ms
    SeedStrengthen(hasher, rng, 100000);
}

enum class RNGLevel {
    FAST, //!< Automatically called by GetRandBytes
    SLOW, //!< Automatically called by GetStrongRandBytes
    PERIODIC, //!< Called by RandAddPeriodic()
};

static void ProcRand(unsigned char* out, int num, RNGLevel level) noexcept
{
    // Make sure the RNG is initialized first (as all Seed* function possibly need hwrand to be available).
    RNGState& rng = GetRNGState();

    assert(num <= 32);

static void AddDataToRng(void* data, size_t len) {
    CSHA512 hasher;
    switch (level) {
    case RNGLevel::FAST:
        SeedFast(hasher);
        break;
    case RNGLevel::SLOW:
        SeedSlow(hasher, rng);
        break;
    case RNGLevel::PERIODIC:
        SeedPeriodic(hasher, rng);
        break;
    }

    // Combine with and update state
    {
        std::unique_lock<std::mutex> lock(cs_rng_state);
        hasher.Write(rng_state, sizeof(rng_state));
        hasher.Write((const unsigned char*)&rng_counter, sizeof(rng_counter));
        ++rng_counter;
        hasher.Finalize(buf);
        memcpy(rng_state, buf + 32, 32);
    }
}

void GetRandBytes(unsigned char* buf, int num) noexcept { ProcRand(buf, num, RNGLevel::FAST); }
void GetStrongRandBytes(unsigned char* buf, int num) noexcept { ProcRand(buf, num, RNGLevel::SLOW); }
void RandAddPeriodic() noexcept { ProcRand(nullptr, 0, RNGLevel::PERIODIC); }
void RandAddEvent(const uint32_t event_info) noexcept { GetRNGState().AddEvent(event_info); }

bool g_mock_deterministic_tests{false};

uint64_t GetRand(uint64_t nMax) noexcept
{
    if (nMax == 0)
        return 0;

    // The range of the random source must be a multiple of the modulus
    // to give every possible output value an equal possibility
    uint64_t nRange = (std::numeric_limits<uint64_t>::max() / nMax) * nMax;
    uint64_t nRand = 0;
    do {
        GetRandBytes((unsigned char*)&nRand, sizeof(nRand));
    } while (nRand >= nRange);
    return (nRand % nMax);
}

std::chrono::microseconds GetRandMicros(std::chrono::microseconds duration_max) noexcept
{
    return std::chrono::microseconds{GetRand(duration_max.count())};
}

int GetRandInt(int nMax) noexcept
{
    return GetRand(nMax);
}

uint256 GetRandHash()
{
    uint256 hash;
    GetRandBytes((unsigned char*)&hash, sizeof(hash));
    return hash;
}

void FastRandomContext::RandomSeed()
{
    uint256 seed = GetRandHash();
    rng.SetKey(seed.begin(), 32);
    requires_seed = false;
}

uint256 FastRandomContext::rand256()
{
    if (bytebuf_size < 32) {
        FillByteBuffer();
    }
    uint256 ret;
    memcpy(ret.begin(), bytebuf + 64 - bytebuf_size, 32);
    bytebuf_size -= 32;
    return ret;
}

std::vector<unsigned char> FastRandomContext::randbytes(size_t len)
{
    std::vector<unsigned char> ret(len);
    if (len > 0) {
        rng.Output(&ret[0], len);
    }
    return ret;
}

FastRandomContext::FastRandomContext(const uint256& seed) : requires_seed(false), bytebuf_size(0), bitbuf_size(0)
{
    rng.SetKey(seed.begin(), 32);
}

bool Random_SanityCheck()
{
    uint64_t start = GetPerformanceCounter();

    /* This does not measure the quality of randomness, but it does test that
     * GetOSRand() overwrites all 32 bytes of the output given a maximum
     * number of tries.
     */
    static const ssize_t MAX_TRIES = 1024;
    uint8_t data[NUM_OS_RANDOM_BYTES];
    bool overwritten[NUM_OS_RANDOM_BYTES] = {}; /* Tracks which bytes have been overwritten at least once */
    int num_overwritten;
    int tries = 0;
    /* Loop until all bytes have been overwritten at least once, or max number tries reached */
    do {
        memset(data, 0, NUM_OS_RANDOM_BYTES);
        GetOSRand(data);
        for (int x=0; x < NUM_OS_RANDOM_BYTES; ++x) {
            overwritten[x] |= (data[x] != 0);
        }

        num_overwritten = 0;
        for (int x=0; x < NUM_OS_RANDOM_BYTES; ++x) {
            if (overwritten[x]) {
                num_overwritten += 1;
            }
        }

        tries += 1;
    } while (num_overwritten < NUM_OS_RANDOM_BYTES && tries < MAX_TRIES);
    if (num_overwritten != NUM_OS_RANDOM_BYTES) return false; /* If this failed, bailed out after too many tries */

    // Check that GetPerformanceCounter increases at least during a GetOSRand() call + 1ms sleep.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    uint64_t stop = GetPerformanceCounter();
    if (stop == start) return false;

    // We called GetPerformanceCounter. Use it as entropy.
    RAND_add((const unsigned char*)&start, sizeof(start), 1);
    RAND_add((const unsigned char*)&stop, sizeof(stop), 1);

    return true;
}

FastRandomContext::FastRandomContext(bool fDeterministic) : requires_seed(!fDeterministic), bytebuf_size(0), bitbuf_size(0)
{
    if (!fDeterministic) {
        return;
    }
    uint256 seed;
    rng.SetKey(seed.begin(), 32);
}

void RandomInit()
{
    RDRandInit();
}
