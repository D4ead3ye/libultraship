/*  atomic64.c - 64-bit atomic builtins for Espresso

    The Wii U's CPU is a 32-bit PowerPC 750 derivative. It has no 64-bit
    atomic instructions, so GCC lowers every 8-byte atomic operation to a
    libatomic call - and devkitPPC ships no libatomic. These are the ones
    the engine and game actually reference.

    All of them serialise on a single uninterruptible spinlock. That is
    heavy-handed, but 8-byte atomics here are low frequency (audio sync
    flags, watchdog heartbeats) and correctness across the three cores
    matters more than contention. OSUninterruptibleSpinLock is used rather
    than the plain variant so an interrupt cannot land mid-update and
    deadlock against a handler touching the same value.
*/
#ifdef __WIIU__

#include <stdint.h>
#include <stdbool.h>
#include <coreinit/spinlock.h>

static OSSpinLock sAtomic64Lock;
static int sAtomic64LockReady = 0;

static inline void atomic64_lock(void) {
    if (!sAtomic64LockReady) {
        OSInitSpinLock(&sAtomic64Lock);
        sAtomic64LockReady = 1;
    }
    OSUninterruptibleSpinLock_Acquire(&sAtomic64Lock);
}

static inline void atomic64_unlock(void) {
    OSUninterruptibleSpinLock_Release(&sAtomic64Lock);
}

uint64_t __atomic_load_8(const volatile void* ptr, int memorder) {
    (void)memorder;
    atomic64_lock();
    uint64_t value = *(const volatile uint64_t*)ptr;
    atomic64_unlock();
    return value;
}

void __atomic_store_8(volatile void* ptr, uint64_t value, int memorder) {
    (void)memorder;
    atomic64_lock();
    *(volatile uint64_t*)ptr = value;
    atomic64_unlock();
}

uint64_t __atomic_exchange_8(volatile void* ptr, uint64_t value, int memorder) {
    (void)memorder;
    atomic64_lock();
    volatile uint64_t* target = (volatile uint64_t*)ptr;
    uint64_t previous = *target;
    *target = value;
    atomic64_unlock();
    return previous;
}

bool __atomic_compare_exchange_8(volatile void* ptr, void* expected, uint64_t desired, bool weak,
                                 int success_memorder, int failure_memorder) {
    (void)weak;
    (void)success_memorder;
    (void)failure_memorder;
    atomic64_lock();
    volatile uint64_t* target = (volatile uint64_t*)ptr;
    uint64_t* expectedValue = (uint64_t*)expected;
    bool matched = (*target == *expectedValue);
    if (matched) {
        *target = desired;
    } else {
        *expectedValue = *target;
    }
    atomic64_unlock();
    return matched;
}

#define ATOMIC64_FETCH_OP(name, op)                                       \
    uint64_t __atomic_fetch_##name##_8(volatile void* ptr, uint64_t value, int memorder) { \
        (void)memorder;                                                   \
        atomic64_lock();                                                  \
        volatile uint64_t* target = (volatile uint64_t*)ptr;              \
        uint64_t previous = *target;                                      \
        *target = previous op value;                                      \
        atomic64_unlock();                                                \
        return previous;                                                  \
    }

ATOMIC64_FETCH_OP(add, +)
ATOMIC64_FETCH_OP(sub, -)
ATOMIC64_FETCH_OP(and, &)
ATOMIC64_FETCH_OP(or, |)
ATOMIC64_FETCH_OP(xor, ^)

#endif
