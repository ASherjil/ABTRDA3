#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/export-internal.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

#ifdef CONFIG_UNWINDER_ORC
#include <asm/orc_header.h>
ORC_HEADER;
#endif

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
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

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xc43eb1a8, "__platform_driver_register" },
	{ 0xcd8bb075, "uio_unregister_device" },
	{ 0x992ebdca, "dma_free_attrs" },
	{ 0xb6e6d99d, "clk_disable" },
	{ 0xb077e70a, "clk_unprepare" },
	{ 0x122c3a7e, "_printk" },
	{ 0xa3c987cb, "platform_driver_unregister" },
	{ 0x8d7b06a7, "devm_kmalloc" },
	{ 0xfe674336, "devm_platform_get_and_ioremap_resource" },
	{ 0xbc58cc5b, "devm_clk_get" },
	{ 0x8ebe4b61, "devm_clk_get_optional" },
	{ 0x7c9a7371, "clk_prepare" },
	{ 0x815588a6, "clk_enable" },
	{ 0xeebc492e, "dma_set_mask" },
	{ 0xc505d0de, "dma_set_coherent_mask" },
	{ 0x7e0eeaaa, "dma_alloc_attrs" },
	{ 0x316b68f9, "__uio_register_device" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0xb15cddc5, "module_layout" },
};

MODULE_INFO(depends, "");

