/**
* JIT code memory on iOS devices.
* iOS never maps memory writable and executable at the same time, and it
* only runs unsigned code in a process a debugger has attached to
* (CS_DEBUGGED), which is what JIT enablers such as StikDebug arrange.
* All JIT code therefore lives in one region that is mapped twice:
* read+execute at mono_ios_jit_rx, and read+write mono_ios_jit_rw_delta
* bytes further. Code runs from the first mapping and is written
* through the second one.
*/

#ifndef __MONO_UTILS_IOS_JIT_H__
#define __MONO_UTILS_IOS_JIT_H__

#if defined(HOST_IOS) && defined(__aarch64__)

#include <string.h>

#define MONO_IOS_JIT_POOL 1

extern unsigned char *mono_ios_jit_rx;
extern unsigned long mono_ios_jit_size;
extern long mono_ios_jit_rw_delta;

static inline __attribute__((always_inline, unused)) void *
mono_ios_jit_writable (const void *p)
{
  if ((unsigned long)p - (unsigned long)mono_ios_jit_rx < mono_ios_jit_size)
      return (void *)((unsigned long)p + mono_ios_jit_rw_delta);
  return (void *)p;
}

/* The address the code or data at P has to be written through. */
#define MONO_JIT_W(p) mono_ios_jit_writable (p)

/* Block writes can target JIT code as well. */
#undef memcpy
#undef memmove
#undef memset
#define memcpy(dest, src, n) __builtin_memcpy (MONO_JIT_W (dest), (src), (n))
#define memmove(dest, src, n) __builtin_memmove (MONO_JIT_W (dest), (src), (n))
#define memset(dest, c, n) __builtin_memset (MONO_JIT_W (dest), (c), (n))

#else

#define MONO_JIT_W(p) (p)

#endif

#endif /* __MONO_UTILS_IOS_JIT_H__ */

#if defined(MONO_IOS_JIT_IMPLEMENTATION) && defined(MONO_IOS_JIT_POOL)

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <mach/mach.h>

#define IOS_JIT_DEFAULT_POOL_MB 128

