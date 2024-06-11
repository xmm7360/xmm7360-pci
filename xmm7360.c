// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Device driver for Intel XMM7360 LTE modems, eg. Fibocom L850-GL.
 * Written by James Wah
 *
 * Development of this driver was supported by genua GmbH
 *
 * This work is dual-licensed under BSD 3-clause and GPL 2.0.
 * You can choose between one of them if you use this work.
 *
 * Copyright (c) 2020 genua GmbH <info@genua.de>
 * Copyright (c) 2020 James Wah <xmm7360@james.wah.net.au>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS”
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/version.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/if.h>
#include <linux/if_arp.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/skbuff.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <net/rtnetlink.h>

MODULE_LICENSE("Dual BSD/GPL");

static struct pci_device_id xmm7360_ids[] = { {
						      PCI_DEVICE(0x8086,
								 0x7360),
					      },
					      {
						      0,
					      } };

MODULE_DEVICE_TABLE(pci, xmm7360_ids);

#define XMM7360_IOCTL_GET_PAGE_SIZE _IOC(_IOC_READ, 'x', 0xc0, sizeof(u32))

static dev_t xmm_base;

static struct tty_driver *xmm7360_tty_driver;

/*
 * The XMM7360 communicates via DMA ring buffers. It has one
 * command ring, plus sixteen transfer descriptor (TD)
 * rings. The command ring is mainly used to configure and
 * deconfigure the TD rings.
 *
 * The 16 TD rings form 8 queue pairs (QP). For example, QP
 * 0 uses ring 0 for host->device, and ring 1 for
 * device->host.
 *
 * The known queue pair functions are as follows:
 *
 * 0:	Mux (Raw IP packets, amongst others)
 * 1:	RPC (funky command protocol based in part on ASN.1 BER)
 * 2:	AT trace? port; does not accept commands after init
 * 4:	AT command port
 * 7:	AT command port
 *
 */

/* Command ring, which is used to configure the queue pairs */
struct cmd_ring_entry {
	dma_addr_t ptr;
	u16 len;
	u8 parm;
	u8 cmd;
	u32 extra;
	u32 unk, flags;
};

#define CMD_RING_OPEN 1
#define CMD_RING_CLOSE 2
#define CMD_RING_FLUSH 3
#define CMD_WAKEUP 4

#define CMD_FLAG_DONE 1
#define CMD_FLAG_READY 2

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

#define BAR0_MODE 0x0c
#define BAR0_DOORBELL 0x04
#define BAR0_WAKEUP 0x14

#define DOORBELL_TD 0
#define DOORBELL_CMD 1

#define BAR2_STATUS 0x00
#define BAR2_MODE 0x18
#define BAR2_CONTROL 0x19
#define BAR2_CONTROLH 0x1a

#define BAR2_BLANK0 0x1b
#define BAR2_BLANK1 0x1c
#define BAR2_BLANK2 0x1d
#define BAR2_BLANK3 0x1e

/* There are 16 TD rings: a Tx and Rx ring for each queue pair */
struct td_ring {
	u8 depth;
	u8 last_handled;
	u16 page_size;

	struct td_ring_entry *tds;
	dma_addr_t tds_phys;

	// One page of page_size per td
	void **pages;
	dma_addr_t *pages_phys;
};

#define TD_MAX_PAGE_SIZE 16384

struct queue_pair {
	struct xmm_dev *xmm;
	u8 depth;
	u16 page_size;
	struct cdev cdev;
	struct tty_port port;
	int tty_index;
	int tty_needs_wake;
	struct device dev;
	int num;
	int open;
	wait_queue_head_t wq;
	struct mutex lock;
	unsigned char user_buf[TD_MAX_PAGE_SIZE];
};

struct xmm_dev {
	struct device *dev;
	struct pci_dev *pci_dev;

	volatile uint32_t *bar0, *bar2;

	int irq;
	wait_queue_head_t wq;

	struct work_struct init_work;

	volatile struct control_page *cp;
	dma_addr_t cp_phys;

	struct td_ring td_ring[16];

	struct queue_pair qp[8];

	struct xmm_net *net;
	struct net_device *netdev;

	int error;
	int card_num;
	int num_ttys;
};

struct mux_bounds {
	uint32_t offset;
	uint32_t length;
};

struct mux_first_header {
	uint32_t tag;
	uint16_t unknown;
	uint16_t sequence;
	uint16_t length;
	uint16_t extra;
	uint16_t next;
	uint16_t pad;
};

struct mux_next_header {
	uint32_t tag;
	uint16_t length;
	uint16_t extra;
	uint16_t next;
	uint16_t pad;
};

#define MUX_MAX_PACKETS 64

struct mux_frame {
	int n_packets, n_bytes, max_size, sequence;
	uint16_t *last_tag_length, *last_tag_next;
	struct mux_bounds bounds[MUX_MAX_PACKETS];
	uint8_t data[TD_MAX_PAGE_SIZE];
};

struct xmm_net {
	struct xmm_dev *xmm;
	struct queue_pair *qp;
	int channel;

	struct sk_buff_head queue;
	struct hrtimer deadline;
	int queued_packets, queued_bytes;

	int sequence;
	spinlock_t lock;
	struct mux_frame frame;
};

static void xmm7360_poll(struct xmm_dev *xmm)
{
	if (xmm->cp->status.code == 0xbadc0ded) {
		dev_err(xmm->dev, "crashed but dma up\n");
		xmm->error = -ENODEV;
	}
	if (xmm->bar2[BAR2_STATUS] != 0x600df00d) {
		dev_err(xmm->dev, "bad status %x\n", xmm->bar2[BAR2_STATUS]);
		xmm->error = -ENODEV;
	}
}

