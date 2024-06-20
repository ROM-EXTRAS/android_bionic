/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "pthread_internal.h"

#include <errno.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#include <async_safe/log.h>
#include <bionic/reserved_signals.h>

#include "bionic/tls_defines.h"
#include "private/ErrnoRestorer.h"
#include "private/ScopedRWLock.h"
#include "private/bionic_futex.h"
#include "private/bionic_globals.h"
#include "private/bionic_tls.h"

static pthread_internal_t* g_thread_list = nullptr;
static pthread_rwlock_t g_thread_list_lock = PTHREAD_RWLOCK_INITIALIZER;

pthread_t __pthread_internal_add(pthread_internal_t* thread) {
  ScopedWriteLock locker(&g_thread_list_lock);

  // We insert at the head.
  thread->next = g_thread_list;
  thread->prev = nullptr;
  if (thread->next != nullptr) {
    thread->next->prev = thread;
  }
  g_thread_list = thread;
  return reinterpret_cast<pthread_t>(thread);
}

void __pthread_internal_remove(pthread_internal_t* thread) {
  ScopedWriteLock locker(&g_thread_list_lock);

  if (thread->next != nullptr) {
    thread->next->prev = thread->prev;
  }
  if (thread->prev != nullptr) {
    thread->prev->next = thread->next;
  } else {
    g_thread_list = thread->next;
  }
}
// N.B. that this is NOT the pagesize, but 4096. This is hardcoded in the codegen.
// See
// https://github.com/search?q=repo%3Allvm/llvm-project%20AArch64StackTagging%3A%3AinsertBaseTaggedPointer&type=code
constexpr size_t kStackMteRingbufferSizeMultiplier = 4096;

static void __pthread_internal_free(pthread_internal_t* thread) {
#ifdef __aarch64__
  if (void* stack_mte_tls = thread->bionic_tcb->tls_slot(TLS_SLOT_STACK_MTE)) {
    size_t size =
        kStackMteRingbufferSizeMultiplier * (reinterpret_cast<uintptr_t>(stack_mte_tls) >> 56ULL);
    void* ptr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(stack_mte_tls) &
                                        ((1ULL << 56ULL) - 1ULL));
    munmap(ptr, size);
  }
#endif
  if (thread->mmap_size != 0) {
    // Free mapped space, including thread stack and pthread_internal_t.
    munmap(thread->mmap_base, thread->mmap_size);
  }
}

void __pthread_internal_remove_and_free(pthread_internal_t* thread) {
  __pthread_internal_remove(thread);
  __pthread_internal_free(thread);
}

pid_t __pthread_internal_gettid(pthread_t thread_id, const char* caller) {
  pthread_internal_t* thread = __pthread_internal_find(thread_id, caller);
  return thread ? thread->tid : -1;
}

pthread_internal_t* __pthread_internal_find(pthread_t thread_id, const char* caller) {
  pthread_internal_t* thread = reinterpret_cast<pthread_internal_t*>(thread_id);

  // Check if we're looking for ourselves before acquiring the lock.
  if (thread == __get_thread()) return thread;

  {
    // Make sure to release the lock before the abort below. Otherwise,
    // some apps might deadlock in their own crash handlers (see b/6565627).
    ScopedReadLock locker(&g_thread_list_lock);
    for (pthread_internal_t* t = g_thread_list; t != nullptr; t = t->next) {
      if (t == thread) return thread;
    }
  }

  // Historically we'd return null, but from API level 26 we catch this error.
  if (android_get_application_target_sdk_version() >= 26) {
    if (thread == nullptr) {
      // This seems to be a common mistake, and it's relatively harmless because
      // there will never be a valid thread at address 0, whereas other invalid
      // addresses might sometimes contain threads or things that look enough like
      // threads for us to do some real damage by continuing.
      // TODO: try getting rid of this when Treble lets us keep vendor blobs on an old API level.
      async_safe_format_log(ANDROID_LOG_WARN, "libc", "invalid pthread_t (0) passed to %s", caller);
    } else {
      async_safe_fatal("invalid pthread_t %p passed to %s", thread, caller);
    }
  }
  return nullptr;
}

