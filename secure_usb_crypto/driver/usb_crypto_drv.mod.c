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
	{ 0xc1080ff1, "crypto_cipher_decrypt_one" },
	{ 0x092a35a2, "_copy_to_user" },
	{ 0xbd03ed67, "__ref_stack_chk_guard" },
	{ 0x47886e07, "usb_register_notify" },
	{ 0x1404c363, "usb_for_each_dev" },
	{ 0xadb55ac9, "usb_register_driver" },
	{ 0xaca12394, "misc_register" },
	{ 0xa8f96c6e, "usb_deregister" },
	{ 0x47886e07, "usb_unregister_notify" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0x27683a56, "memset" },
	{ 0xd5ad82a1, "misc_deregister" },
	{ 0xe54e0a6b, "__fortify_panic" },
	{ 0xa53f4e29, "memcpy" },
	{ 0x90a48d82, "__ubsan_handle_out_of_bounds" },
	{ 0xe4de56b4, "__ubsan_handle_load_invalid_value" },
	{ 0xd272d446, "__fentry__" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0xe8213e80, "_printk" },
	{ 0xf46d5bf3, "mutex_lock" },
	{ 0x546c19d9, "validate_usercopy_range" },
	{ 0xa61fd7aa, "__check_object_size" },
	{ 0x092a35a2, "_copy_from_user" },
	{ 0xf46d5bf3, "mutex_unlock" },
	{ 0x214d2136, "crypto_alloc_base" },
	{ 0xafac3687, "crypto_cipher_setkey" },
	{ 0xc1080ff1, "crypto_cipher_encrypt_one" },
	{ 0x026921de, "crypto_destroy_tfm" },
	{ 0xbebe66ff, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0xc1080ff1,
	0x092a35a2,
	0xbd03ed67,
	0x47886e07,
	0x1404c363,
	0xadb55ac9,
	0xaca12394,
	0xa8f96c6e,
	0x47886e07,
	0xd272d446,
	0x27683a56,
	0xd5ad82a1,
	0xe54e0a6b,
	0xa53f4e29,
	0x90a48d82,
	0xe4de56b4,
	0xd272d446,
	0xd272d446,
	0xe8213e80,
	0xf46d5bf3,
	0x546c19d9,
	0xa61fd7aa,
	0x092a35a2,
	0xf46d5bf3,
	0x214d2136,
	0xafac3687,
	0xc1080ff1,
	0x026921de,
	0xbebe66ff,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"crypto_cipher_decrypt_one\0"
	"_copy_to_user\0"
	"__ref_stack_chk_guard\0"
	"usb_register_notify\0"
	"usb_for_each_dev\0"
	"usb_register_driver\0"
	"misc_register\0"
	"usb_deregister\0"
	"usb_unregister_notify\0"
	"__stack_chk_fail\0"
	"memset\0"
	"misc_deregister\0"
	"__fortify_panic\0"
	"memcpy\0"
	"__ubsan_handle_out_of_bounds\0"
	"__ubsan_handle_load_invalid_value\0"
	"__fentry__\0"
	"__x86_return_thunk\0"
	"_printk\0"
	"mutex_lock\0"
	"validate_usercopy_range\0"
	"__check_object_size\0"
	"_copy_from_user\0"
	"mutex_unlock\0"
	"crypto_alloc_base\0"
	"crypto_cipher_setkey\0"
	"crypto_cipher_encrypt_one\0"
	"crypto_destroy_tfm\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");

MODULE_ALIAS("usb:v1A81p101Fd*dc*dsc*dp*ic*isc*ip*in*");

MODULE_INFO(srcversion, "DDB4C0CC54F8CF59BA60A0F");