static void xmm7360_ding(struct xmm_dev *xmm, int bell)
{
	if (xmm->cp->status.asleep)
		xmm->bar0[BAR0_WAKEUP] = 1;
	xmm->bar0[BAR0_DOORBELL] = bell;
	xmm7360_poll(xmm);
}

static int xmm7360_cmd_ring_wait(struct xmm_dev *xmm)
{
	// Wait for all commands to complete
	int ret = wait_event_interruptible_timeout(
		xmm->wq, (xmm->cp->c_rptr == xmm->cp->c_wptr) || xmm->error,
		msecs_to_jiffies(1000));
	if (ret == 0)
		return -ETIMEDOUT;
	if (ret < 0)
		return ret;
	return xmm->error;
}

static int xmm7360_cmd_ring_execute(struct xmm_dev *xmm, u8 cmd, u8 parm,
				    u16 len, dma_addr_t ptr, u32 extra)
{
	u8 wptr = xmm->cp->c_wptr;
	u8 new_wptr = (wptr + 1) % CMD_RING_SIZE;
	if (xmm->error)
		return xmm->error;
	if (new_wptr == xmm->cp->c_rptr) // ring full
		return -EAGAIN;

	xmm->cp->c_ring[wptr].ptr = ptr;
	xmm->cp->c_ring[wptr].cmd = cmd;
	xmm->cp->c_ring[wptr].parm = parm;
	xmm->cp->c_ring[wptr].len = len;
	xmm->cp->c_ring[wptr].extra = extra;
	xmm->cp->c_ring[wptr].unk = 0;
	xmm->cp->c_ring[wptr].flags = CMD_FLAG_READY;

	xmm->cp->c_wptr = new_wptr;

	xmm7360_ding(xmm, DOORBELL_CMD);
	return xmm7360_cmd_ring_wait(xmm);
}

static int xmm7360_cmd_ring_init(struct xmm_dev *xmm)
{
	int timeout;
	int ret;

	xmm->cp = dma_alloc_coherent(xmm->dev, sizeof(struct control_page),
				     &xmm->cp_phys, GFP_KERNEL);

	xmm->cp->ctl.status =
		xmm->cp_phys + offsetof(struct control_page, status);
	xmm->cp->ctl.s_wptr =
		xmm->cp_phys + offsetof(struct control_page, s_wptr);
	xmm->cp->ctl.s_rptr =
		xmm->cp_phys + offsetof(struct control_page, s_rptr);
	xmm->cp->ctl.c_wptr =
		xmm->cp_phys + offsetof(struct control_page, c_wptr);
	xmm->cp->ctl.c_rptr =
		xmm->cp_phys + offsetof(struct control_page, c_rptr);
	xmm->cp->ctl.c_ring =
		xmm->cp_phys + offsetof(struct control_page, c_ring);
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

	xmm->bar0[BAR0_MODE] = 2; // enable intrs?

	timeout = 100;
	while (xmm->bar2[BAR2_MODE] != 2 && --timeout)
		msleep(10);

	if (!timeout)
		return -ETIMEDOUT;

	// enable going to sleep when idle
	ret = xmm7360_cmd_ring_execute(xmm, CMD_WAKEUP, 0, 1, 0, 0);
	if (ret)
		return ret;

	return 0;
}

static void xmm7360_cmd_ring_free(struct xmm_dev *xmm)
{
	if (xmm->bar0)
		xmm->bar0[BAR0_MODE] = 0;
	if (xmm->cp)
		dma_free_coherent(xmm->dev, sizeof(struct control_page),
				  (void *)xmm->cp, xmm->cp_phys);
	xmm->cp = NULL;
	return;
}

static int xmm7360_td_ring_create(struct xmm_dev *xmm, u8 ring_id, u8 depth,
				  u16 page_size)
{
	struct td_ring *ring = &xmm->td_ring[ring_id];
	int i;
	int ret;

	BUG_ON(ring->depth);
	BUG_ON(depth & (depth - 1));
	BUG_ON(page_size > TD_MAX_PAGE_SIZE);

	memset(ring, 0, sizeof(struct td_ring));
	ring->depth = depth;
	ring->page_size = page_size;
	ring->tds = dma_alloc_coherent(xmm->dev,
				       sizeof(struct td_ring_entry) * depth,
				       &ring->tds_phys, GFP_KERNEL);

	ring->pages = kzalloc(sizeof(void *) * depth, GFP_KERNEL);
	ring->pages_phys = kzalloc(sizeof(dma_addr_t) * depth, GFP_KERNEL);

	for (i = 0; i < depth; i++) {
		ring->pages[i] = dma_alloc_coherent(xmm->dev, ring->page_size,
						    &ring->pages_phys[i],
						    GFP_KERNEL);
		ring->tds[i].addr = ring->pages_phys[i];
	}

	xmm->cp->s_rptr[ring_id] = xmm->cp->s_wptr[ring_id] = 0;
	ret = xmm7360_cmd_ring_execute(xmm, CMD_RING_OPEN, ring_id, depth,
				       ring->tds_phys, 0x60);
	if (ret)
		return ret;
	return 0;
}

static void xmm7360_td_ring_destroy(struct xmm_dev *xmm, u8 ring_id)
{
	struct td_ring *ring = &xmm->td_ring[ring_id];
	int i, depth = ring->depth;

	if (!depth) {
		WARN_ON(1);
		dev_err(xmm->dev, "Tried destroying empty ring!\n");
		return;
	}

	xmm7360_cmd_ring_execute(xmm, CMD_RING_CLOSE, ring_id, 0, 0, 0);

	for (i = 0; i < depth; i++) {
		dma_free_coherent(xmm->dev, ring->page_size, ring->pages[i],
				  ring->pages_phys[i]);
	}

	kfree(ring->pages_phys);
	kfree(ring->pages);

	dma_free_coherent(xmm->dev, sizeof(struct td_ring_entry) * depth,
			  ring->tds, ring->tds_phys);

	ring->depth = 0;
}

