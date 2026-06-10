// SPDX-License-Identifier: GPL-2.0
/*
 * gem_uio — UIO driver for Cadence GEM on Zynq UltraScale+
 *
 * Provides:
 *   - mem[0]: GEM register window (UIO_MEM_PHYS, mapped non-cached by the
 *            UIO mmap default — userspace can use either /dev/mem or this).
 *   - mem[1]: A coherent (non-cached) DMA buffer for descriptor rings.
 *            This is the canonical fix for the cache-line-vs-DMA race on
 *            ARM with non-coherent DMA: SW writebacks of one descriptor
 *            cannot stomp HW writes to neighbouring descriptors when the
 *            ring lives in a non-cached region.
 *
 * Plus holds the GEM's clocks enabled via the kernel's CCF, so the ZynqMP
 * PMU firmware's clock/power-domain refcounts stay consistent during the
 * PMD run (otherwise macb_probe's clk_prepare_enable() can hang).
 *
 * Usage (no device-tree changes required):
 *
 *   insmod gem_uio.ko
 *   echo gem_uio > /sys/bus/platform/devices/ff0b0000.ethernet/driver_override
 *   echo ff0b0000.ethernet > /sys/bus/platform/drivers/macb/unbind
 *   echo ff0b0000.ethernet > /sys/bus/platform/drivers/gem_uio/bind
 *   # ... PMD operates on /dev/uio<N> ...
 *   echo ff0b0000.ethernet > /sys/bus/platform/drivers/gem_uio/unbind
 *   echo > /sys/bus/platform/devices/ff0b0000.ethernet/driver_override
 *   echo ff0b0000.ethernet > /sys/bus/platform/drivers/macb/bind
 *   rmmod gem_uio
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uio_driver.h>

#define DRIVER_NAME "gem_uio"

/* Sized to fit 4 rings × 256 descriptors × 16 B = 16 KiB.  We allocate
 * 64 KiB for headroom and to align cleanly with typical CMA block sizes. */
#define GEM_UIO_DMA_SIZE  (64 * 1024)

struct gem_uio_priv {
	struct uio_info info;
	struct clk *pclk;
	struct clk *hclk;
	struct clk *tx_clk;
	struct clk *rx_clk;
	struct clk *tsu_clk;

	void       *dma_vaddr;
	dma_addr_t  dma_paddr;
	size_t      dma_size;
};

