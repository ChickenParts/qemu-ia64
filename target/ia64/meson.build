ia64_ss = ss.source_set()
ia64_ss.add(files(
  'cpu.c',
  'gdbstub.c',
  'helper.c',
  'interrupt.c',
  'pal.c',
  'translate.c',
))

ia64_system_ss = ss.source_set()

target_arch += {'ia64': ia64_ss}
target_system_arch += {'ia64': ia64_system_ss}
