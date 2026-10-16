import re
from pathlib import Path


def edit(path, old, new):
  path = Path(path)
  text = path.read_text()
  path.write_text(text.replace(old, new))
def insert(path, new):
  path = Path(path)
  text = path.read_text()
  path.write_text(new+text)
# ---------------------------------------------------------------
# Build configuration
# ---------------------------------------------------------------
path = "mono/metadata/mono-gc.h"
edit(path, "void mono_gc_stop_world (void);", "MONO_API void mono_gc_stop_world (void);")
edit(path, "void mono_gc_restart_world (void);", "MONO_API void mono_gc_restart_world (void);")

ios_mk = "sdks/builds/ios.mk"

# The stock device target is full-AOT only ("jit" is in the list of
# disabled features). Use the feature set of the Android target
# instead, which is a JIT runtime ("com" and "shared_perfcounters"
# stay off, as on every iOS target).
edit(
  ios_mk,
  "\t--enable-minimal=ssa,com,interpreter,jit,portability,"
  "assembly_remapping,attach,verifier,full_messages,appdomains,"
  "security,sgen_remset,sgen_marksweep_par,sgen_marksweep_fixed,"
  "sgen_marksweep_fixed_par,sgen_copying,logging,remoting,"
  "shared_perfcounters,gac \\\n",
  "\t--enable-minimal=ssa,com,portability,attach,verifier,"
  "full_messages,security,sgen_remset,sgen_marksweep_par,"
  "sgen_marksweep_fixed,sgen_marksweep_fixed_par,sgen_copying,"
  "logging,shared_perfcounters,shared_handles,interpreter,gac,"
  "cfgdir_config \\\n",
)

# A JIT runtime resolves icalls by name at run time, so the icall
# tables must be linked into libmonosgen instead of being split out
# into libmono-icall-table.a.
edit(ios_mk, "\t--disable-icall-tables \\\n", "")

# With this flag the embedding host has to create the GC threads
# itself, as Xamarin.iOS does. Let the runtime do it.
edit(ios_mk, "\t--with-lazy-gc-thread-creation=yes \\\n", "")

# unity-liveness.c is already listed in unity_gc_dependent_sources,
# but this branch only adds that source list to the Boehm runtime.
# Add it to SGen too.
edit(
  "mono/metadata/Makefile.am",
  "libmonoruntimesgen_la_SOURCES = "
  "$(common_sources) $(icall_tables_sources) "
  "$(ilgen_sources) $(gc_dependent_sources) $(sgen_sources)",
  "libmonoruntimesgen_la_SOURCES = "
  "$(common_sources) $(icall_tables_sources) "
  "$(ilgen_sources) $(gc_dependent_sources) "
  "$(unity_gc_dependent_sources) $(sgen_sources)",
)

# ---------------------------------------------------------------
# Code the current SDK and compiler reject
# ---------------------------------------------------------------

# asl_log () is deprecated since iOS 10, which is an error in the
# runtime now that the deployment target is past that. Log through
# os_log () instead, as this file already does on watchOS and
# Mac Catalyst.
edit(
  "mono/utils/mono-log-darwin.c",
  "|| defined(HOST_MACCAT)\n",
  "|| defined(HOST_MACCAT) || (__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ >= 100000)\n",
)
edit("mono/utils/mono-log-darwin.c", "%s", "%{public}s")
# The second field of struct objc_super is called class or
# super_class depending on the SDK, and the test used here to tell
# them apart picks the wrong one with the current SDK. Fill the
# struct by position instead.
edit(
  "mono/utils/mono-threads-mach-helper.c",
  "\tstruct objc_super super;\n"
  "\tsuper.receiver = self;\n"
  "#if !defined(__cplusplus) && !__OBJC2__\n"
  "\tsuper.class = nsobject;\n"
  "#else\n"
  "\tsuper.super_class = nsobject;\n"
  "#endif\n",
  "\tstruct objc_super super = { self, nsobject };\n",
)

