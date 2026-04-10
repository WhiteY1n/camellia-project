#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x680628e7, "ktime_get_real_ts64" },
	{ 0x5160854e, "vscnprintf" },
	{ 0xe1e1f979, "_raw_spin_lock_irqsave" },
	{ 0x81a1a811, "_raw_spin_unlock_irqrestore" },
	{ 0x90a48d82, "__ubsan_handle_out_of_bounds" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0xe4de56b4, "__ubsan_handle_load_invalid_value" },
	{ 0x8b816bf6, "input_close_device" },
	{ 0x8b816bf6, "input_unregister_handle" },
	{ 0xcb8b6ec6, "kfree" },
	{ 0xca438c02, "input_inject_event" },
	{ 0xbd03ed67, "random_kmalloc_seed" },
	{ 0xfaabfe5e, "kmalloc_caches" },
	{ 0xc064623f, "__kmalloc_cache_noprof" },
	{ 0x52c86b3f, "input_register_handle" },
	{ 0x52c86b3f, "input_open_device" },
	{ 0xd710adbf, "__kmalloc_large_noprof" },
	{ 0xb61837ba, "seq_printf" },
	{ 0xaa9a3b35, "seq_read" },
	{ 0x253f0c1d, "seq_lseek" },
	{ 0x34d5450c, "single_release" },
	{ 0xd272d446, "__fentry__" },
	{ 0x80222ceb, "proc_create" },
	{ 0x7c8d3ace, "input_register_handler" },
	{ 0xe8213e80, "_printk" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0xc0f19660, "remove_proc_entry" },
	{ 0xe931a49e, "single_open" },
	{ 0x73cb05d5, "input_unregister_handler" },
	{ 0xbd03ed67, "__ref_stack_chk_guard" },
	{ 0xbebe66ff, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0x680628e7,
	0x5160854e,
	0xe1e1f979,
	0x81a1a811,
	0x90a48d82,
	0xd272d446,
	0xe4de56b4,
	0x8b816bf6,
	0x8b816bf6,
	0xcb8b6ec6,
	0xca438c02,
	0xbd03ed67,
	0xfaabfe5e,
	0xc064623f,
	0x52c86b3f,
	0x52c86b3f,
	0xd710adbf,
	0xb61837ba,
	0xaa9a3b35,
	0x253f0c1d,
	0x34d5450c,
	0xd272d446,
	0x80222ceb,
	0x7c8d3ace,
	0xe8213e80,
	0xd272d446,
	0xc0f19660,
	0xe931a49e,
	0x73cb05d5,
	0xbd03ed67,
	0xbebe66ff,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"ktime_get_real_ts64\0"
	"vscnprintf\0"
	"_raw_spin_lock_irqsave\0"
	"_raw_spin_unlock_irqrestore\0"
	"__ubsan_handle_out_of_bounds\0"
	"__stack_chk_fail\0"
	"__ubsan_handle_load_invalid_value\0"
	"input_close_device\0"
	"input_unregister_handle\0"
	"kfree\0"
	"input_inject_event\0"
	"random_kmalloc_seed\0"
	"kmalloc_caches\0"
	"__kmalloc_cache_noprof\0"
	"input_register_handle\0"
	"input_open_device\0"
	"__kmalloc_large_noprof\0"
	"seq_printf\0"
	"seq_read\0"
	"seq_lseek\0"
	"single_release\0"
	"__fentry__\0"
	"proc_create\0"
	"input_register_handler\0"
	"_printk\0"
	"__x86_return_thunk\0"
	"remove_proc_entry\0"
	"single_open\0"
	"input_unregister_handler\0"
	"__ref_stack_chk_guard\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");

MODULE_ALIAS("input:b*v*p*e*-e*1,*2,*k*110,*111,*112,*r*0,*1,*8,*a*m*l*s*f*w*");

MODULE_INFO(srcversion, "73C5A4CD3A255B0ACE891DF");
