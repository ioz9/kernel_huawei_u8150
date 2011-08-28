/*
 *  linux/drivers/mmc/host/msm_sdcc.c - Qualcomm MSM 7X00A SDCC Driver
 *
 *  Copyright (C) 2007 Google Inc,
 *  Copyright (C) 2003 Deep Blue Solutions, Ltd, All Rights Reserved.
 *  Copyright (c) 2009-2010, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Based on mmci.c
 *
 * Author: San Mehat (san@android.com)
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/log2.h>
#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sdio.h>
#include <linux/clk.h>
#include <linux/scatterlist.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/memory.h>

#include <asm/cacheflush.h>
#include <asm/div64.h>
#include <asm/sizes.h>

#include <asm/mach/mmc.h>
#include <mach/msm_iomap.h>
#include <mach/clk.h>
#include <mach/dma.h>
#include <mach/htc_pwrsink.h>

#include <linux/hardware_self_adapt.h>

#include <mach/vreg.h>
#include <mach/mpp.h>

#include "msm_sdcc.h"

#define SDCC_WIFI_SLOT		2

#define SDCC_SD_SLOT		1

#define DRIVER_NAME "msm-sdcc"

#define DBG(host, fmt, args...)	\
	pr_debug("%s: %s: " fmt "\n", mmc_hostname(host->mmc), __func__ , args)

#define IRQ_DEBUG 0

#if defined(CONFIG_DEBUG_FS)
static void msmsdcc_dbg_createhost(struct msmsdcc_host *);
static struct dentry *debugfs_dir;
static struct dentry *debugfs_file;
static int  msmsdcc_dbg_init(void);
#endif

#ifdef CONFIG_MMC_AUTO_SUSPEND
static int msmsdcc_auto_suspend(struct mmc_host *, int);
#endif

static unsigned int msmsdcc_pwrsave = 1;
static unsigned int wifi_chip_is_bcm = 0;

#define DUMMY_52_STATE_NONE		0
#define DUMMY_52_STATE_SENT		1

static struct mmc_command dummy52cmd;
static struct mmc_request dummy52mrq = {
	.cmd = &dummy52cmd,
	.data = NULL,
	.stop = NULL,
};
static struct mmc_command dummy52cmd = {
	.opcode = SD_IO_RW_DIRECT,
	.flags = MMC_RSP_PRESENT,
	.data = NULL,
	.mrq = &dummy52mrq,
};

#define VERBOSE_COMMAND_TIMEOUTS	0

#if IRQ_DEBUG == 1
static char *irq_status_bits[] = { "cmdcrcfail", "datcrcfail", "cmdtimeout",
				   "dattimeout", "txunderrun", "rxoverrun",
				   "cmdrespend", "cmdsent", "dataend", NULL,
				   "datablkend", "cmdactive", "txactive",
				   "rxactive", "txhalfempty", "rxhalffull",
				   "txfifofull", "rxfifofull", "txfifoempty",
				   "rxfifoempty", "txdataavlbl", "rxdataavlbl",
				   "sdiointr", "progdone", "atacmdcompl",
				   "sdiointrope", "ccstimeout", NULL, NULL,
				   NULL, NULL, NULL };

static void
msmsdcc_print_status(struct msmsdcc_host *host, char *hdr, uint32_t status)
{
	int i;

	pr_debug("%s-%s ", mmc_hostname(host->mmc), hdr);
	for (i = 0; i < 32; i++) {
		if (status & (1 << i))
			pr_debug("%s ", irq_status_bits[i]);
	}
	pr_debug("\n");
}
#endif

static void
msmsdcc_start_command(struct msmsdcc_host *host, struct mmc_command *cmd,
		      u32 c);

static void msmsdcc_reset_and_restore(struct msmsdcc_host *host)
{
	u32	mci_clk = 0;
	u32	mci_mask0 = 0;
	int ret;

	/* Save the controller state */
	mci_clk = readl(host->base + MMCICLOCK);
	mci_mask0 = readl(host->base + MMCIMASK0);

	/* Reset the controller */
	ret = clk_reset(host->clk, CLK_RESET_ASSERT);
	if (ret)
		pr_err("%s: Clock assert failed at %u Hz with err %d\n",
				mmc_hostname(host->mmc), host->clk_rate, ret);

	ret = clk_reset(host->clk, CLK_RESET_DEASSERT);
	if (ret)
		pr_err("%s: Clock deassert failed at %u Hz with err %d\n",
				mmc_hostname(host->mmc), host->clk_rate, ret);

	pr_info("%s: Controller has been reset\n", mmc_hostname(host->mmc));

	/* Restore the contoller state */
	writel(host->pwr, host->base + MMCIPOWER);
	writel(mci_clk, host->base + MMCICLOCK);
	writel(mci_mask0, host->base + MMCIMASK0);
	ret = clk_set_rate(host->clk, host->clk_rate);
	if (ret)
		pr_err("%s: Failed to set clk rate %u Hz. err %d\n",
				mmc_hostname(host->mmc), host->clk_rate, ret);
}

static int
msmsdcc_request_end(struct msmsdcc_host *host, struct mmc_request *mrq)
{
	int retval = 0;

	BUG_ON(host->curr.data);

	host->curr.mrq = NULL;
	host->curr.cmd = NULL;

	if (mrq->data)
		mrq->data->bytes_xfered = host->curr.data_xfered;
	if (mrq->cmd->error == -ETIMEDOUT)
		mdelay(5);

#ifdef CONFIG_MMC_MSM_PROG_DONE_SCAN
	if ((mrq->cmd->opcode == SD_IO_RW_EXTENDED) &&
			(mrq->cmd->arg & 0x80000000)) {
		/* If its a write and a cmd53 set the prog_scan flag. */
		host->prog_scan = 1;
		/* Send STOP to let the SDCC know to stop. */
		writel(MCI_CSPM_MCIABORT, host->base + MMCICOMMAND);
		retval = 1;
	}
	if (mrq->cmd->opcode == SD_IO_RW_DIRECT) {
		/* Ok the cmd52 following a cmd53 is received */
		/* clear all the flags. */
		host->prog_scan = 0;
		host->prog_enable = 0;
	}
#endif
	/*
	 * Need to drop the host lock here; mmc_request_done may call
	 * back into the driver...
	 */
	spin_unlock(&host->lock);
	mmc_request_done(host->mmc, mrq);
	spin_lock(&host->lock);

	return retval;
}

static void
msmsdcc_stop_data(struct msmsdcc_host *host)
{
	host->curr.data = NULL;
	host->curr.got_dataend = 0;
}

static inline uint32_t msmsdcc_fifo_addr(struct msmsdcc_host *host)
{
	return host->memres->start + MMCIFIFO;
}

static inline void msmsdcc_delay(struct msmsdcc_host *host)
{
	udelay(1 + ((3 * USEC_PER_SEC) /
		(host->clk_rate ? host->clk_rate : host->plat->msmsdcc_fmin)));
}

static inline void
msmsdcc_start_command_exec(struct msmsdcc_host *host, u32 arg, u32 c)
{
	writel(arg, host->base + MMCIARGUMENT);
	msmsdcc_delay(host);
	writel(c, host->base + MMCICOMMAND);
}

static void
msmsdcc_dma_exec_func(struct msm_dmov_cmd *cmd)
{
	struct msmsdcc_host *host = (struct msmsdcc_host *)cmd->user;

	writel(host->cmd_timeout, host->base + MMCIDATATIMER);
	writel((unsigned int)host->curr.xfer_size, host->base + MMCIDATALENGTH);
	writel((readl(host->base + MMCIMASK0) & (~(MCI_IRQ_PIO))) |
	       host->cmd_pio_irqmask, host->base + MMCIMASK0);
	msmsdcc_delay(host);	/* Allow data parms to be applied */
	writel(host->cmd_datactrl, host->base + MMCIDATACTRL);
	msmsdcc_delay(host);	/* Force delay prior to ADM or command */

	if (host->cmd_cmd) {
		msmsdcc_start_command_exec(host,
			(u32)host->cmd_cmd->arg, (u32)host->cmd_c);
	}
}