static void xmm7360_td_ring_write(struct xmm_dev *xmm, u8 ring_id,
				  const void *buf, int len)
{
	struct td_ring *ring = &xmm->td_ring[ring_id];
	u8 wptr = xmm->cp->s_wptr[ring_id];

	BUG_ON(!ring->depth);
	BUG_ON(len > ring->page_size);
	BUG_ON(ring_id & 1);

	memcpy(ring->pages[wptr], buf, len);
	ring->tds[wptr].length = len;
	ring->tds[wptr].flags = 0;
	ring->tds[wptr].unk = 0;

	wptr = (wptr + 1) & (ring->depth - 1);
	BUG_ON(wptr == xmm->cp->s_rptr[ring_id]);

	xmm->cp->s_wptr[ring_id] = wptr;
}

static int xmm7360_td_ring_full(struct xmm_dev *xmm, u8 ring_id)
{
	struct td_ring *ring = &xmm->td_ring[ring_id];
	u8 wptr = xmm->cp->s_wptr[ring_id];
	wptr = (wptr + 1) & (ring->depth - 1);
	return wptr == xmm->cp->s_rptr[ring_id];
}

static void xmm7360_td_ring_read(struct xmm_dev *xmm, u8 ring_id)
{
	struct td_ring *ring = &xmm->td_ring[ring_id];
	u8 wptr = xmm->cp->s_wptr[ring_id];

	if (!ring->depth) {
		dev_err(xmm->dev, "read on disabled ring\n");
		WARN_ON(1);
		return;
	}
	if (!(ring_id & 1)) {
		dev_err(xmm->dev, "read on write ring\n");
		WARN_ON(1);
		return;
	}

	ring->tds[wptr].length = ring->page_size;
	ring->tds[wptr].flags = 0;
	ring->tds[wptr].unk = 0;

	wptr = (wptr + 1) & (ring->depth - 1);
	BUG_ON(wptr == xmm->cp->s_rptr[ring_id]);

	xmm->cp->s_wptr[ring_id] = wptr;
}

static struct queue_pair *xmm7360_init_qp(struct xmm_dev *xmm, int num,
					  u8 depth, u16 page_size)
{
	struct queue_pair *qp = &xmm->qp[num];

	qp->xmm = xmm;
	qp->num = num;
	qp->open = 0;
	qp->depth = depth;
	qp->page_size = page_size;

	mutex_init(&qp->lock);
	init_waitqueue_head(&qp->wq);
	return qp;
}

static int xmm7360_qp_start(struct queue_pair *qp)
{
	struct xmm_dev *xmm = qp->xmm;
	int ret;

	mutex_lock(&qp->lock);

	if (qp->open) {
		ret = -EBUSY;
	} else {
		ret = 0;
		qp->open = 1;

		ret = xmm7360_td_ring_create(xmm, qp->num * 2, qp->depth,
					     qp->page_size);
		if (ret)
			goto out;
		ret = xmm7360_td_ring_create(xmm, qp->num * 2 + 1, qp->depth,
					     qp->page_size);
		if (ret) {
			xmm7360_td_ring_destroy(xmm, qp->num * 2);
			goto out;
		}
		while (!xmm7360_td_ring_full(xmm, qp->num * 2 + 1))
			xmm7360_td_ring_read(xmm, qp->num * 2 + 1);
		xmm7360_ding(xmm, DOORBELL_TD);
	}

out:
	mutex_unlock(&qp->lock);

	return ret;
}

static int xmm7360_qp_stop(struct queue_pair *qp)
{
	struct xmm_dev *xmm = qp->xmm;
	int ret = 0;

	mutex_lock(&qp->lock);
	if (!qp->open) {
		ret = -ENODEV;
	} else {
		ret = 0;
		qp->open = 0;

		xmm7360_td_ring_destroy(xmm, qp->num * 2);
		xmm7360_td_ring_destroy(xmm, qp->num * 2 + 1);
	}
	mutex_unlock(&qp->lock);
	return ret;
}

static int xmm7360_qp_can_write(struct queue_pair *qp)
{
	struct xmm_dev *xmm = qp->xmm;
	return !xmm7360_td_ring_full(xmm, qp->num * 2);
}

static size_t xmm7360_qp_write(struct queue_pair *qp, const char *buf,
			       size_t size)
{
	struct xmm_dev *xmm = qp->xmm;
	int page_size = qp->xmm->td_ring[qp->num * 2].page_size;
	if (xmm->error)
		return xmm->error;
	if (!xmm7360_qp_can_write(qp))
		return 0;
	if (size > page_size)
		size = page_size;
	xmm7360_td_ring_write(xmm, qp->num * 2, buf, size);
	xmm7360_ding(xmm, DOORBELL_TD);
	return size;
}

static size_t xmm7360_qp_write_user(struct queue_pair *qp,
				    const char __user *buf, size_t size)
{
	int page_size = qp->xmm->td_ring[qp->num * 2].page_size;
	int ret;

	if (size > page_size)
		size = page_size;

	ret = copy_from_user(qp->user_buf, buf, size);
	size = size - ret;
	if (!size)
		return 0;
	return xmm7360_qp_write(qp, qp->user_buf, size);
}

static int xmm7360_qp_has_data(struct queue_pair *qp)
{
	struct xmm_dev *xmm = qp->xmm;
	struct td_ring *ring = &xmm->td_ring[qp->num * 2 + 1];
	return xmm->cp->s_rptr[qp->num * 2 + 1] != ring->last_handled;
}