/* Not declared in the iOS SDK. */
int csops (pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define IOS_CS_OPS_STATUS 0
#define IOS_CS_DEBUGGED 0x10000000

unsigned char *mono_ios_jit_rx;
unsigned long mono_ios_jit_size;
long mono_ios_jit_rw_delta;

typedef struct _IosJitBlock IosJitBlock;
struct _IosJitBlock {
  IosJitBlock *next;
  gsize start, size;
};

static pthread_mutex_t ios_jit_mutex = PTHREAD_MUTEX_INITIALIZER;
/* The unused parts of the region, sorted by address. */
static IosJitBlock *ios_jit_free_blocks;
/* The sizes of the blocks handed out by ios_jit_malloc (). */
static GHashTable *ios_jit_malloc_sizes;
static volatile int ios_jit_brk_skipped;

/*
* On devices with TXM (iOS 26 and later) a page only becomes executable
* once the attached debugger has written to it, so the region has to
* come from the debugger. This is the StikDebug protocol: a script
* running in the debugger services brk #0xf00d, taking a command in x16
* (0: detach, 1: prepare a region) and its arguments in x0/x1. It
* leaves the result in x0 and resumes after the brk.
*/
static __attribute__((noinline, optnone, naked)) void *
ios_jit_brk_prepare_region (void *addr, size_t len)
{
  __asm__ (
      "mov x16, #1\n"
      "brk #0xf00d\n"
      "ret\n");
}

static __attribute__((noinline, optnone, naked)) void
ios_jit_brk_detach (void)
{
  __asm__ (
      "mov x16, #0\n"
      "brk #0xf00d\n"
      "ret\n");
}

/* Runs when nothing serviced the brk: step over it and fail the request. */
static void
ios_jit_sigtrap_handler (int sig, siginfo_t *info, void *context)
{
  ucontext_t *uc = (ucontext_t *)context;

  uc->uc_mcontext->__ss.__x [0] = 0;
  uc->uc_mcontext->__ss.__pc += 4;
  ios_jit_brk_skipped = 1;
}

static void *
ios_jit_debugger_request (gboolean detach, gsize size)
{
  struct sigaction action, old_action;
  void *res = NULL;

  memset (&action, 0, sizeof (action));
  action.sa_sigaction = ios_jit_sigtrap_handler;
  action.sa_flags = SA_SIGINFO;
  sigaction (SIGTRAP, &action, &old_action);

  ios_jit_brk_skipped = 0;
  if (detach)
      ios_jit_brk_detach ();
  else
      res = ios_jit_brk_prepare_region (NULL, size);

  sigaction (SIGTRAP, &old_action, NULL);
  return ios_jit_brk_skipped ? NULL : res;
}

static gboolean
ios_jit_enabled (void)
{
  guint32 flags = 0;

  return csops (getpid (), IOS_CS_OPS_STATUS, &flags, sizeof (flags)) == 0 && (flags & IOS_CS_DEBUGGED);
}

static gboolean
ios_jit_debugger_attached (void)
{
  int mib [4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid () };
  struct kinfo_proc info;
  size_t size = sizeof (info);

  memset (&info, 0, sizeof (info));
  return sysctl (mib, 4, &info, &size, NULL, 0) == 0 && (info.kp_proc.p_flag & P_TRACED);
}

static gboolean
ios_jit_is_executable (void *addr)
{
  vm_address_t start = (vm_address_t)addr;
  vm_size_t size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object = MACH_PORT_NULL;

  if (!addr || ((gsize)addr & (mono_pagesize () - 1)))
      return FALSE;
  if (vm_region_64 (mach_task_self (), &start, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS)
      return FALSE;
  return start <= (vm_address_t)addr && (info.protection & VM_PROT_EXECUTE);
}

/* LOCKING: Assumes ios_jit_mutex is held. */
static void
ios_jit_pool_init (void)
{
  const char *env = getenv ("MONO_IOS_JIT_POOL_MB");
  gsize size = (gsize)(env && atoi (env) > 0 ? atoi (env) : IOS_JIT_DEFAULT_POOL_MB) << 20;
  vm_address_t rw = 0;
  vm_prot_t cur_prot, max_prot;
  gboolean from_debugger;
  IosJitBlock *block;
  void *rx = NULL;
  int i;

  /* JIT enablers attach after the process has started. */
  for (i = 0; !ios_jit_enabled (); ++i) {
      if (i == 150)
          g_error ("JIT is not enabled for this process (CS_DEBUGGED is not set), use a JIT enabler such as StikDebug.");
      usleep (100 * 1000);
  }
  /* Plain enablers detach right away, the ones running a script stay. */
  usleep (300 * 1000);

  for (i = 0; i < 3 && !rx && ios_jit_debugger_attached (); ++i) {
      rx = ios_jit_debugger_request (FALSE, size);
      if (!ios_jit_is_executable (rx)) {
          rx = NULL;
          usleep (100 * 1000);
      }
  }
  from_debugger = rx != NULL;

  if (!rx) {
      /* This is enough where there is no TXM. */
      rx = mmap (NULL, size, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
      if (rx == MAP_FAILED)
          g_error ("Could not map the JIT code region: %s", strerror (errno));
  }

  if (vm_remap (mach_task_self (), &rw, size, 0, VM_FLAGS_ANYWHERE, mach_task_self (), (vm_address_t)rx, FALSE, &cur_prot, &max_prot, VM_INHERIT_NONE) != KERN_SUCCESS ||
      vm_protect (mach_task_self (), rw, size, FALSE, VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS)
      g_error ("Could not create the writable mapping of the JIT code region.");

  /* Pages cannot be prepared after this, the region never grows. */
  if (from_debugger)
      ios_jit_debugger_request (TRUE, 0);

  block = g_new0 (IosJitBlock, 1);
  block->start = (gsize)rx;
  block->size = size;
  ios_jit_free_blocks = block;

  mono_ios_jit_rw_delta = (long)(rw - (vm_address_t)rx);
  mono_ios_jit_size = size;
  mono_ios_jit_rx = (unsigned char *)rx;

  g_message ("JIT code region: %d MB at %p, %s", (int)(size >> 20), rx, from_debugger ? "prepared by the debugger" : "mapped by the process");
}

/* Returns zeroed memory. */
static gpointer
ios_jit_alloc (gsize size)
{
  IosJitBlock *block, *before = NULL;
  gpointer res = NULL;

  size = (size + 15) & ~(gsize)15;

  pthread_mutex_lock (&ios_jit_mutex);
  if (!mono_ios_jit_rx)
      ios_jit_pool_init ();
  g_message("trying to access block!")
  for (block = ios_jit_free_blocks; block && block->size < size; block = block->next)
      before = block;
  if (block) {
      res = (gpointer)block->start;
      g_message("res: %p", res);
      block->start += size;
      block->size -= size;
      if (!block->size) {
          if (before)
              before->next = block->next;
          else
              ios_jit_free_blocks = block->next;
          g_free (block);
      }
      else {
          g_message("block size is not null!")
      }
  }
    else {
        g_message("failed to access block!")
    }
  pthread_mutex_unlock (&ios_jit_mutex);

  if (!res) {
      g_message("res is null!")
      //g_error ("The JIT code region (%d MB) is full, set MONO_IOS_JIT_POOL_MB to a larger size.", (int)(mono_ios_jit_size >> 20));
  } else {
      memset (res, 0, size);
  }
  return res;
}

static void
ios_jit_free (gpointer ptr, gsize size)
{
  IosJitBlock *block, *next, *before = NULL;

  block = g_new0 (IosJitBlock, 1);
  block->start = (gsize)ptr;
  block->size = (size + 15) & ~(gsize)15;

  pthread_mutex_lock (&ios_jit_mutex);
  for (next = ios_jit_free_blocks; next && next->start < block->start; next = next->next)
      before = next;
  block->next = next;
  if (before)
      before->next = block;
  else
      ios_jit_free_blocks = block;

  /* Merge with the neighbours. */
  if (next && block->start + block->size == next->start) {
      block->size += next->size;
      block->next = next->next;
      g_free (next);
  }
  if (before && before->start + before->size == block->start) {
      before->size += block->size;
      before->next = block->next;
      g_free (block);
  }
  pthread_mutex_unlock (&ios_jit_mutex);
}

static gpointer
ios_jit_malloc (gsize size)
{
  gpointer res = ios_jit_alloc (size);

  pthread_mutex_lock (&ios_jit_mutex);
  if (!ios_jit_malloc_sizes)
      ios_jit_malloc_sizes = g_hash_table_new (NULL, NULL);
  g_hash_table_insert (ios_jit_malloc_sizes, res, (gpointer)size);
  pthread_mutex_unlock (&ios_jit_mutex);

  return res;
}

static void
ios_jit_mfree (gpointer ptr)
{
  gsize size;

  pthread_mutex_lock (&ios_jit_mutex);
  size = (gsize)g_hash_table_lookup (ios_jit_malloc_sizes, ptr);
  g_hash_table_remove (ios_jit_malloc_sizes, ptr);
  pthread_mutex_unlock (&ios_jit_mutex);

  ios_jit_free (ptr, size);
}

#endif /* MONO_IOS_JIT_IMPLEMENTATION && MONO_IOS_JIT_POOL */
