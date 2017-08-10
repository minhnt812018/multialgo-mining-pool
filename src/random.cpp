// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "random.h"

#include "crypto/sha512.h"
#include "support/cleanse.h"
#ifdef WIN32
#include "compat.h" // for Windows API
#include <wincrypt.h>
#endif
#include "util.h"             // for LogPrint()
#include "utilstrencodings.h" // for GetTime()

#include <cstdlib>
#include <limits>

#ifndef WIN32
#include <sys/time.h>
#endif

#include <openssl/err.h>
#include <openssl/rand.h>

static void RandFailure() {
    LogPrintf("Failed to read randomness, aborting\n");
    abort();
}

static inline int64_t GetPerformanceCounter() {
    int64_t nCounter = 0;
#ifdef WIN32
    QueryPerformanceCounter((LARGE_INTEGER *)&nCounter);
#else
    timeval t;
    gettimeofday(&t, nullptr);
    nCounter = (int64_t)(t.tv_sec * 1000000 + t.tv_usec);
#endif
    return nCounter;
}

void RandAddSeed() {
    // Seed with CPU performance counter
    int64_t nCounter = GetPerformanceCounter();
    RAND_add(&nCounter, sizeof(nCounter), 1.5);
    memory_cleanse((void *)&nCounter, sizeof(nCounter));
}

static void RandAddSeedPerfmon() {
    RandAddSeed();

#ifdef WIN32
    // Don't need this on Linux, OpenSSL automatically uses /dev/urandom
    // Seed with the entire set of perfmon data

    // This can take up to 2 seconds, so only do it every 10 minutes
    static int64_t nLastPerfmon;
    if (GetTime() < nLastPerfmon + 10 * 60) return;
    nLastPerfmon = GetTime();

    std::vector<uint8_t> vData(250000, 0);
    long ret = 0;
    unsigned long nSize = 0;
    // Bail out at more than 10MB of performance data
    const size_t nMaxSize = 10000000;
    while (true) {
        nSize = vData.size();
        ret = RegQueryValueExA(HKEY_PERFORMANCE_DATA, "Global", nullptr,
                               nullptr, vData.data(), &nSize);
        if (ret != ERROR_MORE_DATA || vData.size() >= nMaxSize) {
            break;
        }
        // Grow size of buffer exponentially
        vData.resize(std::max((vData.size() * 3) / 2, nMaxSize));
    }
    RegCloseKey(HKEY_PERFORMANCE_DATA);
    if (ret == ERROR_SUCCESS) {
        RAND_add(vData.data(), nSize, nSize / 100.0);
        memory_cleanse(vData.data(), nSize);
        LogPrint("rand", "%s: %lu bytes\n", __func__, nSize);
    } else {
        // Warn only once
        static bool warned = false;
        if (!warned) {
            LogPrintf("%s: Warning: RegQueryValueExA(HKEY_PERFORMANCE_DATA) "
                      "failed with code %i\n",
                      __func__, ret);
            warned = true;
        }
    }
#endif
}

/** Get 32 bytes of system entropy. */
static void GetOSRand(uint8_t *ent32) {
#ifdef WIN32
    HCRYPTPROV hProvider;
    int ret = CryptAcquireContextW(&hProvider, nullptr, nullptr, PROV_RSA_FULL,
                                   CRYPT_VERIFYCONTEXT);
    if (!ret) {
        RandFailure();
    }
    ret = CryptGenRandom(hProvider, 32, ent32);
    if (!ret) {
        RandFailure();
    }
    CryptReleaseContext(hProvider, 0);
#else
    int f = open("/dev/urandom", O_RDONLY);
    if (f == -1) {
        RandFailure();
    }
    int have = 0;
    do {
        ssize_t n = read(f, ent32 + have, 32 - have);
        if (n <= 0 || n + have > 32) {
            RandFailure();
        }
        have += n;
    } while (have < 32);
    close(f);
#endif
}

void GetRandBytes(uint8_t *buf, int num) {
    if (RAND_bytes(buf, num) != 1) {
        RandFailure();
    }
}

void GetStrongRandBytes(uint8_t *out, int num) {
    assert(num <= 32);
    CSHA512 hasher;
    uint8_t buf[64];

    // First source: OpenSSL's RNG
    RandAddSeedPerfmon();
    GetRandBytes(buf, 32);
    hasher.Write(buf, 32);

    // Second source: OS RNG
    GetOSRand(buf);
    hasher.Write(buf, 32);

    // Produce output
    hasher.Finalize(buf);
    memcpy(out, buf, num);
    memory_cleanse(buf, 64);
}

uint64_t GetRand(uint64_t nMax) {
    if (nMax == 0) {
        return 0;
    }

    // The range of the random source must be a multiple of the modulus to give
    // every possible output value an equal possibility
    uint64_t nRange = (std::numeric_limits<uint64_t>::max() / nMax) * nMax;
    uint64_t nRand = 0;
    do {
        GetRandBytes((uint8_t *)&nRand, sizeof(nRand));
    } while (nRand >= nRange);
    return (nRand % nMax);
}

int GetRandInt(int nMax) {
    return GetRand(nMax);
}

uint256 GetRandHash() {
    uint256 hash;
    GetRandBytes((uint8_t *)&hash, sizeof(hash));
    return hash;
}

FastRandomContext::FastRandomContext(bool fDeterministic) {
    // The seed values have some unlikely fixed points which we avoid.
    if (fDeterministic) {
        Rz = Rw = 11;
    } else {
        uint32_t tmp;
        do {
            GetRandBytes((uint8_t *)&tmp, 4);
        } while (tmp == 0 || tmp == 0x9068ffffU);
        Rz = tmp;
        do {
            GetRandBytes((uint8_t *)&tmp, 4);
        } while (tmp == 0 || tmp == 0x464fffffU);
        Rw = tmp;
    }
}
