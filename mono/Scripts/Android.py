from pathlib import Path

path = Path("sdks/builds/android.mk")
text = path.read_text()

text = text.replace(
  "\t--enable-boehm \\\n",
  "\t--disable-boehm \\\n"
)

text = text.replace(
  "\t--with-sgen=no \\\n",
  "\t--with-sgen=yes \\\n"
)

path.write_text(text)

# unity-liveness.c is already listed in
# unity_gc_dependent_sources, but this branch only adds that
# source list to the Boehm runtime. Add it to SGen too.
path = Path("mono/metadata/Makefile.am")
text = path.read_text()

old = (
  "libmonoruntimesgen_la_SOURCES = "
  "$(common_sources) $(icall_tables_sources) "
  "$(ilgen_sources) $(gc_dependent_sources) $(sgen_sources)"
)

new = (
  "libmonoruntimesgen_la_SOURCES = "
  "$(common_sources) $(icall_tables_sources) "
  "$(ilgen_sources) $(gc_dependent_sources) "
  "$(unity_gc_dependent_sources) $(sgen_sources)"
)

if old not in text:
  raise RuntimeError(
      "Could not find expected libmonoruntimesgen_la_SOURCES"
  )

text = text.replace(old, new, 1)
path.write_text(text)
PY

echo "===== android.mk GC configuration ====="
grep -nE 'enable-boehm|disable-boehm|with-sgen' \
sdks/builds/android.mk

echo "===== SGen liveness configuration ====="
grep -n -A2 -B1 \
'libmonoruntimesgen_la_SOURCES' \
mono/metadata/Makefile.am
