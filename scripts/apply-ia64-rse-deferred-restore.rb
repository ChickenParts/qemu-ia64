#!/usr/bin/env ruby
# frozen_string_literal: true

# Apply the deferred IA-64 BSPSTORE -> br.ret identity repair to the exact
# source shape at ChickenParts/qemu-ia64 75f316c. Every edit is fail-closed:
# a source fragment must occur exactly once or no files are written.

require "optparse"
require "pathname"

class String
  def indent(width)
    prefix = " " * width
    lines.map { |line| line.strip.empty? ? line : "#{prefix}#{line}" }.join
  end
end

options = { apply: false }
OptionParser.new do |parser|
  parser.banner = "Usage: #{File.basename($PROGRAM_NAME)} [--check|--apply] [SOURCE_ROOT]"
  parser.on("--check", "Validate all edits without writing (default)") { options[:apply] = false }
  parser.on("--apply", "Write the validated edits") { options[:apply] = true }
end.parse!

root = Pathname(ARGV.shift || ".").expand_path
abort "unexpected arguments: #{ARGV.join(' ')}" unless ARGV.empty?

files = {
  helper: root / "target/ia64/helper.c",
  rse_h: root / "target/ia64/rse.h",
  rse_c: root / "target/ia64/rse.c",
  test: root / "tests/unit/test-ia64-rse.c",
}
missing = files.values.reject(&:file?)
abort "missing source files:\n  #{missing.join("\n  ")}" unless missing.empty?

texts = files.transform_values(&:read)
originals = texts.dup

def replace_once!(text, old, replacement, label)
  count = text.scan(old).length
  abort "#{label}: expected exactly one source fragment, found #{count}" unless count == 1
  text.sub(old, replacement)
end

def insert_once!(text, anchor, insertion, label)
  replace_once!(text, anchor, "#{insertion}#{anchor}", label)
end

api_marker = "bool ia64_rse_should_arm_stack_switch("
abort "deferred RSE repair already appears to be applied" if texts[:rse_h].include?(api_marker)

header_api = <<~'C'

  /*
   * Recognize only the architectural first half of a non-local restore here.
   * b0 and PFS are intentionally absent: firmware is allowed to restore them
   * after BSPSTORE and before br.ret.
   */
  bool ia64_rse_should_arm_stack_switch(uint64_t old_bspstore,
                                        uint64_t new_bspstore,
                                        uint64_t old_bsp,
                                        size_t shadow_depth,
                                        bool lazy_mode,
                                        bool task_switch);

  /* Bind the late return identity at br.ret; reject an ordinary top return. */
  int ia64_rse_bind_stack_switch_return(
      const struct IA64RSEReturnFrameView *view,
      uint64_t restored_bsp,
      uint64_t current_pfs,
      uint64_t current_b0,
      uint64_t top_ret_addr);
C
texts[:rse_h] = insert_once!(texts[:rse_h],
                             "\n#endif /* TARGET_IA64_RSE_H */\n",
                             header_api,
                             "target/ia64/rse.h")

rse_impl = <<~'C'

  bool ia64_rse_should_arm_stack_switch(uint64_t old_bspstore,
                                        uint64_t new_bspstore,
                                        uint64_t old_bsp,
                                        size_t shadow_depth,
                                        bool lazy_mode,
                                        bool task_switch)
  {
      old_bspstore &= ~UINT64_C(0x7);
      new_bspstore &= ~UINT64_C(0x7);
      old_bsp &= ~UINT64_C(0x7);

      return !task_switch && lazy_mode && shadow_depth > 0 &&
             old_bspstore != 0 && new_bspstore != 0 &&
             new_bspstore <= old_bspstore && new_bspstore <= old_bsp;
  }

  int ia64_rse_bind_stack_switch_return(
      const struct IA64RSEReturnFrameView *view,
      uint64_t restored_bsp,
      uint64_t current_pfs,
      uint64_t current_b0,
      uint64_t top_ret_addr)
  {
      uint64_t target = current_b0 & ~UINT64_C(0xf);
      uint64_t top = top_ret_addr & ~UINT64_C(0xf);

      if (target == 0 || (top != 0 && target == top)) {
          return -1;
      }
      return ia64_rse_find_stack_switch_boundary(view, restored_bsp,
                                                  current_pfs);
  }