static void xmm7360_tty_poll_qp(struct queue_pair *qp)
{
	struct xmm_dev *xmm = qp->xmm;
	struct td_ring *ring = &xmm->td_ring[qp->num * 2 + 1];
	int idx, nread;
	while (xmm7360_qp_has_data(qp)) {
		idx = ring->last_handled;
		nread = ring->tds[idx].length;
		tty_insert_flip_string(&qp->port, ring->pages[idx], nread);
		tty_flip_buffer_push(&qp->port);

		xmm7360_td_ring_read(xmm, qp->num * 2 + 1);
		xmm7360_ding(xmm, DOORBELL_TD);
		ring->last_handled = (idx + 1) & (ring->depth - 1);
	}
}

int xmm7360_cdev_open(struct inode *inode, struct file *file)
{
	struct queue_pair *qp =
		container_of(inode->i_cdev, struct queue_pair, cdev);
	file->private_data = qp;
	return xmm7360_qp_start(qp);
}

int xmm7360_cdev_release(struct inode *inode, struct file *file)
{
	struct queue_pair *qp = file->private_data;
	return xmm7360_qp_stop(qp);
}

ssize_t xmm7360_cdev_write(struct file *file, const char __user *buf,
			   size_t size, loff_t *offset)
{
	struct queue_pair *qp = file->private_data;
	int ret;

	ret = xmm7360_qp_write_user(qp, buf, size);
	if (ret < 0)
		return ret;

	*offset += ret;
	return size;
}

ssize_t xmm7360_cdev_read(struct file *file, char __user *buf, size_t size,
			  loff_t *offset)
{
	struct queue_pair *qp = file->private_data;
	struct xmm_dev *xmm = qp->xmm;
	struct td_ring *ring = &xmm->td_ring[qp->num * 2 + 1];
	int idx, nread, ret;
	ret = wait_event_interruptible(qp->wq,
				       xmm7360_qp_has_data(qp) || xmm->error);
	if (ret < 0)
		return ret;
	if (xmm->error)
		return xmm->error;

	idx = ring->last_handled;
	nread = ring->tds[idx].length;
	if (nread > size)
		nread = size;
	ret = copy_to_user(buf, ring->pages[idx], nread);
	nread -= ret;

	xmm7360_td_ring_read(xmm, qp->num * 2 + 1);
	xmm7360_ding(xmm, DOORBELL_TD);
	ring->last_handled = (idx + 1) & (ring->depth - 1);

	*offset += nread;
	return nread;
}

static unsigned int xmm7360_cdev_poll(struct file *file, poll_table *wait)
{
	struct queue_pair *qp = file->private_data;
	unsigned int mask = 0;

	poll_wait(file, &qp->wq, wait);

	if (qp->xmm->error)
		return POLLHUP;

	if (xmm7360_qp_has_data(qp))
		mask |= POLLIN | POLLRDNORM;

	if (xmm7360_qp_can_write(qp))
		mask |= POLLOUT | POLLWRNORM;

	return mask;
}

static long xmm7360_cdev_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct queue_pair *qp = file->private_data;

	u32 val;

	switch (cmd) {
	case XMM7360_IOCTL_GET_PAGE_SIZE:
		val = qp->xmm->td_ring[qp->num * 2].page_size;
		if (copy_to_user((u32 *)arg, &val, sizeof(u32)))
			return -EFAULT;
		return 0;
	}

	return -ENOTTY;
}

static struct file_operations xmm7360_fops = {
	.read = xmm7360_cdev_read,
	.write = xmm7360_cdev_write,
	.poll = xmm7360_cdev_poll,
	.unlocked_ioctl = xmm7360_cdev_ioctl,
	.open = xmm7360_cdev_open,
	.release = xmm7360_cdev_release
};

static void xmm7360_mux_frame_init(struct xmm_net *xn, struct mux_frame *frame,
				   int sequence)
{
	frame->sequence = xn->sequence;
	frame->max_size = xn->xmm->td_ring[0].page_size;
	frame->n_packets = 0;
	frame->n_bytes = 0;
	frame->last_tag_next = NULL;
	frame->last_tag_length = NULL;
}

static int xmm7360_mux_frame_add_tag(struct mux_frame *frame, uint32_t tag,
				     uint16_t extra, void *data, int data_len)
{
	int total_length;
	if (frame->n_bytes == 0)
		total_length = sizeof(struct mux_first_header) + data_len;
	else
		total_length = sizeof(struct mux_next_header) + data_len;

	while (frame->n_bytes & 3)
		frame->n_bytes++;

	if (frame->n_bytes + total_length > frame->max_size)
		return -1;

	if (frame->last_tag_next)
		*frame->last_tag_next = frame->n_bytes;

	if (frame->n_bytes == 0) {
		struct mux_first_header *hdr =
			(struct mux_first_header *)frame->data;
		memset(hdr, 0, sizeof(struct mux_first_header));
		hdr->tag = htonl(tag);
		hdr->sequence = frame->sequence;
		hdr->length = total_length;
		hdr->extra = extra;
		frame->last_tag_length = &hdr->length;
		frame->last_tag_next = &hdr->next;
		frame->n_bytes += sizeof(struct mux_first_header);
	} else {
		struct mux_next_header *hdr =
			(struct mux_next_header *)(&frame->data[frame->n_bytes]);
		memset(hdr, 0, sizeof(struct mux_next_header));
		hdr->tag = htonl(tag);
		hdr->length = total_length;
		hdr->extra = extra;
		frame->last_tag_length = &hdr->length;
		frame->last_tag_next = &hdr->next;
		frame->n_bytes += sizeof(struct mux_next_header);
	}

	if (data_len) {
		memcpy(&frame->data[frame->n_bytes], data, data_len);
		frame->n_bytes += data_len;
	}

	return 0;
}