static void
msmsdcc_dma_complete_tlet(unsigned long data)
{
	struct msmsdcc_host *host = (struct msmsdcc_host *)data;
	unsigned long		flags;
	struct mmc_request	*mrq;

	spin_lock_irqsave(&host->lock, flags);
	mrq = host->curr.mrq;
	BUG_ON(!mrq);

	if (!(host->dma.result & DMOV_RSLT_VALID)) {
		pr_err("msmsdcc: Invalid DataMover result\n");
		goto out;
	}

	if (host->dma.result & DMOV_RSLT_DONE) {
		host->curr.data_xfered = host->curr.xfer_size;
	} else {
		/* Error or flush  */
		if (host->dma.result & DMOV_RSLT_ERROR)
			pr_err("%s: DMA error (0x%.8x)\n",
			       mmc_hostname(host->mmc), host->dma.result);
		if (host->dma.result & DMOV_RSLT_FLUSH)
			pr_err("%s: DMA channel flushed (0x%.8x)\n",
			       mmc_hostname(host->mmc), host->dma.result);
		if (host->dma.err) {
			pr_err("Flush data: %.8x %.8x %.8x %.8x %.8x %.8x\n",
			       host->dma.err->flush[0], host->dma.err->flush[1],
			       host->dma.err->flush[2], host->dma.err->flush[3],
			       host->dma.err->flush[4],
			       host->dma.err->flush[5]);
			msmsdcc_reset_and_restore(host);
		}
		if (!mrq->data->error)
			mrq->data->error = -EIO;
	}
	dma_unmap_sg(mmc_dev(host->mmc), host->dma.sg, host->dma.num_ents,
		     host->dma.dir);

	if (host->curr.user_pages) {
		struct scatterlist *sg = host->dma.sg;
		int i;

		for (i = 0; i < host->dma.num_ents; i++, sg++)
			flush_dcache_page(sg_page(sg));
	}

	host->dma.sg = NULL;
	host->dma.busy = 0;

	if (host->curr.got_dataend || mrq->data->error) {

		if (mrq->data->error && !(host->curr.got_dataend)) {
			pr_info("%s: Worked around bug 1535304\n",
			       mmc_hostname(host->mmc));
		}
		/*
		 * If we've already gotten our DATAEND / DATABLKEND
		 * for this request, then complete it through here.
		 */
		msmsdcc_stop_data(host);

		if (!mrq->data->error)
			host->curr.data_xfered = host->curr.xfer_size;
		if (!mrq->data->stop || mrq->cmd->error) {
			host->curr.mrq = NULL;
			host->curr.cmd = NULL;
			mrq->data->bytes_xfered = host->curr.data_xfered;

			spin_unlock_irqrestore(&host->lock, flags);

#ifdef CONFIG_MMC_MSM_PROG_DONE_SCAN
			if ((mrq->cmd->opcode == SD_IO_RW_EXTENDED)
				&& (mrq->cmd->arg & 0x80000000)) {
				/* set the prog_scan in a cmd53.*/
				host->prog_scan = 1;
				/* Send STOP to let the SDCC know to stop. */
				writel(MCI_CSPM_MCIABORT,
						host->base + MMCICOMMAND);
			}
#endif
			mmc_request_done(host->mmc, mrq);
			return;
		} else
			msmsdcc_start_command(host, mrq->data->stop, 0);
	}

out:
	spin_unlock_irqrestore(&host->lock, flags);
	return;
}

static void
msmsdcc_dma_complete_func(struct msm_dmov_cmd *cmd,
			  unsigned int result,
			  struct msm_dmov_errdata *err)
{
	struct msmsdcc_dma_data	*dma_data =
		container_of(cmd, struct msmsdcc_dma_data, hdr);
	struct msmsdcc_host *host = dma_data->host;

	dma_data->result = result;
	dma_data->err = err;

	tasklet_schedule(&host->dma_tlet);
}

static int validate_dma(struct msmsdcc_host *host, struct mmc_data *data)
{
	if (host->dma.channel == -1)
		return -ENOENT;

	if ((data->blksz * data->blocks) < MCI_FIFOSIZE)
		return -EINVAL;
	if ((data->blksz * data->blocks) % MCI_FIFOSIZE)
		return -EINVAL;
	return 0;
}

static int msmsdcc_config_dma(struct msmsdcc_host *host, struct mmc_data *data)
{
	struct msmsdcc_nc_dmadata *nc;
	dmov_box *box;
	uint32_t rows;
	uint32_t crci;
	unsigned int n;
	int i, rc;
	struct scatterlist *sg = data->sg;

	rc = validate_dma(host, data);
	if (rc)
		return rc;

	host->dma.sg = data->sg;
	host->dma.num_ents = data->sg_len;

	BUG_ON(host->dma.num_ents > NR_SG); /* Prevent memory corruption */

	nc = host->dma.nc;

	if (host->pdev_id == 1)
		crci = DMOV_SDC1_CRCI;
	else if (host->pdev_id == 2)
		crci = DMOV_SDC2_CRCI;
	else if (host->pdev_id == 3)
		crci = DMOV_SDC3_CRCI;
	else if (host->pdev_id == 4)
		crci = DMOV_SDC4_CRCI;
#ifdef DMOV_SDC5_CRCI
	else if (host->pdev_id == 5)
		crci = DMOV_SDC5_CRCI;
#endif
	else {
		host->dma.sg = NULL;
		host->dma.num_ents = 0;
		return -ENOENT;
	}

	if (data->flags & MMC_DATA_READ)
		host->dma.dir = DMA_FROM_DEVICE;
	else
		host->dma.dir = DMA_TO_DEVICE;

	/* host->curr.user_pages = (data->flags & MMC_DATA_USERPAGE); */
	host->curr.user_pages = 0;
	box = &nc->cmd[0];
	for (i = 0; i < host->dma.num_ents; i++) {
		box->cmd = CMD_MODE_BOX;

		/* Initialize sg dma address */
		sg->dma_address = page_to_dma(mmc_dev(host->mmc), sg_page(sg))
					+ sg->offset;

		if (i == (host->dma.num_ents - 1))
			box->cmd |= CMD_LC;
		rows = (sg_dma_len(sg) % MCI_FIFOSIZE) ?
			(sg_dma_len(sg) / MCI_FIFOSIZE) + 1 :
			(sg_dma_len(sg) / MCI_FIFOSIZE) ;

		if (data->flags & MMC_DATA_READ) {
			box->src_row_addr = msmsdcc_fifo_addr(host);
			box->dst_row_addr = sg_dma_address(sg);

			box->src_dst_len = (MCI_FIFOSIZE << 16) |
					   (MCI_FIFOSIZE);
			box->row_offset = MCI_FIFOSIZE;

			box->num_rows = rows * ((1 << 16) + 1);
			box->cmd |= CMD_SRC_CRCI(crci);
		} else {
			box->src_row_addr = sg_dma_address(sg);
			box->dst_row_addr = msmsdcc_fifo_addr(host);

			box->src_dst_len = (MCI_FIFOSIZE << 16) |
					   (MCI_FIFOSIZE);
			box->row_offset = (MCI_FIFOSIZE << 16);

			box->num_rows = rows * ((1 << 16) + 1);
			box->cmd |= CMD_DST_CRCI(crci);
		}
		box++;
		sg++;
	}

	/* location of command block must be 64 bit aligned */
	BUG_ON(host->dma.cmd_busaddr & 0x07);

	nc->cmdptr = (host->dma.cmd_busaddr >> 3) | CMD_PTR_LP;
	host->dma.hdr.cmdptr = DMOV_CMD_PTR_LIST |
			       DMOV_CMD_ADDR(host->dma.cmdptr_busaddr);
	host->dma.hdr.complete_func = msmsdcc_dma_complete_func;
	host->dma.hdr.crci_mask = msm_dmov_build_crci_mask(1, crci);

	n = dma_map_sg(mmc_dev(host->mmc), host->dma.sg,
			host->dma.num_ents, host->dma.dir);
	/* dsb inside dma_map_sg will write nc out to mem as well */

	if (n != host->dma.num_ents) {
		pr_err("%s: Unable to map in all sg elements\n",
		       mmc_hostname(host->mmc));
		host->dma.sg = NULL;
		host->dma.num_ents = 0;
		return -ENOMEM;
	}

	return 0;
}

static void
msmsdcc_start_command_deferred(struct msmsdcc_host *host,
				struct mmc_command *cmd, u32 *c)
{
	DBG(host, "op %02x arg %08x flags %08x\n",
	    cmd->opcode, cmd->arg, cmd->flags);

	*c |= (cmd->opcode | MCI_CPSM_ENABLE);

	if (cmd->flags & MMC_RSP_PRESENT) {
		if (cmd->flags & MMC_RSP_136)
			*c |= MCI_CPSM_LONGRSP;
		*c |= MCI_CPSM_RESPONSE;
	}

	if (/*interrupt*/0)
		*c |= MCI_CPSM_INTERRUPT;

	if ((((cmd->opcode == 17) || (cmd->opcode == 18))  ||
	     ((cmd->opcode == 24) || (cmd->opcode == 25))) ||
	      (cmd->opcode == 53))
		*c |= MCI_CSPM_DATCMD;