C
texts[:rse_c] = "#{texts[:rse_c].rstrip}\n#{rse_impl}"

old_arm = <<~'C'.indent(4)
  uint64_t restored_b0 = env->b[0] & ~UINT64_C(0xf);
  uint64_t top_ret = 0;
  if (env->rse_depth > 0 && env->rse_frames) {
      top_ret = env->rse_frames[env->rse_depth - 1].ret_addr &
                ~UINT64_C(0xf);
  }
  if (!task_switch && old_bspstore != 0 && bspstore != 0 &&
      env->rse_depth > 0 && ia64_rse_is_lazy(env) &&
      bspstore <= old_bspstore && bspstore <= old_bsp &&
      restored_b0 != 0 && restored_b0 != top_ret) {
      env->rse_stack_switch_pending = 1;
      env->rse_stack_switch_bsp = bspstore;
      env->rse_stack_switch_pfs = env->ar[IA64_AR_PFS];
      env->rse_stack_switch_b0 = restored_b0;
      env->rse_stack_switch_ip = env->ip;
      ia64_rse_strict_trace(env, "stack_switch_arm", bspstore,
                            restored_b0);
  } else {
      ia64_rse_cancel_stack_switch(env);
  }
C
new_arm = <<~'C'.indent(4)
  if (ia64_rse_should_arm_stack_switch(old_bspstore, bspstore, old_bsp,
                                       env->rse_depth,
                                       ia64_rse_is_lazy(env),
                                       task_switch)) {
      /* b0 and PFS are restored later; zero identity marks provisional. */
      env->rse_stack_switch_pending = 1;
      env->rse_stack_switch_bsp = bspstore;
      env->rse_stack_switch_pfs = 0;
      env->rse_stack_switch_b0 = 0;
      env->rse_stack_switch_ip = env->ip;
      ia64_rse_strict_trace(env, "stack_switch_arm", bspstore, 0);
  } else {
      ia64_rse_cancel_stack_switch(env);
  }
C
texts[:helper] = replace_once!(texts[:helper], old_arm, new_arm,
                               "target/ia64/helper.c BSPSTORE arm")

ret_anchor = <<~'C'.indent(4)
  uint64_t bsp = ia64_rse_get_bsp(env);
  uint64_t pfs_cfm = env->ar[IA64_AR_PFS] & IA64_PFM_MASK;
  uint64_t b0 = env->b[0] & ~0xFULL;
  bool stack_switch = ia64_rse_stack_switch_matches(env, bsp, b0,
                                                    pfs_cfm);
C
ret_replacement = <<~'C'.indent(4)
  uint64_t bsp = ia64_rse_get_bsp(env);
  uint64_t pfs_cfm = env->ar[IA64_AR_PFS] & IA64_PFM_MASK;
  uint64_t b0 = env->b[0] & ~0xFULL;

  if (env->rse_stack_switch_pending && env->rse_stack_switch_b0 == 0) {
      int boundary = -1;

      if (env->rse_stack_switch_bsp == (bsp & ~UINT64_C(0x7)) &&
          env->rse_frames && env->rse_depth > 0) {
          const struct IA64RSEReturnFrameView view = {
              .base = env->rse_frames,
              .count = env->rse_depth,
              .stride = sizeof(*env->rse_frames),
              .cfm_offset = offsetof(struct IA64RSEFrame, cfm),
              .ret_addr_offset = offsetof(struct IA64RSEFrame, ret_addr),
              .bsp_offset = offsetof(struct IA64RSEFrame, bsp),
          };
          uint64_t top_ret = env->rse_frames[env->rse_depth - 1].ret_addr;

          boundary = ia64_rse_bind_stack_switch_return(
              &view, bsp, pfs_cfm, b0, top_ret);
      }

      if (boundary >= 0) {
          env->rse_stack_switch_pfs = pfs_cfm;
          env->rse_stack_switch_b0 = b0;
          ia64_rse_strict_trace(env, "stack_switch_bind", b0, pfs_cfm);
      } else {
          ia64_rse_cancel_stack_switch(env);
      }
  }

  bool stack_switch = ia64_rse_stack_switch_matches(env, bsp, b0,
                                                    pfs_cfm);