static int xmm7360_mux_frame_append_data(struct mux_frame *frame, void *data,
					 int data_len)
{
	if (frame->n_bytes + data_len > frame->max_size)
		return -1;

	BUG_ON(!frame->last_tag_length);

	memcpy(&frame->data[frame->n_bytes], data, data_len);
	*frame->last_tag_length += data_len;
	frame->n_bytes += data_len;

	return 0;
}

static int xmm7360_mux_frame_append_packet(struct mux_frame *frame,
					   struct sk_buff *skb)
{
	int expected_adth_size =
		sizeof(struct mux_next_header) + 4 +
		(frame->n_packets + 1) * sizeof(struct mux_bounds);
	int ret;
	uint8_t pad[16];

	if (frame->n_packets >= MUX_MAX_PACKETS)
		return -1;

	if (frame->n_bytes + skb->len + 16 + expected_adth_size >
	    frame->max_size)
		return -1;

	BUG_ON(!frame->last_tag_length);

	frame->bounds[frame->n_packets].offset = frame->n_bytes;
	frame->bounds[frame->n_packets].length = skb->len + 16;
	frame->n_packets++;

	memset(pad, 0, sizeof(pad));
	ret = xmm7360_mux_frame_append_data(frame, pad, 16);
	if (ret)
		return ret;

	ret = xmm7360_mux_frame_append_data(frame, skb->data, skb->len);
	return ret;
}

static int xmm7360_mux_frame_push(struct xmm_dev *xmm, struct mux_frame *frame)
{
	struct mux_first_header *hdr = (void *)&frame->data[0];
	int ret;
	hdr->length = frame->n_bytes;

	ret = xmm7360_qp_write(xmm->net->qp, frame->data, frame->n_bytes);
	if (ret < 0)
		return ret;
	return 0;
}

static int xmm7360_mux_control(struct xmm_net *xn, u32 arg1, u32 arg2, u32 arg3,
			       u32 arg4)
{
	struct mux_frame *frame = &xn->frame;
	int ret;
	uint32_t cmdh_args[] = { arg1, arg2, arg3, arg4 };
	unsigned long flags;

	spin_lock_irqsave(&xn->lock, flags);

	xmm7360_mux_frame_init(xn, frame, 0);
	xmm7360_mux_frame_add_tag(frame, 'ACBH', 0, NULL, 0);
	xmm7360_mux_frame_add_tag(frame, 'CMDH', xn->channel, cmdh_args,
				  sizeof(cmdh_args));
	ret = xmm7360_mux_frame_push(xn->xmm, frame);

	spin_unlock_irqrestore(&xn->lock, flags);

	return ret;
}

static void xmm7360_net_uninit(struct net_device *dev)
{
}

static int xmm7360_net_open(struct net_device *dev)
{
	struct xmm_net *xn = netdev_priv(dev);
	struct sk_buff *skb;
	xn->queued_packets = xn->queued_bytes = 0;
	while ((skb = skb_dequeue(&xn->queue)))
		kfree_skb(skb);
	netif_start_queue(dev);
	return xmm7360_mux_control(xn, 1, 0, 0, 0);
}

static int xmm7360_net_close(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

static int xmm7360_net_must_flush(struct xmm_net *xn, int new_packet_bytes)
{
	int frame_size;
	if (xn->queued_packets >= MUX_MAX_PACKETS)
		return 1;

	frame_size = sizeof(struct mux_first_header) + xn->queued_bytes +
		     sizeof(struct mux_next_header) + 4 +
		     sizeof(struct mux_bounds) * xn->queued_packets;

	frame_size += 16 + new_packet_bytes + sizeof(struct mux_bounds);

	return frame_size > xn->frame.max_size;
}

static void xmm7360_net_flush(struct xmm_net *xn)
{
	struct sk_buff *skb;
	struct mux_frame *frame = &xn->frame;
	int ret;
	u32 unknown = 0;

	if (skb_queue_empty(&xn->queue))
		return;

	xmm7360_mux_frame_init(xn, frame, xn->sequence++);
	xmm7360_mux_frame_add_tag(frame, 'ADBH', 0, NULL, 0);

	while ((skb = skb_dequeue(&xn->queue))) {
		ret = xmm7360_mux_frame_append_packet(frame, skb);
		if (ret)
			goto drop;
	}

	ret = xmm7360_mux_frame_add_tag(frame, 'ADTH', xn->channel, &unknown,
					sizeof(uint32_t));
	if (ret)
		goto drop;
	ret = xmm7360_mux_frame_append_data(frame, &frame->bounds[0],
					    sizeof(struct mux_bounds) *
						    frame->n_packets);
	if (ret)
		goto drop;
	ret = xmm7360_mux_frame_push(xn->xmm, frame);
	if (ret)
		goto drop;

	xn->queued_packets = xn->queued_bytes = 0;

	return;

drop:
	dev_err(xn->xmm->dev, "Failed to ship coalesced frame");
}

static enum hrtimer_restart xmm7360_net_deadline_cb(struct hrtimer *t)
{
	struct xmm_net *xn = container_of(t, struct xmm_net, deadline);
	unsigned long flags;
	spin_lock_irqsave(&xn->lock, flags);
	xmm7360_net_flush(xn);
	spin_unlock_irqrestore(&xn->lock, flags);
	return HRTIMER_NORESTART;
}

static netdev_tx_t xmm7360_net_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct xmm_net *xn = netdev_priv(dev);
	ktime_t kt;
	unsigned long flags;

	if (netif_queue_stopped(dev))
		return NETDEV_TX_BUSY;

	skb_orphan(skb);

	spin_lock_irqsave(&xn->lock, flags);
	if (xmm7360_net_must_flush(xn, skb->len)) {
		if (xmm7360_qp_can_write(xn->qp)) {
			xmm7360_net_flush(xn);
		} else {
			netif_stop_queue(dev);
			spin_unlock_irqrestore(&xn->lock, flags);
			return NETDEV_TX_BUSY;
		}
	}

	xn->queued_packets++;
	xn->queued_bytes += 16 + skb->len;
	skb_queue_tail(&xn->queue, skb);

	spin_unlock_irqrestore(&xn->lock, flags);

	if (!hrtimer_active(&xn->deadline)) {
		kt = ktime_set(0, 100000);
		hrtimer_start(&xn->deadline, kt, HRTIMER_MODE_REL);
	}

	return NETDEV_TX_OK;
}