static uintptr_t __get_main_stack_startstack() {
  FILE* fp = fopen("/proc/self/stat", "re");
  if (fp == nullptr) {
    async_safe_fatal("couldn't open /proc/self/stat: %m");
  }

  char line[BUFSIZ];
  if (fgets(line, sizeof(line), fp) == nullptr) {
    async_safe_fatal("couldn't read /proc/self/stat: %m");
  }

  fclose(fp);

  // See man 5 proc. There's no reason comm can't contain ' ' or ')',
  // so we search backwards for the end of it. We're looking for this field:
  //
  //  startstack %lu (28) The address of the start (i.e., bottom) of the stack.
  uintptr_t startstack = 0;
  const char* end_of_comm = strrchr(line, ')');
  if (sscanf(end_of_comm + 1,
             " %*c "
             "%*d %*d %*d %*d %*d "
             "%*u %*u %*u %*u %*u %*u %*u "
             "%*d %*d %*d %*d %*d %*d "
             "%*u %*u %*d %*u %*u %*u %" SCNuPTR,
             &startstack) != 1) {
    async_safe_fatal("couldn't parse /proc/self/stat");
  }

  return startstack;
}

void __find_main_stack_limits(uintptr_t* low, uintptr_t* high) {
  // Ask the kernel where our main thread's stack started.
  uintptr_t startstack = __get_main_stack_startstack();

  // Hunt for the region that contains that address.
  FILE* fp = fopen("/proc/self/maps", "re");
  if (fp == nullptr) {
    async_safe_fatal("couldn't open /proc/self/maps: %m");
  }
  char line[BUFSIZ];
  while (fgets(line, sizeof(line), fp) != nullptr) {
    uintptr_t lo, hi;
    if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR, &lo, &hi) == 2) {
      if (lo <= startstack && startstack <= hi) {
        *low = lo;
        *high = hi;
        fclose(fp);
        return;
      }
    }
  }
  async_safe_fatal("stack not found in /proc/self/maps");
}

__LIBC_HIDDEN__ void* __allocate_stack_mte_ringbuffer(size_t n, pthread_internal_t* thread) {
  if (n > 7) async_safe_fatal("error: invalid mte stack ring buffer size");
  // Allocation needs to be aligned to 2*size to make the fancy code-gen work.
  // So we allocate 3*size - pagesz bytes, which will always contain size bytes
  // aligned to 2*size, and unmap the unneeded part.
  // See
  // https://github.com/search?q=repo%3Allvm/llvm-project%20AArch64StackTagging%3A%3AinsertBaseTaggedPointer&type=code
  //
  // In the worst case, we get an allocation that is one page past the properly
  // aligned address, in which case we have to unmap the previous
  // 2*size - pagesz bytes. In that case, we still have size properly aligned
  // bytes left.
  size_t size = (1 << n) * kStackMteRingbufferSizeMultiplier;
  size_t pgsize = page_size();

  size_t alloc_size = __BIONIC_ALIGN(3 * size - pgsize, pgsize);
  void* allocation_ptr =
      mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (allocation_ptr == MAP_FAILED)
    async_safe_fatal("error: failed to allocate stack mte ring buffer");
  uintptr_t allocation = reinterpret_cast<uintptr_t>(allocation_ptr);

  size_t alignment = 2 * size;
  uintptr_t aligned_allocation = __BIONIC_ALIGN(allocation, alignment);
  if (allocation != aligned_allocation) {
    munmap(reinterpret_cast<void*>(allocation), aligned_allocation - allocation);
  }
  if (aligned_allocation + size != allocation + alloc_size) {
    munmap(reinterpret_cast<void*>(aligned_allocation + size),
           (allocation + alloc_size) - (aligned_allocation + size));
  }

  const char* name;
  if (thread == nullptr) {
    name = "stack_mte_ring:main";
  } else {
    // The kernel doesn't copy the name string, but this variable will last at least as long as the
    // mapped area. We unmap the ring buffer before unmapping the rest of the thread storage.
    auto& name_buffer = thread->stack_mte_ringbuffer_vma_name_buffer;
    static_assert(arraysize(name_buffer) >= arraysize("stack_mte_ring:") + 11 + 1);
    async_safe_format_buffer(name_buffer, arraysize(name_buffer), "stack_mte_ring:%d", thread->tid);
    name = name_buffer;
  }
  prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, reinterpret_cast<void*>(aligned_allocation), size, name);

  // We store the size in the top byte of the pointer (which is ignored)
  return reinterpret_cast<void*>(aligned_allocation | ((1ULL << n) << 56ULL));
}