C
texts[:helper] = replace_once!(texts[:helper], ret_anchor, ret_replacement,
                               "target/ia64/helper.c br.ret bind")

test_functions = <<~'C'

  static void test_stack_switch_deferred_firmware_order(void)
  {
      TestIA64RSEState env;
      struct IA64RSEFrame frames[3];
      struct IA64RSEReturnFrameView view;

      init_frames(&env, frames, G_N_ELEMENTS(frames));
      frames[0].bsp = 0x1ffc2000;
      frames[0].cfm = 0x201;
      frames[0].ret_addr = 0x1ff10000;
      frames[1].bsp = 0x1ffc3088;
      frames[1].cfm = 0x30a;
      frames[1].ret_addr = 0x1ff3d1d0;
      frames[2].bsp = 0x1ffc30b8;
      frames[2].cfm = 0x30a;
      frames[2].ret_addr = 0x1ff3e780;
      view = (struct IA64RSEReturnFrameView) {
          .base = env.rse_frames,
          .count = env.rse_depth,
          .stride = sizeof(*env.rse_frames),
          .cfm_offset = offsetof(struct IA64RSEFrame, cfm),
          .ret_addr_offset = offsetof(struct IA64RSEFrame, ret_addr),
          .bsp_offset = offsetof(struct IA64RSEFrame, bsp),
      };

      g_assert_true(ia64_rse_should_arm_stack_switch(
          0x1ffc30b8, 0x1ffc3088, 0x1ffc30b8, env.rse_depth,
          true, false));
      g_assert_cmpint(ia64_rse_bind_stack_switch_return(
          &view, 0x1ffc3088, 0x30a, 0x1ff3d1d0, 0x1ff3e780), ==, 1);
  }

  static void test_stack_switch_deferred_rejects_ordinary_return(void)
  {
      TestIA64RSEState env;
      struct IA64RSEFrame frames[1];
      struct IA64RSEReturnFrameView view;

      init_frames(&env, frames, G_N_ELEMENTS(frames));
      frames[0].bsp = 0x1ffc3088;
      frames[0].cfm = 0x30a;
      frames[0].ret_addr = 0x1ff3e780;
      view = (struct IA64RSEReturnFrameView) {
          .base = env.rse_frames,
          .count = env.rse_depth,
          .stride = sizeof(*env.rse_frames),
          .cfm_offset = offsetof(struct IA64RSEFrame, cfm),
          .ret_addr_offset = offsetof(struct IA64RSEFrame, ret_addr),
          .bsp_offset = offsetof(struct IA64RSEFrame, bsp),
      };
      g_assert_cmpint(ia64_rse_bind_stack_switch_return(
          &view, 0x1ffc3088, 0x30a, 0x1ff3e780, 0x1ff3e780), ==, -1);
  }
C
texts[:test] = insert_once!(texts[:test], "\nint main(int argc, char **argv)\n",
                            test_functions,
                            "tests/unit/test-ia64-rse.c functions")
registrations = <<~'C'.indent(4)
  g_test_add_func("/ia64/rse/stack-switch-deferred-firmware-order",
                  test_stack_switch_deferred_firmware_order);
  g_test_add_func("/ia64/rse/stack-switch-deferred-ordinary-return",
                  test_stack_switch_deferred_rejects_ordinary_return);
C
texts[:test] = insert_once!(texts[:test], "    return g_test_run();\n",
                            registrations,
                            "tests/unit/test-ia64-rse.c registrations")

changed = texts.keys.select { |key| texts[key] != originals[key] }
abort "no changes prepared" if changed.empty?

if options[:apply]
  changed.each { |key| files[key].write(texts[key]) }
  puts "applied deferred RSE repair to #{changed.length} files"
else
  puts "check passed; deferred RSE repair would update #{changed.length} files"
end
changed.each { |key| puts "  #{files[key].relative_path_from(root)}" }