static void xmm7360_net_mux_handle_frame(struct xmm_net *xn, u8 *data, int len)
{
	struct mux_first_header *first;
	struct mux_next_header *adth;
	int n_packets, i;
	struct mux_bounds *bounds;
	struct sk_buff *skb;
	void *p;
	u8 ip_version;

	first = (void *)data;
	if (ntohl(first->tag) == 'ACBH')
		return;

	if (ntohl(first->tag) != 'ADBH') {
		dev_info(xn->xmm->dev, "Unexpected tag %x\n", first->tag);
		return;
	}

	adth = (void *)(&data[first->next]);
	if (ntohl(adth->tag) != 'ADTH') {
		dev_err(xn->xmm->dev, "Unexpected tag %x, expected ADTH\n",
			adth->tag);
		return;
	}

	n_packets = (adth->length - sizeof(struct mux_next_header) - 4) /
		    sizeof(struct mux_bounds);

	bounds =
		(void *)&data[first->next + sizeof(struct mux_next_header) + 4];

	for (i = 0; i < n_packets; i++) {
		if (!bounds[i].length)
			continue;

		skb = dev_alloc_skb(bounds[i].length + NET_IP_ALIGN);
		if (!skb)
			return;
		skb_reserve(skb, NET_IP_ALIGN);
		p = skb_put(skb, bounds[i].length);
		memcpy(p, &data[bounds[i].offset], bounds[i].length);

		skb->dev = xn->xmm->netdev;

		ip_version = skb->data[0] >> 4;
		if (ip_version == 4) {
			skb->protocol = htons(ETH_P_IP);
		} else if (ip_version == 6) {
			skb->protocol = htons(ETH_P_IPV6);
		} else {
			kfree_skb(skb);
			return;
		}

		netif_rx(skb);
	}
}

static void xmm7360_net_poll(struct xmm_dev *xmm)
{
	struct queue_pair *qp;
	struct td_ring *ring;
	int idx, nread;
	if (!xmm->net)
		return;
	qp = xmm->net->qp;
	ring = &xmm->td_ring[qp->num * 2 + 1];

	if (netif_queue_stopped(xmm->netdev) && xmm7360_qp_can_write(qp))
		netif_wake_queue(xmm->netdev);

	while (xmm7360_qp_has_data(qp)) {
		idx = ring->last_handled;
		nread = ring->tds[idx].length;
		xmm7360_net_mux_handle_frame(xmm->net, ring->pages[idx], nread);

		xmm7360_td_ring_read(xmm, qp->num * 2 + 1);
		xmm7360_ding(xmm, DOORBELL_TD);
		ring->last_handled = (idx + 1) & (ring->depth - 1);
	}
}

static const struct net_device_ops xmm7360_netdev_ops = {
	.ndo_uninit = xmm7360_net_uninit,
	.ndo_open = xmm7360_net_open,
	.ndo_stop = xmm7360_net_close,
	.ndo_start_xmit = xmm7360_net_xmit,
};

