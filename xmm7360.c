// vim: noet ts=8 sw=8

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>

MODULE_LICENSE("Dual BSD/GPL");

static struct pci_device_id xmm7360_ids[] = {
	{ PCI_DEVICE(0x8086, 0x7360), },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, xmm7360_ids);

struct xmm_dev {
	struct device *dev;
	struct pci_dev *pci_dev;

	volatile uint32_t *bar0, *bar2;

	int irq[4];
};

static irqreturn_t xmm7360_irq0(int irq, void *dev_id) {
	struct xmm_dev *xmm = dev_id;
	int id;

	pr_info("xmm irq0\n");
	return IRQ_HANDLED;
}

static irqreturn_t xmm7360_irq(int irq, void *dev) {
	pr_info("\n\n\nxmm irq!!! %d %p\n", irq, dev);
	return IRQ_HANDLED;
}

static irq_handler_t xmm7360_irq_handlers[] = {
	xmm7360_irq0,
	xmm7360_irq,
	xmm7360_irq,
	xmm7360_irq,
};

static int xmm7360_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct xmm_dev *xmm = kzalloc(sizeof(struct xmm_dev), GFP_KERNEL);
	int i, ret;
	u32 status;

	xmm->pci_dev = dev;
	xmm->dev = &dev->dev;

	if (!xmm) {
		dev_err(&(dev->dev), "kzalloc\n");
		return -ENOMEM;
	}

	ret = pci_enable_device(dev);
	if (ret) {
		dev_err(&(dev->dev), "pci_enable_device\n");
		goto fail;
	}
	pci_set_master(dev);

	ret = pci_request_region(dev, 0, "xmm0");
	if (ret) {
		dev_err(&(dev->dev), "pci_request_region(0)\n");
		goto fail;
	}
	xmm->bar0 = pci_iomap(dev, 0, pci_resource_len(dev, 0));

	ret = pci_request_region(dev, 2, "xmm2");
	if (ret) {
		dev_err(&(dev->dev), "pci_request_region(2)\n");
		goto fail;
	}
	xmm->bar2 = pci_iomap(dev, 2, pci_resource_len(dev, 2));

	ret = pci_alloc_irq_vectors(dev, 4, 4, PCI_IRQ_MSI | PCI_IRQ_MSIX);
	if (ret < 0) {
		dev_err(&(dev->dev), "pci_alloc_irq_vectors\n");
		goto fail;
	}

	for (i=0; i<4; i++) {
		xmm->irq[i] = pci_irq_vector(dev, i);
		ret = request_irq(xmm->irq[i], xmm7360_irq_handlers[i], 0, "xmm7360", xmm);
		if (ret) {
			dev_err(&(dev->dev), "request_irq\n");
			goto fail;
		}
	}

	pci_set_drvdata(dev, xmm);

	// Wait for modem core to boot if it's still coming up.
	// Typically ~5 seconds
	for (i=0; i<100; i++) {
		status = xmm->bar2[0];
		if (status == 0x600df00d)
			break;
		if (status == 0xbadc0ded) {
			dev_err(xmm->dev, "Modem is in crash dump state, aborting probe\n");
			ret = -EINVAL;
			goto fail;
		}
		mdelay(200);
	}

	if (status != 0x600df00d) {
		dev_err(xmm->dev, "Unknown modem status: 0x%08x\n", status);
		ret = -EINVAL;
		goto fail;
	}

	pci_set_dma_mask(dev, 0xffffffffffffffff);
	if (ret) {
		dev_err(xmm->dev, "Cannot set DMA mask\n");
		return ret;
	}
	dma_set_coherent_mask(xmm->dev, 0xffffffffffffffff);

	return ret;

fail:
	for (i=0; i<4; i++) {
		if (xmm->irq[i])
			free_irq(xmm->irq[i], xmm);
	}
	pci_release_region(dev, 2);
	pci_release_region(dev, 0);
	pci_free_irq_vectors(dev);
	pci_disable_device(dev);

	kfree(xmm);
	return ret;
}

static void xmm7360_remove(struct pci_dev *dev) {
	struct xmm_dev *xmm = pci_get_drvdata(dev);
	int i;
	for (i=0; i<4; i++) {
		if (xmm->irq[i])
			free_irq(xmm->irq[i], xmm);
	}
	pci_free_irq_vectors(dev);
	pci_release_region(dev, 0);
	pci_release_region(dev, 2);
	pci_disable_device(dev);
	kfree(xmm);
}

static struct pci_driver xmm7360_driver = {
	.name		= "xmm7360",
	.id_table	= xmm7360_ids,
	.probe		= xmm7360_probe,
	.remove		= xmm7360_remove,
};

static int xmm7360_init(void)
{
	int ret;

	ret = pci_register_driver(&xmm7360_driver);
	if (ret)
		return ret;

	return 0;
}

static void xmm7360_exit(void)
{
	pci_unregister_driver(&xmm7360_driver);
}

module_init(xmm7360_init);
module_exit(xmm7360_exit);