	if (host->prog_scan && (cmd->opcode == 12)) {
		*c |= MCI_CPSM_PROGENA;
		host->prog_enable = 1;
	}

#ifdef CONFIG_MMC_MSM_PROG_DONE_SCAN
	if ((cmd->opcode == SD_IO_RW_DIRECT)
			&& (host->prog_scan == 1)) {
		*c |= MCI_CPSM_PROGENA;
		host->prog_enable = 1;
	}
#endif

	if (cmd == cmd->mrq->stop)
		*c |= MCI_CSPM_MCIABORT;

	if (host->curr.cmd != NULL) {
		pr_err("%s: Overlapping command requests\n",
		       mmc_hostname(host->mmc));
	}
	host->curr.cmd = cmd;
}

static void
msmsdcc_start_data(struct msmsdcc_host *host, struct mmc_data *data,
			struct mmc_command *cmd, u32 c)
{
	unsigned int datactrl, timeout;
	unsigned long long clks;
	void __iomem *base = host->base;
	unsigned int pio_irqmask = 0;

	host->curr.data = data;
	host->curr.xfer_size = data->blksz * data->blocks;
	host->curr.xfer_remain = host->curr.xfer_size;
	host->curr.data_xfered = 0;
	host->curr.got_dataend = 0;

	memset(&host->pio, 0, sizeof(host->pio));

	datactrl = MCI_DPSM_ENABLE | (data->blksz << 4);

	if (!msmsdcc_config_dma(host, data))
		datactrl |= MCI_DPSM_DMAENABLE;
	else {
		host->pio.sg = data->sg;
		host->pio.sg_len = data->sg_len;
		host->pio.sg_off = 0;

		if (data->flags & MMC_DATA_READ) {
			pio_irqmask = MCI_RXFIFOHALFFULLMASK;
			if (host->curr.xfer_remain < MCI_FIFOSIZE)
				pio_irqmask |= MCI_RXDATAAVLBLMASK;
		} else
			pio_irqmask = MCI_TXFIFOHALFEMPTYMASK;
	}

	if (data->flags & MMC_DATA_READ)
		datactrl |= MCI_DPSM_DIRECTION;

	clks = (unsigned long long)data->timeout_ns * host->clk_rate;
	do_div(clks, 1000000000UL);
	timeout = data->timeout_clks + (unsigned int)clks*2 ;

	if (datactrl & MCI_DPSM_DMAENABLE) {
		/* Save parameters for the exec function */
		host->cmd_timeout = timeout;
		host->cmd_pio_irqmask = pio_irqmask;
		host->cmd_datactrl = datactrl;
		host->cmd_cmd = cmd;

		host->dma.hdr.exec_func = msmsdcc_dma_exec_func;
		host->dma.hdr.user = (void *)host;
		host->dma.busy = 1;

		if (cmd) {
			msmsdcc_start_command_deferred(host, cmd, &c);
			host->cmd_c = c;
		}
		dsb();
		msm_dmov_enqueue_cmd_ext(host->dma.channel, &host->dma.hdr);
		if (data->flags & MMC_DATA_WRITE)
			host->prog_scan = 1;
	} else {
		writel(timeout, base + MMCIDATATIMER);

		writel(host->curr.xfer_size, base + MMCIDATALENGTH);

		writel((readl(host->base + MMCIMASK0) & (~(MCI_IRQ_PIO))) |
				pio_irqmask, host->base + MMCIMASK0);
		msmsdcc_delay(host);	/* Allow parms to be applied */
		writel(datactrl, base + MMCIDATACTRL);

		if (cmd) {
			msmsdcc_delay(host); /* Delay between data/command */
			/* Daisy-chain the command if requested */
			msmsdcc_start_command(host, cmd, c);
		}
	}
}

static void
msmsdcc_start_command(struct msmsdcc_host *host, struct mmc_command *cmd, u32 c)
{
	msmsdcc_start_command_deferred(host, cmd, &c);
	msmsdcc_start_command_exec(host, cmd->arg, c);
}

static void
msmsdcc_data_err(struct msmsdcc_host *host, struct mmc_data *data,
		 unsigned int status)
{
	if (status & MCI_DATACRCFAIL) {
		pr_err("%s: Data CRC error\n",
		       mmc_hostname(host->mmc));
		pr_err("%s: opcode 0x%.8x\n", __func__,
		       data->mrq->cmd->opcode);
		pr_err("%s: blksz %d, blocks %d\n", __func__,
		       data->blksz, data->blocks);
		data->error = -EILSEQ;
	} else if (status & MCI_DATATIMEOUT) {
		/* CRC is optional for the bus test commands, not all
		 * cards respond back with CRC. However controller
		 * waits for the CRC and times out. Hence ignore the
		 * data timeouts during the Bustest.
		 */
		if (!(data->mrq->cmd->opcode == MMC_BUSTEST_W
			|| data->mrq->cmd->opcode == MMC_BUSTEST_R)) {
			pr_err("%s: Data timeout\n",
				 mmc_hostname(host->mmc));
			data->error = -ETIMEDOUT;
		}
	} else if (status & MCI_RXOVERRUN) {
		pr_err("%s: RX overrun\n", mmc_hostname(host->mmc));
		data->error = -EIO;
	} else if (status & MCI_TXUNDERRUN) {
		pr_err("%s: TX underrun\n", mmc_hostname(host->mmc));
		data->error = -EIO;
	} else {
		pr_err("%s: Unknown error (0x%.8x)\n",
		      mmc_hostname(host->mmc), status);
		data->error = -EIO;
	}
}


static int
msmsdcc_pio_read(struct msmsdcc_host *host, char *buffer, unsigned int remain)
{
	void __iomem	*base = host->base;
	uint32_t	*ptr = (uint32_t *) buffer;
	int		count = 0;

	if (remain % 4)
		remain = ((remain >> 2) + 1) << 2;

	while (readl(base + MMCISTATUS) & MCI_RXDATAAVLBL) {

		*ptr = readl(base + MMCIFIFO + (count % MCI_FIFOSIZE));
		ptr++;
		count += sizeof(uint32_t);

		remain -=  sizeof(uint32_t);
		if (remain == 0)
			break;
	}
	return count;
}

static int
msmsdcc_pio_write(struct msmsdcc_host *host, char *buffer,
		  unsigned int remain, u32 status)
{
	void __iomem *base = host->base;
	char *ptr = buffer;

	do {
		unsigned int count, maxcnt, sz;

		maxcnt = status & MCI_TXFIFOEMPTY ? MCI_FIFOSIZE :
						    MCI_FIFOHALFSIZE;
		count = min(remain, maxcnt);

		sz = count % 4 ? (count >> 2) + 1 : (count >> 2);
		writesl(base + MMCIFIFO, ptr, sz);
		ptr += count;
		remain -= count;

		if (remain == 0)
			break;

		status = readl(base + MMCISTATUS);
	} while (status & MCI_TXFIFOHALFEMPTY);

	return ptr - buffer;
}

static irqreturn_t
msmsdcc_pio_irq(int irq, void *dev_id)
{
	struct msmsdcc_host	*host = dev_id;
	void __iomem		*base = host->base;
	uint32_t		status;

	status = readl(base + MMCISTATUS);
	if (((readl(host->base + MMCIMASK0) & status) & (MCI_IRQ_PIO)) == 0)
		return IRQ_NONE;

#if IRQ_DEBUG
	msmsdcc_print_status(host, "irq1-r", status);
#endif

	spin_lock(&host->lock);

	do {
		unsigned long flags;
		unsigned int remain, len;
		char *buffer;

		if (!(status & (MCI_TXFIFOHALFEMPTY | MCI_RXDATAAVLBL)))
			break;

		/* Map the current scatter buffer */
		local_irq_save(flags);
#ifdef CONFIG_HUAWEI_WIFI_SDCC
		if ( NULL == host->pio.sg ) {
			local_irq_restore(flags);
			printk(KERN_ERR"host->pio.sg is NULL\n");
			break;
		}
#endif
		buffer = kmap_atomic(sg_page(host->pio.sg),
				     KM_BIO_SRC_IRQ) + host->pio.sg->offset;
		buffer += host->pio.sg_off;
		remain = host->pio.sg->length - host->pio.sg_off;

		len = 0;
		if (status & MCI_RXACTIVE)
			len = msmsdcc_pio_read(host, buffer, remain);
		if (status & MCI_TXACTIVE)
			len = msmsdcc_pio_write(host, buffer, remain, status);

		/* Unmap the buffer */
		kunmap_atomic(buffer, KM_BIO_SRC_IRQ);
		local_irq_restore(flags);

		host->pio.sg_off += len;
		host->curr.xfer_remain -= len;
		host->curr.data_xfered += len;
		remain -= len;

		if (remain) /* Done with this page? */
			break; /* Nope */

		if (status & MCI_RXACTIVE && host->curr.user_pages)
			flush_dcache_page(sg_page(host->pio.sg));

		if (!--host->pio.sg_len) {
			memset(&host->pio, 0, sizeof(host->pio));
			break;
		}

		/* Advance to next sg */
		host->pio.sg++;
		host->pio.sg_off = 0;

		status = readl(base + MMCISTATUS);
	} while (1);

	if (status & MCI_RXACTIVE && host->curr.xfer_remain < MCI_FIFOSIZE) {
		writel((readl(host->base + MMCIMASK0) & (~(MCI_IRQ_PIO))) |
				MCI_RXDATAAVLBLMASK, host->base + MMCIMASK0);
		if (!host->curr.xfer_remain) {
			/* Delay needed (same port was just written) */
			msmsdcc_delay(host);
			writel((readl(host->base + MMCIMASK0) &
				(~(MCI_IRQ_PIO))) | 0, host->base + MMCIMASK0);
		}
	} else if (!host->curr.xfer_remain)
		writel((readl(host->base + MMCIMASK0) & (~(MCI_IRQ_PIO))) | 0,
				host->base + MMCIMASK0);

	spin_unlock(&host->lock);

	return IRQ_HANDLED;
}

