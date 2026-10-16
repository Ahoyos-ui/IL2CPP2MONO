/**
 * JIT code memory on iOS devices.
 * iOS never maps memory writable and executable at the same time, and it
 * only runs unsigned code in a process a debugger has attached to
 * (CS_DEBUGGED), which is what JIT enablers such as StikDebug arrange.
 * All JIT code therefore lives in one region that is mapped twice:
 * read+execute at mono_ios_jit_rx, and read+write mono_ios_jit_rw_delta
 * bytes further. Code runs from the first mapping and is written
 * through the second one.
 *
 * iOS 26 with TXM (Trusted Execution Monitor) requires the region to be
 * prepared by the attached debugger via the brk #0xf00d protocol before
 * any page becomes executable.
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

#define MONO_JIT_W(p) mono_ios_jit_writable (p)

#undef memcpy
#undef memmove
#undef memset
#define memcpy(dest, src, n) __builtin_memcpy (MONO_JIT_W (dest), (src), (n))
#define memmove(dest, src, n) __builtin_memmove (MONO_JIT_W (dest), (src), (n))
#define memset(dest, c, n) __builtin_memset (MONO_JIT_W (dest), (c), (n))

#endif
#endif

#if defined(MONO_IOS_JIT_IMPLEMENTATION) && defined(MONO_IOS_JIT_POOL)

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <limits.h>
#include <mach/mach.h>

#define IOS_JIT_DEFAULT_POOL_MB 128

/* Not declared in the iOS SDK. */
int csops (pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define IOS_CS_OPS_STATUS 0
#define IOS_CS_DEBUGGED 0x10000000

/* TXM firmware path used to detect iOS 26+ hardware. */
#define IOS_TXM_FIRMWARE_PATH "/private/preboot/%@/FUD/Ap,TrustedExecutionMonitor.img4"

unsigned char *mono_ios_jit_rx;
unsigned long mono_ios_jit_size;
long mono_ios_jit_rw_delta;

typedef struct _IosJitBlock IosJitBlock;
struct _IosJitBlock {
  IosJitBlock *next;
  gsize start, size;
};

static pthread_mutex_t ios_jit_mutex = PTHREAD_MUTEX_INITIALIZER;
static IosJitBlock *ios_jit_free_blocks;
static GHashTable *ios_jit_malloc_sizes;
static volatile int ios_jit_brk_skipped;
static gboolean ios_jit_initialized = FALSE;

/* StikDebug brk protocol. x16: 0=detach, 1=prepare region. Args in x0/x1. */
static __attribute__((noinline, optnone, naked)) void *
ios_jit_brk_prepare_region (void *addr, size_t len)
{
  __asm__ (
      "mov x0, %0\n"
      "mov x1, %1\n"
      "mov x16, #1\n"
      "brk #0xf00d\n"
      "ret\n"
      :
      : "r"(addr), "r"(len)
      : "x0", "x1", "x16", "memory");
}

static __attribute__((noinline, optnone, naked)) void
ios_jit_brk_detach (void)
{
  __asm__ (
      "mov x16, #0\n"
      "brk #0xf00d\n"
      "ret\n"
      :
      :
      : "x16", "memory");
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
  return csops (getpid (), IOS_CS_OPS_STATUS, &flags, sizeof (flags)) == 0
         && (flags & IOS_CS_DEBUGGED);
}

static gboolean
ios_jit_debugger_attached (void)
{
  int mib [4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid () };
  struct kinfo_proc info;
  size_t size = sizeof (info);
  memset (&info, 0, sizeof (info));
  return sysctl (mib, 4, &info, &size, NULL, 0) == 0
         && (info.kp_proc.p_flag & P_TRACED);
}

/* Detects TXM hardware via the TrustedExecutionMonitor firmware file. */
static gboolean
ios_jit_has_txm (void)
{
  char path [PATH_MAX];
  const char *home = getenv ("HOME");
  struct stat st;

  if (!home)
      return FALSE;

  snprintf (path, sizeof (path),
            "/private/preboot/%s/FUD/Ap,TrustedExecutionMonitor.img4", home);
  return stat (path, &st) == 0;
}

static gboolean
ios_jit_is_executable (void *addr)
{
  vm_address_t start = (vm_address_t)addr;
  vm_size_t size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object = MACH_PORT_NULL;
  gboolean result = FALSE;

  if (!addr || ((gsize)addr & (mono_pagesize () - 1)))
      return FALSE;
  if (vm_region_64 (mach_task_self (), &start, &size,
                    VM_REGION_BASIC_INFO_64,
                    (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS)
      return FALSE;

  result = start <= (vm_address_t)addr && (info.protection & VM_PROT_EXECUTE);

  if (object != MACH_PORT_NULL)
      mach_port_deallocate (mach_task_self (), object);

  return result;
}

/* LOCKING: Assumes ios_jit_mutex is held. */
static void
ios_jit_pool_init (void)
{
  const char *env = getenv ("MONO_IOS_JIT_POOL_MB");
  gsize size = (gsize)(env && atoi (env) > 0 ? atoi (env)
                                               : IOS_JIT_DEFAULT_POOL_MB) << 20;
  vm_address_t rw = 0;
  vm_prot_t cur_prot, max_prot;
  gboolean from_debugger = FALSE;
  IosJitBlock *block;
  void *rx = NULL;
  int i;

  /* Wait for the JIT enabler to attach. */
  for (i = 0; !ios_jit_enabled (); ++i) {
      if (i == 150)
          g_error ("JIT is not enabled (CS_DEBUGGED not set). "
                   "Use StikDebug or another JIT enabler.");
      usleep (100 * 1000);
  }
  usleep (300 * 1000); /* Let one-shot enablers detach. */

  if (ios_jit_has_txm ()) {
      for (i = 0; i < 3 && !rx && ios_jit_debugger_attached (); ++i) {
          rx = ios_jit_debugger_request (FALSE, size);
          if (!ios_jit_is_executable (rx)) {
              rx = NULL;
              usleep (100 * 1000);
          }
      }
      from_debugger = rx != NULL;

      if (!from_debugger) {
          /* On TXM, mmap(PROT_EXEC) does NOT produce executable pages.
           * Failing here is better than silently continuing broken. */
          g_error ("TXM detected but the debugger did not prepare the JIT region. "
                   "Ensure StikDebug is attached and the iOS 26 script is loaded.");
      }
  }

  if (!rx) {
      /* Non-TXM fallback: plain mmap works. */
      rx = mmap (NULL, size, PROT_READ | PROT_EXEC,
                 MAP_ANON | MAP_PRIVATE, -1, 0);
      if (rx == MAP_FAILED)
          g_error ("Could not map the JIT code region: %s", strerror (errno));
  }

  if (vm_remap (mach_task_self (), &rw, size, 0, VM_FLAGS_ANYWHERE,
                mach_task_self (), (vm_address_t)rx, FALSE,
                &cur_prot, &max_prot, VM_INHERIT_NONE) != KERN_SUCCESS ||
      vm_protect (mach_task_self (), rw, size, FALSE,
                  VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS)
      g_error ("Could not create the writable mapping of the JIT region.");

  if (from_debugger)
      ios_jit_debugger_request (TRUE, 0);

  block = g_new0 (IosJitBlock, 1);
  block->start = (gsize)rx;
  block->size = size;
  ios_jit_free_blocks = block;

  mono_ios_jit_rw_delta = (long)(rw - (vm_address_t)rx);
  mono_ios_jit_size = size;
  mono_ios_jit_rx = (unsigned char *)rx;
  ios_jit_initialized = TRUE;

  g_message ("JIT region: %d MB at %p, %s", (int)(size >> 20), rx,
             from_debugger ? "prepared by debugger" : "mapped by process");
}

/* Returns zeroed memory. */
static gpointer
ios_jit_alloc (gsize size)
{
  IosJitBlock *block, *before = NULL;
  gpointer res = NULL;

  size = (size + 15) & ~(gsize)15;

  pthread_mutex_lock (&ios_jit_mutex);
  if (!ios_jit_initialized)
      ios_jit_pool_init ();

  for (block = ios_jit_free_blocks; block && block->size < size;
       block = block->next)
      before = block;

  if (block) {
      res = (gpointer)block->start;
      block->start += size;
      block->size -= size;
      if (!block->size) {
          if (before)
              before->next = block->next;
          else
              ios_jit_free_blocks = block->next;
          g_free (block);
      }
  }
  pthread_mutex_unlock (&ios_jit_mutex);

  if (!res)
      g_error ("JIT pool (%d MB) exhausted. Set MONO_IOS_JIT_POOL_MB larger.",
               (int)(mono_ios_jit_size >> 20));

  memset (res, 0, size);
  return res;
}

static void
ios_jit_free (gpointer ptr, gsize size)
{
  IosJitBlock *block, *next, *before = NULL;

  if (!size || !ptr)
      return;

  block = g_new0 (IosJitBlock, 1);
  block->start = (gsize)ptr;
  block->size = (size + 15) & ~(gsize)15;

  pthread_mutex_lock (&ios_jit_mutex);
  for (next = ios_jit_free_blocks; next && next->start < block->start;
       next = next->next)
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
  if (!res)
      return NULL;                /* do not poison the hash with (NULL, size) */

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
  gpointer lookup;

  if (!ptr)
      return;

  pthread_mutex_lock (&ios_jit_mutex);
  lookup = g_hash_table_lookup (ios_jit_malloc_sizes, ptr);
  if (!lookup) {
      pthread_mutex_unlock (&ios_jit_mutex);
      g_warning ("ios_jit_mfree: unknown pointer %p", ptr);
      return;
  }
  size = (gsize)lookup;
  g_hash_table_remove (ios_jit_malloc_sizes, ptr);
  pthread_mutex_unlock (&ios_jit_mutex);

  ios_jit_free (ptr, size);
}

#endif /* MONO_IOS_JIT_IMPLEMENTATION && MONO_IOS_JIT_POOL */
