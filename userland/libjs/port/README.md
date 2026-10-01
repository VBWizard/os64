# QuickJS target adaptation

R1 must implement os64 allocation accounting using os64_malloc_size, shared
string mappings, epoch/localtime conversion, formatting and fatal diagnostics,
and compiler-runtime support. The core must link to libmath rather than host
libm. No production adaptation or shared-library recipe is implemented here.
The maintained patches apply to a generated copy; upstream originals remain
byte-identical to the pinned archive. Do not spoof a platform macro to disable
Atomics: the Emscripten macro also disables the independent stack checks.

QuickJS's public header includes stdio.h and string.h. R1 must supply reviewed
freestanding compatibility declarations for binding consumers without exposing
host libc or claiming a complete libc API. R0's runtime header needs neither.