static void
msmsdcc_request_start(struct msmsdcc_host *host, struct mmc_request *mrq);

static irqreturn_t
msmsdcc_irq(int irq, void *dev_id)
{
	struct msmsdcc_host	*host = dev_id;
	void __iomem		*base = host->base;
	u32			status;
	int			ret = 0;
	int			timer = 0;

	spin_lock(&host->lock);

	do {
		struct mmc_command *cmd;
		struct mmc_data *data;

		if (timer) {
			timer = 0;
			msmsdcc_delay(host);
		}

		status = readl(host->base + MMCISTATUS);

		if (((readl(host->base + MMCIMASK0) & status) &
						(~(MCI_IRQ_PIO))) == 0)
			break;

#if IRQ_DEBUG
		msmsdcc_print_status(host, "irq0-r", status);
#endif
		status &= readl(host->base + MMCIMASK0);
		writel(status, host->base + MMCICLEAR);
#if IRQ_DEBUG
		msmsdcc_print_status(host, "irq0-p", status);
#endif

		if ((host->plat->dummy52_required) &&
		    (host->dummy_52_state == DUMMY_52_STATE_SENT)) {
			if (status & MCI_PROGDONE) {
				host->dummy_52_state = DUMMY_52_STATE_NONE;
				host->curr.cmd = NULL;
				spin_unlock(&host->lock);
				msmsdcc_request_start(host, host->curr.mrq);
				return IRQ_HANDLED;
			}
			break;
		}

		data = host->curr.data;
#ifdef CONFIG_MMC_MSM_SDIO_SUPPORT
		if (status & MCI_SDIOINTROPE)
			mmc_signal_sdio_irq(host->mmc);
#endif
		/*
		 * Check for proper command response
		 */
		cmd = host->curr.cmd;
		if ((status & (MCI_CMDSENT | MCI_CMDRESPEND | MCI_CMDCRCFAIL |
			      MCI_CMDTIMEOUT | MCI_PROGDONE)) && cmd) {

			host->curr.cmd = NULL;

			cmd->resp[0] = readl(base + MMCIRESPONSE0);
			cmd->resp[1] = readl(base + MMCIRESPONSE1);
			cmd->resp[2] = readl(base + MMCIRESPONSE2);
			cmd->resp[3] = readl(base + MMCIRESPONSE3);

			if (status & MCI_CMDTIMEOUT) {
#if VERBOSE_COMMAND_TIMEOUTS
				pr_err("%s: Command timeout\n",
				       mmc_hostname(host->mmc));
#endif
				cmd->error = -ETIMEDOUT;
			} else if (status & MCI_CMDCRCFAIL &&
				   cmd->flags & MMC_RSP_CRC) {
				pr_err("%s: Command CRC error\n",
				       mmc_hostname(host->mmc));
				cmd->error = -EILSEQ;
			}

			if (!cmd->data || cmd->error) {
				if (host->curr.data && host->dma.sg)
					msm_dmov_stop_cmd(host->dma.channel,
							  &host->dma.hdr, 0);
				else if (host->curr.data) { /* Non DMA */
					msmsdcc_stop_data(host);
					timer |= msmsdcc_request_end(host,
							cmd->mrq);
				} else { /* host->data == NULL */
					if (!cmd->error && host->prog_enable) {
						if (status & MCI_PROGDONE) {
							host->prog_scan = 0;
							host->prog_enable = 0;
							timer |=
							 msmsdcc_request_end(
								host, cmd->mrq);
						} else
							host->curr.cmd = cmd;
					} else {
						if (host->prog_enable) {
							host->prog_scan = 0;
							host->prog_enable = 0;
						}
						timer |=
							msmsdcc_request_end(
							 host, cmd->mrq);
					}
				}
			} else if (cmd->data) {
				if (!(cmd->data->flags & MMC_DATA_READ))
					msmsdcc_start_data(host, cmd->data,
								NULL, 0);
			}
		}

		if (data) {
			/* Check for data errors */
			if (status & (MCI_DATACRCFAIL|MCI_DATATIMEOUT|
				      MCI_TXUNDERRUN|MCI_RXOVERRUN)) {
				msmsdcc_data_err(host, data, status);
				host->curr.data_xfered = 0;
				if (host->dma.sg)
					msm_dmov_stop_cmd(host->dma.channel,
							  &host->dma.hdr, 0);
				else {
					msmsdcc_reset_and_restore(host);
					if (host->curr.data)
						msmsdcc_stop_data(host);
					if (!data->stop)
						timer |=
						 msmsdcc_request_end(host,
								    data->mrq);
					else {
						msmsdcc_start_command(host,
								     data->stop,
								     0);
						timer = 1;
					}
				}
			}

			/* Check for data done */
			if (!host->curr.got_dataend && (status & MCI_DATAEND))
				host->curr.got_dataend = 1;

			if (host->curr.got_dataend) {
				/*
				 * If DMA is still in progress, we complete
				 * via the completion handler
				 */
				if (!host->dma.busy) {
					/*
					 * There appears to be an issue in the
					 * controller where if you request a
					 * small block transfer (< fifo size),
					 * you may get your DATAEND/DATABLKEND
					 * irq without the PIO data irq.
					 *
					 * Check to see if theres still data
					 * to be read, and simulate a PIO irq.
					 */
					if (readl(host->base + MMCISTATUS) &
						       MCI_RXDATAAVLBL) {
						spin_unlock(&host->lock);
						msmsdcc_pio_irq(1, host);
						spin_lock(&host->lock);
					}
					msmsdcc_stop_data(host);
					if (!data->error)
						host->curr.data_xfered =
							host->curr.xfer_size;

					if (!data->stop)
						timer |= msmsdcc_request_end(
							  host, data->mrq);
					else {
						msmsdcc_start_command(host,
							      data->stop, 0);
						timer = 1;
					}
				}
			}
		}

		ret = 1;
	} while (status);

	spin_unlock(&host->lock);

	return IRQ_RETVAL(ret);
}

#ifdef CONFIG_HUAWEI_WIFI_SDCC
#define MSMSDCC_POLLING_RETRIES         10000000
static int msmsdcc_wait_prog_done(struct msmsdcc_host *host)
{
	unsigned int		i = 0;
	unsigned int		status = 0;

	while (i++ < MSMSDCC_POLLING_RETRIES) {
		status = readl(host->base + MMCISTATUS);
		if (status & MCI_CMDSENT)
			printk("command is sent out\n");
		if (status & MCI_PROGDONE)
			break;
	}
	if (i >= MSMSDCC_POLLING_RETRIES) {
		printk("wait PROG_DONE fail\n");
		return -1;
	}
	return 0;
}