static void xmm7360_net_setup(struct net_device *dev)
{
	struct xmm_net *xn = netdev_priv(dev);
	spin_lock_init(&xn->lock);
	hrtimer_init(&xn->deadline, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	xn->deadline.function = xmm7360_net_deadline_cb;
	skb_queue_head_init(&xn->queue);

	dev->netdev_ops = &xmm7360_netdev_ops;

	dev->hard_header_len = 0;
	dev->addr_len = 0;
	dev->mtu = 1500;
	dev->min_mtu = 1500;
	dev->max_mtu = 1500;

	dev->tx_queue_len = 1000;

	dev->type = ARPHRD_NONE;
	dev->flags = IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;
}

static int xmm7360_create_net(struct xmm_dev *xmm)
{
	struct net_device *netdev;
	struct xmm_net *xn;
	int ret;

	netdev = alloc_netdev(sizeof(struct xmm_net), "wwan%d",
			      NET_NAME_UNKNOWN, xmm7360_net_setup);

	if (!netdev)
		return -ENOMEM;

	SET_NETDEV_DEV(netdev, xmm->dev);

	xmm->netdev = netdev;

	xn = netdev_priv(netdev);
	xn->xmm = xmm;
	xmm->net = xn;

	rtnl_lock();
	ret = register_netdevice(netdev);
	rtnl_unlock();

	xn->qp = xmm7360_init_qp(xmm, 0, 128, TD_MAX_PAGE_SIZE);

	if (!ret)
		ret = xmm7360_qp_start(xn->qp);

	if (ret < 0) {
		free_netdev(netdev);
		xmm->netdev = NULL;
		xmm7360_qp_stop(xn->qp);
	}

	return ret;
}

static void xmm7360_destroy_net(struct xmm_dev *xmm)
{
	if (xmm->netdev) {
		xmm7360_qp_stop(xmm->net->qp);
		rtnl_lock();
		unregister_netdevice(xmm->netdev);
		rtnl_unlock();
		free_netdev(xmm->netdev);
		xmm->net = NULL;
		xmm->netdev = NULL;
	}
}

static irqreturn_t xmm7360_irq0(int irq, void *dev_id)
{
	struct xmm_dev *xmm = dev_id;
	struct queue_pair *qp;
	int id;

	xmm7360_poll(xmm);
	wake_up(&xmm->wq);
	if (xmm->td_ring) {
		xmm7360_net_poll(xmm);

		for (id = 1; id < 8; id++) {
			qp = &xmm->qp[id];

			/* wake _cdev_read() */
			if (qp->open)
				wake_up(&qp->wq);

			/* tty tasks */
			if (qp->open && qp->port.ops) {
				xmm7360_tty_poll_qp(qp);
				if (qp->tty_needs_wake &&
				    xmm7360_qp_can_write(qp) && qp->port.tty) {
					struct tty_ldisc *ldisc =
						tty_ldisc_ref(qp->port.tty);
					if (ldisc) {
						if (ldisc->ops->write_wakeup)
							ldisc->ops->write_wakeup(
								qp->port.tty);
						tty_ldisc_deref(ldisc);
					}
					qp->tty_needs_wake = 0;
				}
			}
		}
	}

	return IRQ_HANDLED;
}

static void xmm7360_dev_deinit(struct xmm_dev *xmm)
{
	int i;
	xmm->error = -ENODEV;

	cancel_work_sync(&xmm->init_work);

	xmm7360_destroy_net(xmm);

	for (i = 0; i < 8; i++) {
		if (xmm->qp[i].xmm) {
			if (xmm->qp[i].cdev.owner) {
				cdev_del(&xmm->qp[i].cdev);
				device_unregister(&xmm->qp[i].dev);
			}
			if (xmm->qp[i].port.ops) {
				tty_unregister_device(xmm7360_tty_driver,
						      xmm->qp[i].tty_index);
				tty_port_destroy(&xmm->qp[i].port);
			}
		}
		memset(&xmm->qp[i], 0, sizeof(struct queue_pair));
	}
	xmm7360_cmd_ring_free(xmm);
}

static void xmm7360_remove(struct pci_dev *dev)
{
	struct xmm_dev *xmm = pci_get_drvdata(dev);

	xmm7360_dev_deinit(xmm);

	if (xmm->irq)
		free_irq(xmm->irq, xmm);
	pci_free_irq_vectors(dev);
	pci_release_region(dev, 0);
	pci_release_region(dev, 2);
	pci_disable_device(dev);
	kfree(xmm);
}

static void xmm7360_cdev_dev_release(struct device *dev)
{
}

static int xmm7360_tty_open(struct tty_struct *tty, struct file *filp)
{
	struct queue_pair *qp = tty->driver_data;
	return tty_port_open(&qp->port, tty, filp);
}

static void xmm7360_tty_close(struct tty_struct *tty, struct file *filp)
{
	struct queue_pair *qp = tty->driver_data;
	if (qp)
		tty_port_close(&qp->port, tty, filp);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
static ssize_t xmm7360_tty_write(struct tty_struct *tty,
			     const unsigned char *buffer, size_t count)
#else
static int xmm7360_tty_write(struct tty_struct *tty,
			     const unsigned char *buffer, int count)
#endif
{
	struct queue_pair *qp = tty->driver_data;
	int written;
	written = xmm7360_qp_write(qp, buffer, count);
	if (written < count)
		qp->tty_needs_wake = 1;
	return written;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
static unsigned xmm7360_tty_write_room(struct tty_struct *tty)
#else
static int xmm7360_tty_write_room(struct tty_struct *tty)
#endif
{
	struct queue_pair *qp = tty->driver_data;
	if (!xmm7360_qp_can_write(qp))
		return 0;
	else
		return qp->xmm->td_ring[qp->num * 2].page_size;
}

static int xmm7360_tty_install(struct tty_driver *driver,
			       struct tty_struct *tty)
{
	struct queue_pair *qp;
	int ret;

	ret = tty_standard_install(driver, tty);
	if (ret)
		return ret;

	tty->port = driver->ports[tty->index];
	qp = container_of(tty->port, struct queue_pair, port);
	tty->driver_data = qp;
	return 0;
}

static int xmm7360_tty_port_activate(struct tty_port *tport,
				     struct tty_struct *tty)
{
	struct queue_pair *qp = tty->driver_data;
	return xmm7360_qp_start(qp);
}

static void xmm7360_tty_port_shutdown(struct tty_port *tport)
{
	struct queue_pair *qp = tport->tty->driver_data;
	xmm7360_qp_stop(qp);
}

static const struct tty_port_operations xmm7360_tty_port_ops = {
	.activate = xmm7360_tty_port_activate,
	.shutdown = xmm7360_tty_port_shutdown,
};

static const struct tty_operations xmm7360_tty_ops = {
	.open = xmm7360_tty_open,
	.close = xmm7360_tty_close,
	.write = xmm7360_tty_write,
	.write_room = xmm7360_tty_write_room,
	.install = xmm7360_tty_install,
};

static int xmm7360_create_tty(struct xmm_dev *xmm, int num)
{
	struct device *tty_dev;
	struct queue_pair *qp = xmm7360_init_qp(xmm, num, 8, 4096);
	int ret;
	tty_port_init(&qp->port);
	qp->port.ops = &xmm7360_tty_port_ops;
	qp->tty_index = xmm->num_ttys++;
	tty_dev = tty_port_register_device(&qp->port, xmm7360_tty_driver,
					   qp->tty_index, xmm->dev);

	if (IS_ERR(tty_dev)) {
		qp->port.ops = NULL; // prevent calling unregister
		ret = PTR_ERR(tty_dev);
		dev_err(xmm->dev, "Could not allocate tty?\n");
		tty_port_destroy(&qp->port);
		return ret;
	}

	return 0;
}

static int xmm7360_create_cdev(struct xmm_dev *xmm, int num, const char *name,
			       int cardnum)
{
	struct queue_pair *qp = xmm7360_init_qp(xmm, num, 16, TD_MAX_PAGE_SIZE);
	int ret;

	cdev_init(&qp->cdev, &xmm7360_fops);
	qp->cdev.owner = THIS_MODULE;
	device_initialize(&qp->dev);
	qp->dev.devt = MKDEV(MAJOR(xmm_base), num); // XXX multiple cards
	qp->dev.parent = &xmm->pci_dev->dev;
	qp->dev.release = xmm7360_cdev_dev_release;
	dev_set_name(&qp->dev, name, cardnum);
	dev_set_drvdata(&qp->dev, qp);
	ret = cdev_device_add(&qp->cdev, &qp->dev);
	if (ret) {
		dev_err(xmm->dev, "cdev_device_add: %d\n", ret);
		return ret;
	}
	return 0;
}

static int xmm7360_dev_init(struct xmm_dev *xmm)
{
	int ret, i;
	u32 status;

	xmm->error = 0;
	xmm->num_ttys = 0;

	status = xmm->bar2[0];
	if (status == 0xfeedb007) {
		dev_info(xmm->dev, "modem still booting, waiting...");
		for (i = 0; i < 100; i++) {
			status = xmm->bar2[0];
			if (status != 0xfeedb007)
				break;
			msleep(200);
		}
	}

	if (status != 0x600df00d) {
		dev_err(xmm->dev, "unknown modem status: 0x%08x\n", status);
		return -EINVAL;
	}

	dev_info(xmm->dev, "modem is ready");

	ret = xmm7360_cmd_ring_init(xmm);
	if (ret) {
		dev_err(xmm->dev, "Could not bring up command ring\n");
		return ret;
	}

	ret = xmm7360_create_cdev(xmm, 1, "xmm%d/rpc", xmm->card_num);
	if (ret)
		return ret;
	ret = xmm7360_create_cdev(xmm, 3, "xmm%d/trace", xmm->card_num);
	if (ret)
		return ret;
	ret = xmm7360_create_tty(xmm, 2);
	if (ret)
		return ret;
	ret = xmm7360_create_tty(xmm, 4);
	if (ret)
		return ret;
	ret = xmm7360_create_tty(xmm, 7);
	if (ret)
		return ret;
	ret = xmm7360_create_net(xmm);
	if (ret)
		return ret;

	return 0;
}

void xmm7360_dev_init_work(struct work_struct *work)
{
	struct xmm_dev *xmm = container_of(work, struct xmm_dev, init_work);
	xmm7360_dev_init(xmm);
}

static int xmm7360_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct xmm_dev *xmm = kzalloc(sizeof(struct xmm_dev), GFP_KERNEL);
	int ret;

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

	ret = dma_set_mask_and_coherent(xmm->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_err(xmm->dev, "Cannot set DMA mask\n");
		goto fail;
	}

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

	ret = pci_alloc_irq_vectors(dev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_MSIX);
	if (ret < 0) {
		dev_err(&(dev->dev), "pci_alloc_irq_vectors\n");
		goto fail;
	}

	init_waitqueue_head(&xmm->wq);
	INIT_WORK(&xmm->init_work, xmm7360_dev_init_work);

	xmm->irq = pci_irq_vector(dev, 0);
	ret = request_irq(xmm->irq, xmm7360_irq0, 0, "xmm7360", xmm);
	if (ret) {
		dev_err(&(dev->dev), "request_irq\n");
		goto fail;
	}

	pci_set_drvdata(dev, xmm);

	ret = xmm7360_dev_init(xmm);
	if (!ret)
		return 0;

fail:
	xmm7360_dev_deinit(xmm);
	xmm7360_remove(dev);
	return ret;
}

static struct pci_driver xmm7360_driver = {
	.name = "xmm7360",
	.id_table = xmm7360_ids,
	.probe = xmm7360_probe,
	.remove = xmm7360_remove,
};

static int xmm7360_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&xmm_base, 0, 8, "xmm");
	if (ret) {
		return ret;
	}

	xmm7360_tty_driver = tty_alloc_driver(8, 0);
	if (IS_ERR(xmm7360_tty_driver)) {
		pr_err("xmm7360: Failed to allocate tty\n");
		return -ENOMEM;
	}

	xmm7360_tty_driver->driver_name = "xmm7360";
	xmm7360_tty_driver->name = "ttyXMM";
	xmm7360_tty_driver->major = 0;
	xmm7360_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	xmm7360_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	xmm7360_tty_driver->flags =
		TTY_DRIVER_REAL_RAW |
		TTY_DRIVER_DYNAMIC_DEV; // Could this flags be defined in the flags??
	xmm7360_tty_driver->init_termios = tty_std_termios;
	xmm7360_tty_driver->init_termios.c_cflag = B115200 | CS8 | CREAD |
						   HUPCL | CLOCAL;
	xmm7360_tty_driver->init_termios.c_lflag &= ~ECHO;
	xmm7360_tty_driver->init_termios.c_ispeed = 115200;
	xmm7360_tty_driver->init_termios.c_ospeed = 115200;
	tty_set_operations(xmm7360_tty_driver, &xmm7360_tty_ops);

	ret = tty_register_driver(xmm7360_tty_driver);
	if (ret) {
		pr_err("xmm7360: failed to register xmm7360_tty driver\n");
		return ret;
	}

	ret = pci_register_driver(&xmm7360_driver);
	if (ret) {
		return ret;
	}

	return 0;
}

static void xmm7360_exit(void)
{
	pci_unregister_driver(&xmm7360_driver);
	unregister_chrdev_region(xmm_base, 8);
	tty_unregister_driver(xmm7360_tty_driver);
	tty_driver_kref_put(xmm7360_tty_driver);
}

module_init(xmm7360_init);
module_exit(xmm7360_exit);