static int gem_uio_probe(struct platform_device *pdev)
{
	struct gem_uio_priv *priv;
	struct resource *res;
	void __iomem *mmio;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* Map the GEM register window — exposed as mem[0] for completeness */
	mmio = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(mmio))
		return PTR_ERR(mmio);

	/* Acquire clocks — same names the macb driver uses */
	priv->pclk = devm_clk_get(&pdev->dev, "pclk");
	if (IS_ERR(priv->pclk)) {
		pr_err("gem_uio: failed to get pclk (%ld)\n", PTR_ERR(priv->pclk));
		return PTR_ERR(priv->pclk);
	}

	priv->hclk = devm_clk_get(&pdev->dev, "hclk");
	if (IS_ERR(priv->hclk)) {
		pr_err("gem_uio: failed to get hclk (%ld)\n", PTR_ERR(priv->hclk));
		return PTR_ERR(priv->hclk);
	}

	priv->tx_clk = devm_clk_get_optional(&pdev->dev, "tx_clk");
	if (IS_ERR(priv->tx_clk))
		return PTR_ERR(priv->tx_clk);

	priv->rx_clk = devm_clk_get_optional(&pdev->dev, "rx_clk");
	if (IS_ERR(priv->rx_clk))
		return PTR_ERR(priv->rx_clk);

	priv->tsu_clk = devm_clk_get_optional(&pdev->dev, "tsu_clk");
	if (IS_ERR(priv->tsu_clk))
		return PTR_ERR(priv->tsu_clk);

	/* Enable all clocks via the kernel CCF — keeps the firmware refcount
	 * consistent so macb_probe doesn't hang on rebind later. */
	ret = clk_prepare_enable(priv->pclk);
	if (ret) {
		pr_err("gem_uio: failed to enable pclk (%d)\n", ret);
		return ret;
	}
	ret = clk_prepare_enable(priv->hclk);
	if (ret) {
		pr_err("gem_uio: failed to enable hclk (%d)\n", ret);
		goto err_disable_pclk;
	}
	ret = clk_prepare_enable(priv->tx_clk);
	if (ret) {
		pr_err("gem_uio: failed to enable tx_clk (%d)\n", ret);
		goto err_disable_hclk;
	}
	ret = clk_prepare_enable(priv->rx_clk);
	if (ret) {
		pr_err("gem_uio: failed to enable rx_clk (%d)\n", ret);
		goto err_disable_txclk;
	}
	ret = clk_prepare_enable(priv->tsu_clk);
	if (ret) {
		pr_err("gem_uio: failed to enable tsu_clk (%d)\n", ret);
		goto err_disable_rxclk;
	}

	/* Zynq UltraScale+ supports 40-bit DMA addresses.  This call is also
	 * required so dma_alloc_coherent can succeed against a platform device
	 * that doesn't (yet) have a DMA mask set by the device tree. */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (ret) {
		pr_err("gem_uio: dma_set_mask_and_coherent failed (%d)\n", ret);
		goto err_disable_tsuclk;
	}

	/* Allocate the coherent DMA buffer that backs userspace descriptor
	 * rings.  On ARM with non-coherent DMA, dma_alloc_coherent gives us
	 * memory that is mapped non-cached (or write-combining) in both kernel
	 * and userspace views — no software cache maintenance needed. */
	priv->dma_size  = GEM_UIO_DMA_SIZE;
	priv->dma_vaddr = dma_alloc_coherent(&pdev->dev, priv->dma_size,
					     &priv->dma_paddr, GFP_KERNEL);
	if (!priv->dma_vaddr) {
		pr_err("gem_uio: dma_alloc_coherent(%zu) failed\n", priv->dma_size);
		ret = -ENOMEM;
		goto err_disable_tsuclk;
	}

	priv->info.name    = DRIVER_NAME;
	priv->info.version = "1.0";
	priv->info.irq     = UIO_IRQ_NONE;

	/* mem[0] — GEM register window */
	priv->info.mem[0].name    = "gem_regs";
	priv->info.mem[0].addr    = res->start;
	priv->info.mem[0].size    = resource_size(res);
	priv->info.mem[0].memtype = UIO_MEM_PHYS;

	/* mem[1] — coherent DMA buffer for descriptor rings.  The default
	 * UIO mmap callback applies pgprot_noncached to UIO_MEM_PHYS regions,
	 * matching the kernel's coherent mapping. */
	priv->info.mem[1].name    = "dma_descs";
	priv->info.mem[1].addr    = priv->dma_paddr;
	priv->info.mem[1].size    = priv->dma_size;
	priv->info.mem[1].memtype = UIO_MEM_PHYS;

	ret = uio_register_device(&pdev->dev, &priv->info);
	if (ret) {
		pr_err("gem_uio: failed to register UIO device (%d)\n", ret);
		goto err_free_dma;
	}

	platform_set_drvdata(pdev, priv);
	pr_info("gem_uio: regs=0x%llx (%llu B), dma_descs=0x%llx (%zu B), clocks enabled\n",
		(unsigned long long)res->start,
		(unsigned long long)resource_size(res),
		(unsigned long long)priv->dma_paddr, priv->dma_size);
	return 0;

err_free_dma:
	dma_free_coherent(&pdev->dev, priv->dma_size, priv->dma_vaddr, priv->dma_paddr);
err_disable_tsuclk:
	clk_disable_unprepare(priv->tsu_clk);
err_disable_rxclk:
	clk_disable_unprepare(priv->rx_clk);
err_disable_txclk:
	clk_disable_unprepare(priv->tx_clk);
err_disable_hclk:
	clk_disable_unprepare(priv->hclk);
err_disable_pclk:
	clk_disable_unprepare(priv->pclk);
	return ret;
}

static int gem_uio_remove(struct platform_device *pdev)
{
	struct gem_uio_priv *priv = platform_get_drvdata(pdev);

	uio_unregister_device(&priv->info);

	dma_free_coherent(&pdev->dev, priv->dma_size, priv->dma_vaddr, priv->dma_paddr);

	clk_disable_unprepare(priv->tsu_clk);
	clk_disable_unprepare(priv->rx_clk);
	clk_disable_unprepare(priv->tx_clk);
	clk_disable_unprepare(priv->hclk);
	clk_disable_unprepare(priv->pclk);

	pr_info("gem_uio: clocks disabled, DMA freed, UIO unregistered\n");
	return 0;
}

static struct platform_driver gem_uio_driver = {
	.probe  = gem_uio_probe,
	.remove = gem_uio_remove,
	.driver = {
		.name = DRIVER_NAME,
	},
};
module_platform_driver(gem_uio_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Areeb Sherjil");
MODULE_DESCRIPTION("UIO driver for Cadence GEM — coherent DMA descriptor pool + clocks");
