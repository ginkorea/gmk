/*
 * GMK/cpu — Lock abstraction (spinlock in freestanding, pthread_mutex in hosted)
 */
#ifndef GMK_LOCK_H
#define GMK_LOCK_H

#ifdef GMK_FREESTANDING

#include "arch/spinlock.h"

typedef gmk_spinlock_t gmk_lock_t;

#define gmk_lock_init(l)    gmk_spinlock_init(l)
#define gmk_lock_destroy(l) gmk_spinlock_destroy(l)
#define gmk_lock_acquire(l) gmk_spinlock_acquire(l)
#define gmk_lock_release(l) gmk_spinlock_release(l)

#else /* hosted */

#include <pthread.h>

typedef pthread_mutex_t gmk_lock_t;

static inline void gmk_lock_init(gmk_lock_t *l)    { pthread_mutex_init(l, 0); }
static inline void gmk_lock_destroy(gmk_lock_t *l)  { pthread_mutex_destroy(l); }
static inline void gmk_lock_acquire(gmk_lock_t *l)  { pthread_mutex_lock(l); }
static inline void gmk_lock_release(gmk_lock_t *l)  { pthread_mutex_unlock(l); }

#endif /* GMK_FREESTANDING */

#endif /* GMK_LOCK_H */