bool __pthread_internal_remap_stack_with_mte() {
#if defined(__aarch64__)
  ScopedWriteLock creation_locker(&g_thread_creation_lock);
  ScopedReadLock list_locker(&g_thread_list_lock);
  // If process already uses memtag-stack ABI, we don't need to do anything.
  if (__libc_memtag_stack_abi) return false;
  __libc_memtag_stack_abi = true;

  for (pthread_internal_t* t = g_thread_list; t != nullptr; t = t->next) {
    if (t->terminating) continue;
    t->bionic_tcb->tls_slot(TLS_SLOT_STACK_MTE) =
        __allocate_stack_mte_ringbuffer(0, t->is_main() ? nullptr : t);
  }
  if (!atomic_load(&__libc_globals->memtag)) return false;
  if (atomic_exchange(&__libc_memtag_stack, true)) return false;
  uintptr_t lo, hi;
  __find_main_stack_limits(&lo, &hi);

  if (mprotect(reinterpret_cast<void*>(lo), hi - lo,
               PROT_READ | PROT_WRITE | PROT_MTE | PROT_GROWSDOWN)) {
    async_safe_fatal("error: failed to set PROT_MTE on main thread");
  }
  for (pthread_internal_t* t = g_thread_list; t != nullptr; t = t->next) {
    if (t->terminating || t->is_main()) continue;
    if (mprotect(t->mmap_base_unguarded, t->mmap_size_unguarded,
                 PROT_READ | PROT_WRITE | PROT_MTE)) {
      async_safe_fatal("error: failed to set PROT_MTE on thread: %d", t->tid);
    }
  }
  return true;
#else
  return false;
#endif  // defined(__aarch64__)
}

bool android_run_on_all_threads(bool (*func)(void*), void* arg) {
  // Take the locks in this order to avoid inversion (pthread_create ->
  // __pthread_internal_add).
  ScopedWriteLock creation_locker(&g_thread_creation_lock);
  ScopedReadLock list_locker(&g_thread_list_lock);

  // Call the function directly for the current thread so that we don't need to worry about
  // the consequences of synchronizing with ourselves.
  if (!func(arg)) {
    return false;
  }

  static sem_t g_sem;
  if (sem_init(&g_sem, 0, 0) != 0) {
    return false;
  }

  static bool (*g_func)(void*);
  static void *g_arg;
  g_func = func;
  g_arg = arg;

  static _Atomic(bool) g_retval;
  atomic_init(&g_retval, true);

  auto handler = [](int, siginfo_t*, void*) {
    ErrnoRestorer restorer;
    if (!g_func(g_arg)) {
      atomic_store(&g_retval, false);
    }
    sem_post(&g_sem);
  };

  struct sigaction act = {}, oldact;
  act.sa_flags = SA_SIGINFO;
  act.sa_sigaction = handler;
  sigfillset(&act.sa_mask);
  if (sigaction(BIONIC_SIGNAL_RUN_ON_ALL_THREADS, &act, &oldact) != 0) {
    sem_destroy(&g_sem);
    return false;
  }

  pid_t my_pid = getpid();
  size_t num_tids = 0;
  for (pthread_internal_t* t = g_thread_list; t != nullptr; t = t->next) {
    // The function is called directly for the current thread above, so no need to send a signal to
    // ourselves to call it here.
    if (t == __get_thread()) continue;

    // If a thread is terminating (has blocked signals) or has already terminated, our signal will
    // never be received, so we need to check for that condition and skip the thread if it is the
    // case.
    if (atomic_load(&t->terminating)) continue;

    if (tgkill(my_pid, t->tid, BIONIC_SIGNAL_RUN_ON_ALL_THREADS) == 0) {
      ++num_tids;
    } else {
      atomic_store(&g_retval, false);
    }
  }

  for (size_t i = 0; i != num_tids; ++i) {
    if (TEMP_FAILURE_RETRY(sem_wait(&g_sem)) != 0) {
      atomic_store(&g_retval, false);
      break;
    }
  }

  sigaction(BIONIC_SIGNAL_RUN_ON_ALL_THREADS, &oldact, 0);
  sem_destroy(&g_sem);
  return atomic_load(&g_retval);
}