# getsectbynamefromheader_64 () is deprecated since iOS 16, an
# error for the same reason as asl_log (). It still exists, and
# its replacement returns addresses with the slide applied where
# this one does not, so keep it and silence the warning for this
# call.
edit(
  "mono/metadata/w32process-unix-osx.c",
  "\tconst struct section_64 *sec = getsectbynamefromheader_64 (hdr64, SEG_DATA, SECT_DATA);\n",
  "#pragma clang diagnostic push\n"
  '#pragma clang diagnostic ignored "-Wdeprecated-declarations"\n'
  "\tconst struct section_64 *sec = getsectbynamefromheader_64 (hdr64, SEG_DATA, SECT_DATA);\n"
  "#pragma clang diagnostic pop\n",
)

# An SDK header includes <stdbool.h>, which makes bool a macro, so
# the two icall signatures spelled with it get _Bool pasted into
# their names. Spell them with boolean, which the signature types
# already accept.
for sig in ("3, (bool, ptr, ptrref)", "4, (bool, ptr, int32, ptrref)"):
  edit(
    "mono/metadata/icall-signatures.h",
    f"ICALL_SIG ({sig})",
    f"ICALL_SIG ({sig.replace('bool', 'boolean')})",
  )
for name in ("mono_icall_sig_bool_ptr_int32_ptrref", "mono_icall_sig_bool_ptr_ptrref"):
  edit(
    "mono/mini/aot-compiler.c",
    f"&{name},",
    f"&{name.replace('_bool_', '_boolean_')},",
  )

# Storing a pointer into an integer and assigning a function to
# a pointer of another type were warnings and are errors now.
edit(
  "mono/utils/mono-sigcontext.h",
  "\tUCONTEXT_REG_PC (ctx) = (val); \\\n",
  "\tUCONTEXT_REG_PC (ctx) = (host_mgreg_t)(val); \\\n",
)
edit(
  "mono/mini/debugger-agent.c",
  "\tcallbacks->buffer_add_byte = buffer_add_byte;\n"
  "\tcallbacks->buffer_add_int = buffer_add_int;\n"
  "\tcallbacks->buffer_add_id = buffer_add_id;\n"
  "\tcallbacks->buffer_add_string = buffer_add_string;\n",
  "\tcallbacks->buffer_add_byte = (void (*)(void *, guint8))buffer_add_byte;\n"
  "\tcallbacks->buffer_add_int = (void (*)(void *, guint32))buffer_add_int;\n"
  "\tcallbacks->buffer_add_id = (void (*)(void *, int))buffer_add_id;\n"
  "\tcallbacks->buffer_add_string = (void (*)(void *, const char *))buffer_add_string;\n",
)

