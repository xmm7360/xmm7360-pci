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

/* Command ring, which is used to configure the queue pairs */
struct cmd_ring_entry {
	dma_addr_t ptr;
	u16 len;
	u8 parm;
	u8 cmd;
	u32 unk1, unk2, flags;
};

#define CMD_QP_OPEN	1
#define CMD_QP_CLOSE	3
#define CMD_WAKEUP	4

#define CMD_FLAG_DONE	1
#define CMD_FLAG_READY	2

/* Transfer descriptors used on the Tx and Rx rings of each queue pair */
struct td_ring_entry {
	dma_addr_t addr;
	u16 length;
	u16 flags;
	u32 unk;
};

#define TD_FLAG_COMPLETE 0x200

/* Root configuration object. This contains pointers to all of the control
 * structures that the modem will interact with.
 */
struct control {
	dma_addr_t status;
	dma_addr_t s_wptr, s_rptr;
	dma_addr_t c_wptr, c_rptr;
	dma_addr_t c_ring;
	u16 c_ring_size;
	u16 unk;
};

struct status {
	u32 code;
	u32 mode;
	u32 asleep;
	u32 pad;
};

#define CMD_RING_SIZE 0x80

/* All of the control structures can be packed into one page of RAM. */
struct control_page {
	struct control ctl;
	// Status words - written by modem.
	volatile struct status status;
	// Slave ring write/read pointers.
	volatile u32 s_wptr[16], s_rptr[16];
	// Command ring write/read pointers.
	volatile u32 c_wptr, c_rptr;
	// Command ring entries.
	volatile struct cmd_ring_entry c_ring[CMD_RING_SIZE];
};

#define BAR0_MODE	0x0c
#define BAR0_DOORBELL	0x04
#define BAR0_WAKEUP	0x14

#define DOORBELL_QP	0
#define DOORBELL_CMD	1

#define BAR2_STATUS	0x00
#define BAR2_MODE	0x18
#define BAR2_CONTROL	0x19
#define BAR2_CONTROLH	0x1a

#define BAR2_BLANK0	0x1b
#define BAR2_BLANK1	0x1c
#define BAR2_BLANK2	0x1d
#define BAR2_BLANK3	0x1e


struct xmm_dev {
	struct device *dev;
	struct pci_dev *pci_dev;

	volatile uint32_t *bar0, *bar2;

	int irq[4];

	volatile struct control_page *cp;
	dma_addr_t cp_phys;

};

static void xmm7360_dump(struct xmm_dev *xmm)
{
	pr_info("xmm %08x slp %d cmd %d:%d\n", xmm->cp->status.code, xmm->cp->status.asleep, xmm->cp->c_rptr, xmm->cp->c_wptr);
}

static void xmm7360_ding(struct xmm_dev *xmm, int bell)
{
	if (xmm->cp->status.asleep)
		xmm->bar0[BAR0_WAKEUP] = 1;
	xmm->bar0[BAR0_DOORBELL] = bell;
}

static int xmm7360_cmd_ring_submit(struct xmm_dev *xmm, u8 cmd, u8 parm, u16 len, dma_addr_t ptr)
{
	u8 wptr = xmm->cp->c_wptr;
	u8 new_wptr = (wptr + 1) % CMD_RING_SIZE;
	if (new_wptr == xmm->cp->c_rptr)	// ring full
		return -EAGAIN;

	pr_info("xmm7360_cmd_ring_submit %x %02x %04x %llx\n", cmd, parm, len, ptr);

	xmm->cp->c_ring[wptr].ptr = ptr;
	xmm->cp->c_ring[wptr].cmd = cmd;
	xmm->cp->c_ring[wptr].parm = parm;
	xmm->cp->c_ring[wptr].len = len;
	xmm->cp->c_ring[wptr].unk1 = 0;
	xmm->cp->c_ring[wptr].unk2 = 0;
	xmm->cp->c_ring[wptr].flags = CMD_FLAG_READY;

	xmm->cp->c_wptr = new_wptr;

	return 0;
}

static int xmm7360_cmd_ring_init(struct xmm_dev *xmm) {
	int timeout;
	int ret;

	xmm->cp = dma_alloc_coherent(xmm->dev, sizeof(struct control_page), &xmm->cp_phys, GFP_KERNEL);

	xmm->cp->ctl.status = xmm->cp_phys + offsetof(struct control_page, status);
	xmm->cp->ctl.s_wptr = xmm->cp_phys + offsetof(struct control_page, s_wptr);
	xmm->cp->ctl.s_rptr = xmm->cp_phys + offsetof(struct control_page, s_rptr);
	xmm->cp->ctl.c_wptr = xmm->cp_phys + offsetof(struct control_page, c_wptr);
	xmm->cp->ctl.c_rptr = xmm->cp_phys + offsetof(struct control_page, c_rptr);
	xmm->cp->ctl.c_ring = xmm->cp_phys + offsetof(struct control_page, c_ring);
	xmm->cp->ctl.c_ring_size = CMD_RING_SIZE;

	xmm->bar2[BAR2_CONTROL] = xmm->cp_phys;
	xmm->bar2[BAR2_CONTROLH] = xmm->cp_phys >> 32;

	xmm->bar0[BAR0_MODE] = 1;


	timeout = 100;
	while (xmm->bar2[BAR2_MODE] == 0 && --timeout)
		msleep(10);

	if (!timeout)
		return -ETIMEDOUT;

	xmm->bar2[BAR2_BLANK0] = 0;
	xmm->bar2[BAR2_BLANK1] = 0;
	xmm->bar2[BAR2_BLANK2] = 0;
	xmm->bar2[BAR2_BLANK3] = 0;

	xmm->bar0[BAR0_MODE] = 2;	// enable intrs?

	timeout = 100;
	while (xmm->bar2[BAR2_MODE] != 2 && !--timeout)
		msleep(10);

	if (!timeout)
		return -ETIMEDOUT;

	ret = xmm7360_cmd_ring_submit(xmm, CMD_WAKEUP, 0, 1, 0);
	if (ret)
		return ret;
	ret = xmm7360_cmd_ring_submit(xmm, 0xf0, 0x80, 0, 0);
	if (ret)
		return ret;

	xmm7360_ding(xmm, DOORBELL_CMD);

	return 0;
}

static void xmm7360_cmd_ring_free(struct xmm_dev *xmm) {
	if (xmm->bar0)
		xmm->bar0[BAR0_MODE] = 0;
	if (xmm->cp)
		dma_free_coherent(xmm->dev, sizeof(struct control_page), (void*)xmm->cp, xmm->cp_phys);
	xmm->cp = NULL;
	return;
}

static irqreturn_t xmm7360_irq0(int irq, void *dev_id) {
	struct xmm_dev *xmm = dev_id;
	int id;

	pr_info("xmm irq0\n");
	xmm7360_dump(xmm);
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
		goto fail;
	}
	dma_set_coherent_mask(xmm->dev, 0xffffffffffffffff);

	ret = xmm7360_cmd_ring_init(xmm);
	if (ret) {
		dev_err(xmm->dev, "Could not bring up command ring\n");
		goto fail;
	}

	return ret;

fail:
	xmm7360_cmd_ring_free(xmm);

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
	xmm7360_cmd_ring_free(xmm);

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