static int msmsdcc_send_dummy_cmd52_read(struct msmsdcc_host *host)
{
	unsigned int	retries = MSMSDCC_POLLING_RETRIES;
	void __iomem	*base = host->base;
	unsigned int	status = 0;

	writel(MCI_PROGDONECLR, host->base + MMCICLEAR);

	if (readl(base + MMCICOMMAND) & MCI_CPSM_ENABLE) {
		writel(0, base + MMCICOMMAND);
		udelay(2 + ((5 * 1000000) / host->clk_rate));
	}
	writel(0, base + MMCIARGUMENT);
	writel(52 | MCI_CPSM_ENABLE | MCI_CPSM_RESPONSE | MCI_CPSM_PROGENA, base + MMCICOMMAND);
	msmsdcc_wait_prog_done(host);

	while(retries) {
		status = readl(host->base + MMCISTATUS);

		if (status & MCI_CMDCRCFAIL) {
			printk("Sending dummy SD CMD52 failed: -EILSEQ\n");
			return -EILSEQ;
		}
		if (status & MCI_CMDTIMEOUT) {
			printk("Sending dummy SD CMD52 failed: -ETIMEDOUT\n");
			return -ETIMEDOUT;
		}
		if (status & (MCI_CMDSENT | MCI_CMDRESPEND))
			return 0;
		retries--;
	}

	printk("Sending dummy SD CMD52 failed: -ETIMEDOUT\n");
	return -ETIMEDOUT;
}
#endif

static void
msmsdcc_request_start(struct msmsdcc_host *host, struct mmc_request *mrq)
{
	if (mrq->data && mrq->data->flags & MMC_DATA_READ) {
		/* Queue/read data, daisy-chain command when data starts */
		msmsdcc_start_data(host, mrq->data, mrq->cmd, 0);
	} else {
		msmsdcc_start_command(host, mrq->cmd, 0);
	}
}

static void
msmsdcc_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct msmsdcc_host *host = mmc_priv(mmc);
	unsigned long		flags;

	WARN_ON(host->curr.mrq != NULL);

        WARN_ON(host->pwr == 0);

	spin_lock_irqsave(&host->lock, flags);

	if (host->eject) {
		if (mrq->data && !(mrq->data->flags & MMC_DATA_READ)) {
			mrq->cmd->error = 0;
			mrq->data->bytes_xfered = mrq->data->blksz *
						  mrq->data->blocks;
		} else
			mrq->cmd->error = -ENOMEDIUM;

		spin_unlock_irqrestore(&host->lock, flags);
		mmc_request_done(mmc, mrq);
		return;
	}

	host->curr.mrq = mrq;
	
#ifdef CONFIG_HUAWEI_WIFI_SDCC 	
	if ( host->pdev_id == SDCC_WIFI_SLOT ) {
		/* add fix for dummy52 issue*/
		if (host->pre_cmd_with_data) {
			if ( mrq->data ) {
				host->saved_irq0mask = readl(host->base + MMCIMASK0);
				writel(0, host->base + MMCIMASK0);
				writel(0x018007FF, host->base + MMCICLEAR);
				msmsdcc_send_dummy_cmd52_read(host);
				writel(0x18007ff, host->base + MMCICLEAR);
				writel(host->saved_irq0mask, host->base + MMCIMASK0);
			}
		    host->pre_cmd_with_data = 0;
		}
		if ( mrq->data ) {
			host->pre_cmd_with_data = 1;
		}
	}
#endif

	if (host->plat->dummy52_required) {
		if (host->dummy_52_needed) {
			if (mrq->data) {
				host->dummy_52_state = DUMMY_52_STATE_SENT;
				msmsdcc_start_command(host, &dummy52cmd,
						      MCI_CPSM_PROGENA);
				spin_unlock_irqrestore(&host->lock, flags);
				return;
			}
			host->dummy_52_needed = 0;
		}
		if ((mrq->cmd->opcode == SD_IO_RW_EXTENDED) && (mrq->data))
			host->dummy_52_needed = 1;
	}

	msmsdcc_request_start(host, mrq);
	spin_unlock_irqrestore(&host->lock, flags);
}

static void
msmsdcc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct msmsdcc_host *host = mmc_priv(mmc);
	u32 clk = 0, pwr = 0;
	int rc;

	DBG(host, "ios->clock = %u\n", ios->clock);

	if (ios->clock) {

		if (!host->clks_on) {
			if (!IS_ERR(host->pclk))
				clk_enable(host->pclk);
			clk_enable(host->clk);
			host->clks_on = 1;
		}

		if ((ios->clock < host->plat->msmsdcc_fmax) &&
				(ios->clock > host->plat->msmsdcc_fmid))
			ios->clock = host->plat->msmsdcc_fmid;

		if (ios->clock != host->clk_rate) {
			rc = clk_set_rate(host->clk, ios->clock);
			WARN_ON(rc < 0);
			host->clk_rate = ios->clock;
		}
		clk |= MCI_CLK_ENABLE;
	}

	if (ios->bus_width == MMC_BUS_WIDTH_8)
		clk |= MCI_CLK_WIDEBUS_8;
	else if (ios->bus_width == MMC_BUS_WIDTH_4)
		clk |= MCI_CLK_WIDEBUS_4;
	else
		clk |= MCI_CLK_WIDEBUS_1;

	if (ios->clock > 400000 && msmsdcc_pwrsave)
		clk |= MCI_CLK_PWRSAVE;

	clk |= MCI_CLK_FLOWENA;
	clk |= MCI_CLK_SELECTIN; /* feedback clock */

	if (host->plat->translate_vdd)
		pwr |= host->plat->translate_vdd(mmc_dev(mmc), ios->vdd);

	switch (ios->power_mode) {
	case MMC_POWER_OFF:
		htc_pwrsink_set(PWRSINK_SDCARD, 0);
		break;
	case MMC_POWER_UP:
		pwr |= MCI_PWR_UP;
		break;
	case MMC_POWER_ON:
		htc_pwrsink_set(PWRSINK_SDCARD, 100);
		pwr |= MCI_PWR_ON;
		break;
	}

	if (ios->bus_mode == MMC_BUSMODE_OPENDRAIN)
		pwr |= MCI_OD;

	writel(clk, host->base + MMCICLOCK);

	udelay(50);

	if (host->pwr != pwr) {
		host->pwr = pwr;
		writel(pwr, host->base + MMCIPOWER);
	}

	if (!(clk & MCI_CLK_ENABLE) && host->clks_on) {
		clk_disable(host->clk);
		if (!IS_ERR(host->pclk))
			clk_disable(host->pclk);
		host->clks_on = 0;
	}
}

static int msmsdcc_get_ro(struct mmc_host *mmc)
{
	int wpswitch_status = -ENOSYS;
	struct msmsdcc_host *host = mmc_priv(mmc);

	if (host->plat->wpswitch) {
		wpswitch_status = host->plat->wpswitch(mmc_dev(mmc));
		if (wpswitch_status < 0)
			wpswitch_status = -ENOSYS;
	}
	pr_debug("%s: Card read-only status %d\n", __func__, wpswitch_status);
	return wpswitch_status;
}

#ifdef CONFIG_MMC_MSM_SDIO_SUPPORT
static void msmsdcc_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct msmsdcc_host *host = mmc_priv(mmc);

	if (enable) {
		host->mci_irqenable |= MCI_SDIOINTOPERMASK;
		writel(readl(host->base + MMCIMASK0) | MCI_SDIOINTOPERMASK,
			       host->base + MMCIMASK0);
	} else {
		host->mci_irqenable &= ~MCI_SDIOINTOPERMASK;
		writel(readl(host->base + MMCIMASK0) & ~MCI_SDIOINTOPERMASK,
		       host->base + MMCIMASK0);
	}
}
#endif /* CONFIG_MMC_MSM_SDIO_SUPPORT */

static const struct mmc_host_ops msmsdcc_ops = {
	.request	= msmsdcc_request,
	.set_ios	= msmsdcc_set_ios,
	.get_ro		= msmsdcc_get_ro,
#ifdef CONFIG_MMC_MSM_SDIO_SUPPORT
	.enable_sdio_irq = msmsdcc_enable_sdio_irq,
#endif
#ifdef CONFIG_MMC_AUTO_SUSPEND
	.auto_suspend	= msmsdcc_auto_suspend,
#endif
};

static void
msmsdcc_check_status(unsigned long data)
{
	struct msmsdcc_host *host = (struct msmsdcc_host *)data;
	unsigned int status;

	if (!host->plat->status) {
		mmc_detect_change(host->mmc, 0);
	} else {
		status = host->plat->status(mmc_dev(host->mmc));
		host->eject = !status;
		if (status ^ host->oldstat) {
			pr_info("%s: Slot status change detected (%d -> %d)\n",
			       mmc_hostname(host->mmc), host->oldstat, status);
			mmc_detect_change(host->mmc, 0);
		}
		host->oldstat = status;
	}
}

static irqreturn_t
msmsdcc_platform_status_irq(int irq, void *dev_id)
{
	struct msmsdcc_host *host = dev_id;

	pr_debug("%s: %d\n", __func__, irq);
	msmsdcc_check_status((unsigned long) host);
	return IRQ_HANDLED;
}

