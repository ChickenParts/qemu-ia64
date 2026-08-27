:orphan:

IA-64 PAL processor model
=========================

The ``ipf`` machine currently exposes one explicit processor profile:
a first-generation Merced Itanium at 800 MHz with 4 MiB of external
L3 cache.  This is the high-end processor configuration used by the
Intel 460GX reference systems represented by the machine, including
the HP i2000 and rx4610.

PAL reports the processor-visible topology of that profile:

* split 16 KiB instruction and data L1 caches;
* a unified 96 KiB L2 cache;
* a unified 4 MiB L3 cache;
* a 100 MHz platform frequency base;
* an 8/1 processor ratio and 4/3 bus-clock ratio; and
* a 10/1 ITC ratio, matching QEMU's nanosecond virtual counter.

TCG does not simulate cache contents or processor cycles.  These
values are nevertheless a guest ABI and must describe one coherent
processor rather than a collection of firmware-enabling constants.
Translation-cache and translation-register counts are therefore
derived from the implemented CPU state arrays.

Future processor variants should be explicit CPU models or
properties.  Firmware hashes, instruction addresses, and image-byte
patterns must never select PAL results.