# ---------------------------------------------------------------
# JIT code memory (see mono/utils/mono-ios-jit.h)
# ---------------------------------------------------------------
codeman = "mono/utils/mono-codeman.c"
edit(
  codeman,
  """#if defined(HAVE_PTHREAD_JIT_WRITE_PROTECT_NP) && defined(TARGET_OSX)
	if (__builtin_available (macOS 11, *)) {""",
  """#if defined(TARGET_IOS)
	if (true) {g_message("processing JIT!");"""
)
insert(codeman, "#include <pthread.h>\n")
"""
# Every instruction is emitted through arm_emit (). The header is
# included by all of mono/mini, which also brings in the memcpy () and
# memset () wrappers.
codegen = Path("mono/arch/arm64/arm64-codegen.h")
edit(
  codegen,
  "#define arm_emit(p, ins) do { *(guint32*)(p) = (ins); (p) += 4; } while (0)",
  "#include <mono/utils/mono-ios-jit.h>\n"
  "\n"
  "#define arm_emit(p, ins) do { *(guint32*)MONO_JIT_W (p) = (ins); (p) += 4; } while (0)",
)

# The helpers of that header which patch an instruction in place.
store = r"\*\((guint32|guint64|gpointer)\s*\*\)\s*\(?\s*(\w+)\s*\)?\s*=(?!=)"
text, count = re.subn(store, r"*(\1*)MONO_JIT_W (\2) =", codegen.read_text())
unpatched = [
  line
  for line in text.splitlines()
  if re.search(r"\*\s*\([^()]*\*\s*\)\s*\(?\s*\w+\s*\)?\s*(\[[^\]]*\]\s*)?=(?!=)", line)
]
if count < 1 or unpatched:
  raise RuntimeError(
    f"{codegen}: {count} in-place stores patched, left: {unpatched!r}"
  )
codegen.write_text(text)

# The remaining places which store data into code memory.
edit(
  "mono/mini/mini-arm64.c",
  "\t*(guint64*)code = (guint64)target;\n",
  "\t*(guint64*)MONO_JIT_W (code) = (guint64)target;\n",
)
edit(
  "mono/mini/tramp-arm64.c",
  "\t*(gpointer*)patch_addr = addr;\n",
  "\t*(gpointer*)MONO_JIT_W (patch_addr) = addr;\n",
)
edit(
  "mono/mini/tramp-arm64.c",
  "\t\t*(guint64*)code = (guint64)(gsize)tramp;\n",
  "\t\t*(guint64*)MONO_JIT_W (code) = (guint64)(gsize)tramp;\n",
)
edit(
  "mono/mini/mini-runtime.c",
  "\t\t\tjump_table [i] = code + ",
  "\t\t\t((gpointer *)MONO_JIT_W (jump_table)) [i] = code + ",
)
# These tables are only read back, so use the writable address for good.
for reserve in (
    "mono_code_manager_reserve (cfg->dynamic_info->code_mp, ",
    "mono_mem_manager_code_reserve (cfg->mem_manager, ",
):
  call = reserve + "sizeof (gpointer) * patch_info->data.table->table_size)"
  edit(
    "mono/mini/mini.c",
    f"table = (void **){call};",
    f"table = (void **)MONO_JIT_W ({call});",
  )
edit(
  codeman,
  "#include <mono/utils/mono-tls.h>\n",
  "#include <mono/utils/mono-tls.h>\n"
  "\n"
  "#define MONO_IOS_JIT_IMPLEMENTATION\n"
  "#include <mono/utils/mono-ios-jit.h>\n",
)
edit(
  codeman,
  "static void*\ncodechunk_valloc (void *preferred, guint32 size)\n{\n",
  "#ifdef MONO_IOS_JIT_POOL\n"
  "\n"
  "static void*\n"
  "codechunk_valloc (void *preferred, guint32 size)\n"
  "{\n"
  "\treturn ios_jit_alloc (size);\n"
  "}\n"
  "\n"
  "static void\n"
  "codechunk_vfree (void *ptr, guint32 size)\n"
  "{\n"
  "\tios_jit_free (ptr, size);\n"
  "}\n"
  "\n"
  "static void\n"
  "codechunk_cleanup (void)\n"
  "{\n"
  "}\n"
  "\n"
  "#else\n"
  "\n"
  "static void*\ncodechunk_valloc (void *preferred, guint32 size)\n{\n",
)
edit(
  codeman,
  "\tg_hash_table_destroy (valloc_freelists);\n}\n",
  "\tg_hash_table_destroy (valloc_freelists);\n}\n"
  "\n"
  "#endif /* MONO_IOS_JIT_POOL */\n",
)
edit(
  codeman,
  "\tgpointer res = dlmemalign (MIN_ALIGN, n);\n",
  "#ifdef MONO_IOS_JIT_POOL\n"
  "\tgpointer res = ios_jit_malloc (n);\n"
  "#else\n"
  "\tgpointer res = dlmemalign (MIN_ALIGN, n);\n"
  "#endif\n",
)
edit(
  codeman,
  "\tdlfree (p);\n",
  "#ifdef MONO_IOS_JIT_POOL\n"
  "\tios_jit_mfree (p);\n"
  "#else\n"
  "\tdlfree (p);\n"
  "#endif\n",
)
"""