static irqreturn_t
msmsdcc_platform_sdiowakeup_irq(int irq, void *dev_id)
{
	pr_info("%s: SDIO Wake up IRQ : %d\n", __func__, irq);
	return IRQ_HANDLED;
}

static void
msmsdcc_status_notify_cb(int card_present, void *dev_id)
{
	struct msmsdcc_host *host = dev_id;

	pr_debug("%s: card_present %d\n", mmc_hostname(host->mmc),
	       card_present);
	msmsdcc_check_status((unsigned long) host);
}

static int
msmsdcc_init_dma(struct msmsdcc_host *host)
{
	memset(&host->dma, 0, sizeof(struct msmsdcc_dma_data));
	host->dma.host = host;
	host->dma.channel = -1;

	if (!host->dmares)
		return -ENODEV;

	host->dma.nc = dma_alloc_coherent(NULL,
					  sizeof(struct msmsdcc_nc_dmadata),
					  &host->dma.nc_busaddr,
					  GFP_KERNEL);
	if (host->dma.nc == NULL) {
		pr_err("Unable to allocate DMA buffer\n");
		return -ENOMEM;
	}
	memset(host->dma.nc, 0x00, sizeof(struct msmsdcc_nc_dmadata));
	host->dma.cmd_busaddr = host->dma.nc_busaddr;
	host->dma.cmdptr_busaddr = host->dma.nc_busaddr +
				offsetof(struct msmsdcc_nc_dmadata, cmdptr);
	host->dma.channel = host->dmares->start;

	return 0;
}

static ssize_t
show_polling(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	int poll;
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);
	poll = !!(mmc->caps & MMC_CAP_NEEDS_POLL);
	spin_unlock_irqrestore(&host->lock, flags);

	return snprintf(buf, PAGE_SIZE, "%d\n", poll);
}

static ssize_t
set_polling(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	int value;
	unsigned long flags;

	sscanf(buf, "%d", &value);

	spin_lock_irqsave(&host->lock, flags);
	if (value) {
		mmc->caps |= MMC_CAP_NEEDS_POLL;
		if (host->pdev_id == SDCC_WIFI_SLOT) {
		printk("%s: no need to enable polling for slot 2 \n",__FUNCTION__);
		mmc->caps &= ~MMC_CAP_NEEDS_POLL;
        }
		mmc_detect_change(host->mmc, 0);
	} else {
		mmc->caps &= ~MMC_CAP_NEEDS_POLL;
	}
#ifdef CONFIG_HAS_EARLYSUSPEND
	host->polling_enabled = mmc->caps & MMC_CAP_NEEDS_POLL;
#endif
	spin_unlock_irqrestore(&host->lock, flags);
	return count;
}

void bcm_wlan_power_on(int enable)
{
	int ret = 0;

   	if (enable)
	{
		/* pull high */
		ret = mpp_config_digital_out(14,
				MPP_CFG(MPP_DLOGIC_LVL_MSME, MPP_DLOGIC_OUT_CTRL_HIGH)); 
		if (ret) 
		{
			printk(KERN_ERR "%s: WL_REG_ON mpp_config_digital_out failed to up(%d)\n",
					__func__, ret);
			return;
		}
		else
		{
			printk(KERN_DEBUG "%s: WL_REG_ON voltage level successed\n",__func__);
		}
		mdelay(1);
		// WLAN chip to reset
		/* pull up */
		ret = mpp_config_digital_out(20,
				MPP_CFG(MPP_DLOGIC_LVL_MSMP, MPP_DLOGIC_OUT_CTRL_HIGH));  
		if (ret) 
		{
			printk(KERN_ERR "%s: EXT_POR_L failed to pull up(%d)\n",
					__func__, ret);
			return;
		}
		printk(KERN_DEBUG "%s: EXT_POR_L successed to pull up\n",__func__);
		mdelay(150);
	}else{

		// WLAN 1.2V down
		ret = mpp_config_digital_out(14,
				MPP_CFG(MPP_DLOGIC_LVL_MSME, MPP_DLOGIC_OUT_CTRL_LOW));  /* pull high */
		if (ret) 
		{
			printk(KERN_ERR "%s: WL_REG_ON level mpp_config_digital_out failed to down(%d)\n",
					__func__, ret);
			return;
		}
		printk(KERN_DEBUG "%s: WL_REG_ON level pull down\n",__func__);
		mdelay(1);
		// WLAN chip down 
		ret = mpp_config_digital_out(20,
				MPP_CFG(MPP_DLOGIC_LVL_MSMP, MPP_DLOGIC_OUT_CTRL_LOW));  /* pull up */
		if (ret) 
		{
			printk(KERN_ERR "%s: EXT_POR_L failed to pull down(%d)\n",
					__func__, ret);
			return;
		}
		else
		{
			printk(KERN_INFO "%s: EXT_POR_Lsuccessed to  pull down\n",__func__);
		}

		mdelay(1);
	}
}

void bcm_wlan_power_off(int i)
{
	bcm_wlan_power_on(0);
}

EXPORT_SYMBOL(bcm_wlan_power_off);
EXPORT_SYMBOL(bcm_wlan_power_on);

static DEVICE_ATTR(polling, S_IRUGO | S_IWUSR,
		show_polling, set_polling);
static struct attribute *dev_attrs[] = {
	&dev_attr_polling.attr,
	NULL,
};
static struct attribute_group dev_attr_grp = {
	.attrs = dev_attrs,
};

#ifdef CONFIG_HAS_EARLYSUSPEND
#ifndef CONFIG_HUAWEI_WIFI_SDCC
static void msmsdcc_early_suspend(struct early_suspend *h)
{
	struct msmsdcc_host *host =
		container_of(h, struct msmsdcc_host, early_suspend);
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);
	host->polling_enabled = host->mmc->caps & MMC_CAP_NEEDS_POLL;
	host->mmc->caps &= ~MMC_CAP_NEEDS_POLL;
	spin_unlock_irqrestore(&host->lock, flags);
};
static void msmsdcc_late_resume(struct early_suspend *h)
{
	struct msmsdcc_host *host =
		container_of(h, struct msmsdcc_host, early_suspend);
	unsigned long flags;

	if (host->polling_enabled) {
		spin_lock_irqsave(&host->lock, flags);
		host->mmc->caps |= MMC_CAP_NEEDS_POLL;
		mmc_detect_change(host->mmc, 0);
		spin_unlock_irqrestore(&host->lock, flags);
	}
};
#endif
#endif

static int
msmsdcc_probe(struct platform_device *pdev)
{
	struct mmc_platform_data *plat = pdev->dev.platform_data;
	struct msmsdcc_host *host;
	struct mmc_host *mmc;
	struct resource *irqres = NULL;
	struct resource *memres = NULL;
	struct resource *dmares = NULL;
	int ret;
	int i;

	/* must have platform data */
	if (!plat) {
		pr_err("%s: Platform data not available\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	if (pdev->id < 1 || pdev->id > 5)
		return -EINVAL;

	if (pdev->resource == NULL || pdev->num_resources < 3) {
		pr_err("%s: Invalid resource\n", __func__);
		return -ENXIO;
	}

	for (i = 0; i < pdev->num_resources; i++) {
		if (pdev->resource[i].flags & IORESOURCE_MEM)
			memres = &pdev->resource[i];
		if (pdev->resource[i].flags & IORESOURCE_IRQ)
			irqres = &pdev->resource[i];
		if (pdev->resource[i].flags & IORESOURCE_DMA)
			dmares = &pdev->resource[i];
	}
	if (!irqres || !memres) {
		pr_err("%s: Invalid resource\n", __func__);
		return -ENXIO;
	}

	/*
	 * Setup our host structure
	 */

	mmc = mmc_alloc_host(sizeof(struct msmsdcc_host), &pdev->dev);
	if (!mmc) {
		ret = -ENOMEM;
		goto out;
	}

	host = mmc_priv(mmc);
	host->pdev_id = pdev->id;
	host->plat = plat;
	host->mmc = mmc;
	host->curr.cmd = NULL;

	host->base = ioremap(memres->start, PAGE_SIZE);
	if (!host->base) {
		ret = -ENOMEM;
		goto host_free;
	}

	host->irqres = irqres;
	host->memres = memres;
	host->dmares = dmares;
	spin_lock_init(&host->lock);

#ifdef CONFIG_MMC_EMBEDDED_SDIO
	if (plat->embedded_sdio)
		mmc_set_embedded_sdio_data(mmc,
					   &plat->embedded_sdio->cis,
					   &plat->embedded_sdio->cccr,
					   plat->embedded_sdio->funcs,
					   plat->embedded_sdio->num_funcs);
#endif

	tasklet_init(&host->dma_tlet, msmsdcc_dma_complete_tlet,
			(unsigned long)host);

	/*
	 * Setup DMA
	 */
	ret = msmsdcc_init_dma(host);
	if (ret)
		goto ioremap_free;

	/*
	 * Setup main peripheral bus clock
	 */
	host->pclk = clk_get(&pdev->dev, "sdc_pclk");
	if (!IS_ERR(host->pclk)) {
		ret = clk_enable(host->pclk);
		if (ret)
			goto pclk_put;

		host->pclk_rate = clk_get_rate(host->pclk);
	}

	/*
	 * Setup SDC MMC clock
	 */
	host->clk = clk_get(&pdev->dev, "sdc_clk");
	if (IS_ERR(host->clk)) {
		ret = PTR_ERR(host->clk);
		goto pclk_disable;
	}

	ret = clk_set_rate(host->clk, plat->msmsdcc_fmin);
	if (ret) {
		pr_err("%s: Clock rate set failed (%d)\n", __func__, ret);
		goto clk_put;
	}

	ret = clk_enable(host->clk);
	if (ret)
		goto clk_put;

	host->clk_rate = clk_get_rate(host->clk);

	host->clks_on = 1;

	/*
	 * Setup MMC host structure
	 */
	mmc->ops = &msmsdcc_ops;
	mmc->f_min = plat->msmsdcc_fmin;
	mmc->f_max = plat->msmsdcc_fmax;
	mmc->ocr_avail = plat->ocr_mask;
	mmc->caps |= plat->mmc_bus_width;

	mmc->caps |= MMC_CAP_MMC_HIGHSPEED | MMC_CAP_SD_HIGHSPEED;

	if (plat->nonremovable)
		mmc->caps |= MMC_CAP_NONREMOVABLE;
#ifdef CONFIG_MMC_MSM_SDIO_SUPPORT
	mmc->caps |= MMC_CAP_SDIO_IRQ;
#endif

	mmc->max_phys_segs = NR_SG;
	mmc->max_hw_segs = NR_SG;
	mmc->max_blk_size = 4096;	/* MCI_DATA_CTL BLOCKSIZE up to 4096 */
	mmc->max_blk_count = 65536;

	mmc->max_req_size = 33554432;	/* MCI_DATA_LENGTH is 25 bits */
	mmc->max_seg_size = mmc->max_req_size;

	writel(0, host->base + MMCIMASK0);
	writel(MCI_CLEAR_STATIC_MASK, host->base + MMCICLEAR);

	/* Delay needed (MMCIMASK0 was just written above) */
	msmsdcc_delay(host);
	writel(MCI_IRQENABLE, host->base + MMCIMASK0);
	host->mci_irqenable = MCI_IRQENABLE;

	ret = request_irq(irqres->start, msmsdcc_irq, IRQF_SHARED,
			  DRIVER_NAME " (cmd)", host);
	if (ret)
		goto clk_disable;
/*
  *according to qualcomm SR :
  *In 7225 code, 
  * ret = request_irq(cmd_irqres->start, msmsdcc_irq, IRQF_SHARED, DRIVER_NAME " (cmd)", host); 
  * ret = request_irq(pio_irqres->start, msmsdcc_pio_irq, IRQF_SHARED, DRIVER_NAME " (pio)", host); 
  * and devices.c: 
  *{ 
  *    .start = INT_SDC1_0, 
  *    .end = INT_SDC1_0, 
  *    .flags = IORESOURCE_IRQ, 
  *    .name = "cmd_irq", 
  *}, 
  *{ 
  *    .start = INT_SDC1_1, 
  *    .end = INT_SDC1_1, 
  *    .flags = IORESOURCE_IRQ, 
  *    .name = "pio_irq", 
  * }, 
  * So in fact, pio irq use SDC1_1 and dma irq use SDC1_0. 
  *
  *in our current devices-msm7x25.c 
  *	{
  *		.start	= INT_SDC1_0,
  *		.end	       = INT_SDC1_1,
  *		.flags	= IORESOURCE_IRQ,
  *	},
  * so the corresponding interrupt souce of msmsdcc_pio_irq should be irqres->end rather than irqres->start
  */
       //for sd slot      
	if(SDCC_SD_SLOT == pdev->id) 
	 	ret = request_irq(irqres->end, msmsdcc_pio_irq, IRQF_SHARED,
				  DRIVER_NAME " (pio)", host);
	else
	 	ret = request_irq(irqres->start, msmsdcc_pio_irq, IRQF_SHARED,
				  DRIVER_NAME " (pio)", host);
		
	if (ret)
		goto irq_free;

	if (plat->sdiowakeup_irq) {
		ret = request_irq(plat->sdiowakeup_irq,
			msmsdcc_platform_sdiowakeup_irq,
			IRQF_SHARED | IRQF_TRIGGER_FALLING,
			DRIVER_NAME "sdiowakeup", host);
		if (ret) {
			pr_err("Unable to get sdio wakeup IRQ %d (%d)\n",
				plat->sdiowakeup_irq, ret);
			goto irq_free;
		} else {
			set_irq_wake(plat->sdiowakeup_irq, 1);
			disable_irq(plat->sdiowakeup_irq);
		}
	}

	/*
	 * Setup card detect change
	 */

	if (plat->status) {
		host->oldstat = plat->status(mmc_dev(host->mmc));
		host->eject = !host->oldstat;
	}

	if (plat->status_irq) {
		ret = request_irq(plat->status_irq,
				  msmsdcc_platform_status_irq,
				  IRQF_SHARED | plat->irq_flags,
				  DRIVER_NAME " (slot)",
				  host);
		if (ret) {
			pr_err("Unable to get slot IRQ %d (%d)\n",
			       plat->status_irq, ret);
			goto sdiowakeup_irq_free;
		}
	} else if (plat->register_status_notify) {
		plat->register_status_notify(msmsdcc_status_notify_cb, host);
	} else if (!plat->status)
		pr_err("%s: No card detect facilities available\n",
		       mmc_hostname(mmc));

	mmc_set_drvdata(pdev, mmc);
	mmc_add_host(mmc);

#ifdef CONFIG_HAS_EARLYSUSPEND
#ifndef CONFIG_HUAWEI_WIFI_SDCC
	host->early_suspend.suspend = msmsdcc_early_suspend;
	host->early_suspend.resume  = msmsdcc_late_resume;
	host->early_suspend.level   = EARLY_SUSPEND_LEVEL_DISABLE_FB;
	register_early_suspend(&host->early_suspend);
#endif
#endif

	pr_info("%s: Qualcomm MSM SDCC at 0x%016llx irq %d,%d dma %d\n",
	       mmc_hostname(mmc), (unsigned long long)memres->start,
	       (unsigned int) irqres->start,
	       (unsigned int) plat->status_irq, host->dma.channel);

	pr_info("%s: 8 bit data mode %s\n", mmc_hostname(mmc),
		(mmc->caps & MMC_CAP_8_BIT_DATA ? "enabled" : "disabled"));
	pr_info("%s: 4 bit data mode %s\n", mmc_hostname(mmc),
	       (mmc->caps & MMC_CAP_4_BIT_DATA ? "enabled" : "disabled"));
	pr_info("%s: polling status mode %s\n", mmc_hostname(mmc),
	       (mmc->caps & MMC_CAP_NEEDS_POLL ? "enabled" : "disabled"));
	pr_info("%s: MMC clock %u -> %u Hz, PCLK %u Hz\n",
	       mmc_hostname(mmc), plat->msmsdcc_fmin, plat->msmsdcc_fmax,
							host->pclk_rate);
	pr_info("%s: Slot eject status = %d\n", mmc_hostname(mmc),
	       host->eject);
	pr_info("%s: Power save feature enable = %d\n",
	       mmc_hostname(mmc), msmsdcc_pwrsave);

	if (host->dma.channel != -1) {
		pr_info("%s: DM non-cached buffer at %p, dma_addr 0x%.8x\n",
		       mmc_hostname(mmc), host->dma.nc, host->dma.nc_busaddr);
		pr_info("%s: DM cmd busaddr 0x%.8x, cmdptr busaddr 0x%.8x\n",
		       mmc_hostname(mmc), host->dma.cmd_busaddr,
		       host->dma.cmdptr_busaddr);
	} else
		pr_info("%s: PIO transfer enabled\n", mmc_hostname(mmc));

#if defined(CONFIG_DEBUG_FS)
	msmsdcc_dbg_createhost(host);
#endif
	if (!plat->status_irq) {
		ret = sysfs_create_group(&pdev->dev.kobj, &dev_attr_grp);
		if (ret)
			goto platform_irq_free;
	}
	return 0;

 platform_irq_free:
	if (plat->status_irq)
		free_irq(plat->status_irq, host);
 sdiowakeup_irq_free:
	if (plat->sdiowakeup_irq) {
		set_irq_wake(plat->sdiowakeup_irq, 0);
		free_irq(plat->sdiowakeup_irq, host);
	}
 irq_free:
	free_irq(irqres->start, host);
 clk_disable:
	clk_disable(host->clk);
 clk_put:
	clk_put(host->clk);
 pclk_disable:
	if (!IS_ERR(host->pclk))
		clk_disable(host->pclk);
 pclk_put:
	if (!IS_ERR(host->pclk))
		clk_put(host->pclk);

	dma_free_coherent(NULL, sizeof(struct msmsdcc_nc_dmadata),
			host->dma.nc, host->dma.nc_busaddr);
 ioremap_free:
	iounmap(host->base);
 host_free:
	mmc_free_host(mmc);
 out:
	return ret;
}

static int msmsdcc_remove(struct platform_device *pdev)
{
	struct mmc_host *mmc = mmc_get_drvdata(pdev);
	struct mmc_platform_data *plat;
	struct msmsdcc_host *host;

	if (!mmc)
		return -ENXIO;

	host = mmc_priv(mmc);

	DBG(host, "Removing SDCC2 device = %d\n", pdev->id);
	plat = host->plat;

	if (!plat->status_irq)
		sysfs_remove_group(&pdev->dev.kobj, &dev_attr_grp);

	tasklet_kill(&host->dma_tlet);
	mmc_remove_host(mmc);

	if (plat->status_irq)
		free_irq(plat->status_irq, host);

	if (plat->sdiowakeup_irq) {
		set_irq_wake(plat->sdiowakeup_irq, 0);
		free_irq(plat->sdiowakeup_irq, host);
	}

	free_irq(host->irqres->start, host);

	writel(0, host->base + MMCIMASK0);
	writel(0, host->base + MMCIMASK1);
	writel(MCI_CLEAR_STATIC_MASK, host->base + MMCICLEAR);
	writel(0, host->base + MMCIDATACTRL);
	writel(0, host->base + MMCICOMMAND);

	clk_put(host->clk);
	if (!IS_ERR(host->pclk))
		clk_put(host->pclk);

	dma_free_coherent(NULL, sizeof(struct msmsdcc_nc_dmadata),
			host->dma.nc, host->dma.nc_busaddr);
	iounmap(host->base);
	mmc_free_host(mmc);

#ifdef CONFIG_HAS_EARLYSUSPEND
#ifndef CONFIG_HUAWEI_WIFI_SDCC
	unregister_early_suspend(&host->early_suspend);
#endif
#endif

	return 0;
}

#ifdef CONFIG_PM
static int
msmsdcc_suspend(struct platform_device *dev, pm_message_t state)
{
	struct mmc_host *mmc = mmc_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	int rc = 0;

#ifdef CONFIG_MMC_AUTO_SUSPEND
	if (test_and_set_bit(0, &host->suspended))
		return 0;
#endif
	if (mmc) {
		if (host->plat->status_irq)
			disable_irq(host->plat->status_irq);

		if (!mmc->card || mmc->card->type != MMC_TYPE_SDIO)
			rc = mmc_suspend_host(mmc, state);
		if (!rc) {
			writel(0, host->base + MMCIMASK0);

			if (host->clks_on) {
				clk_disable(host->clk);
				if (!IS_ERR(host->pclk))
					clk_disable(host->pclk);
				host->clks_on = 0;
			}
		}

		if (host->plat->sdiowakeup_irq)
			enable_irq(host->plat->sdiowakeup_irq);
	}
	return rc;
}

static int
msmsdcc_resume(struct platform_device *dev)
{
	struct mmc_host *mmc = mmc_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	unsigned long flags;

#ifdef CONFIG_MMC_AUTO_SUSPEND
	if (!test_and_clear_bit(0, &host->suspended))
		return 0;
#endif
	if (mmc) {
		spin_lock_irqsave(&host->lock, flags);
		if (!host->clks_on) {
			if (!IS_ERR(host->pclk))
				clk_enable(host->pclk);
			clk_enable(host->clk);
			host->clks_on = 1;
		}

		writel(host->mci_irqenable, host->base + MMCIMASK0);

		spin_unlock_irqrestore(&host->lock, flags);

		if (host->plat->sdiowakeup_irq)
			disable_irq(host->plat->sdiowakeup_irq);

		if (!mmc->card || mmc->card->type != MMC_TYPE_SDIO)
			mmc_resume_host(mmc);
		if (host->plat->status_irq)
			enable_irq(host->plat->status_irq);

	}
	return 0;
}
#else
#define msmsdcc_suspend NULL
#define msmsdcc_resume NULL
#endif

#ifdef CONFIG_MMC_AUTO_SUSPEND
static int msmsdcc_auto_suspend(struct mmc_host *host, int suspend)
{
	struct platform_device *pdev;
	pdev = container_of(host->parent, struct platform_device, dev);

	if (suspend)
		return msmsdcc_suspend(pdev, PMSG_AUTO_SUSPEND);
	else
		return msmsdcc_resume(pdev);
}
#else
#define msmsdcc_auto_suspend NULL
#endif

static struct platform_driver msmsdcc_driver = {
	.probe		= msmsdcc_probe,
	.remove		= msmsdcc_remove,
	.suspend	= msmsdcc_suspend,
	.resume		= msmsdcc_resume,
	.driver		= {
		.name	= "msm_sdcc",
	},
};

static int __init msmsdcc_init(void)
{
#if defined(CONFIG_DEBUG_FS)
	int ret = 0;
	ret = msmsdcc_dbg_init();
	if (ret) {
		pr_err("Failed to create debug fs dir \n");
		return ret;
	}
#endif
	wifi_chip_is_bcm = board_support_bcm_wifi(NULL);
    printk(KERN_ERR"%s: now wifi_chip_is_bcm is set to %d\n", __func__, wifi_chip_is_bcm);

	return platform_driver_register(&msmsdcc_driver);
}

static void __exit msmsdcc_exit(void)
{
	platform_driver_unregister(&msmsdcc_driver);

#if defined(CONFIG_DEBUG_FS)
	debugfs_remove(debugfs_file);
	debugfs_remove(debugfs_dir);
#endif
}

module_init(msmsdcc_init);
module_exit(msmsdcc_exit);

MODULE_DESCRIPTION("Qualcomm Multimedia Card Interface driver");
MODULE_LICENSE("GPL");

#if defined(CONFIG_DEBUG_FS)

static int
msmsdcc_dbg_state_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t
msmsdcc_dbg_state_read(struct file *file, char __user *ubuf,
		       size_t count, loff_t *ppos)
{
	struct msmsdcc_host *host = (struct msmsdcc_host *) file->private_data;
	char buf[1024];
	int max, i;

	i = 0;
	max = sizeof(buf) - 1;

	i += scnprintf(buf + i, max - i, "STAT: %p %p %p\n", host->curr.mrq,
		       host->curr.cmd, host->curr.data);
	if (host->curr.cmd) {
		struct mmc_command *cmd = host->curr.cmd;

		i += scnprintf(buf + i, max - i, "CMD : %.8x %.8x %.8x\n",
			      cmd->opcode, cmd->arg, cmd->flags);
	}
	if (host->curr.data) {
		struct mmc_data *data = host->curr.data;
		i += scnprintf(buf + i, max - i,
			      "DAT0: %.8x %.8x %.8x %.8x %.8x %.8x\n",
			      data->timeout_ns, data->timeout_clks,
			      data->blksz, data->blocks, data->error,
			      data->flags);
		i += scnprintf(buf + i, max - i, "DAT1: %.8x %.8x %.8x %p\n",
			      host->curr.xfer_size, host->curr.xfer_remain,
			      host->curr.data_xfered, host->dma.sg);
	}

	return simple_read_from_buffer(ubuf, count, ppos, buf, i);
}

static const struct file_operations msmsdcc_dbg_state_ops = {
	.read	= msmsdcc_dbg_state_read,
	.open	= msmsdcc_dbg_state_open,
};

static void msmsdcc_dbg_createhost(struct msmsdcc_host *host)
{
	if (debugfs_dir) {
		debugfs_file = debugfs_create_file(mmc_hostname(host->mmc),
							0644, debugfs_dir, host,
							&msmsdcc_dbg_state_ops);
	}
}

static int __init msmsdcc_dbg_init(void)
{
	int err;

	debugfs_dir = debugfs_create_dir("msmsdcc", 0);
	if (IS_ERR(debugfs_dir)) {
		err = PTR_ERR(debugfs_dir);
		debugfs_dir = NULL;
		return err;
	}

	return 0;
}
#endif
