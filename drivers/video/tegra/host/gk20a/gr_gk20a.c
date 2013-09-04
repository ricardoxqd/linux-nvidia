/*
 * drivers/video/tegra/host/gk20a/gr_gk20a.c
 *
 * GK20A Graphics
 *
 * Copyright (c) 2011-2013, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/delay.h>	/* for udelay */
#include <linux/mm.h>		/* for totalram_pages */
#include <linux/scatterlist.h>
#include <linux/nvmap.h>
#include <linux/tegra-soc.h>

#include "../dev.h"

#include "gk20a.h"
#include "gr_ctx_gk20a.h"

#include "hw_ccsr_gk20a.h"
#include "hw_ctxsw_prog_gk20a.h"
#include "hw_fifo_gk20a.h"
#include "hw_gr_gk20a.h"
#include "hw_mc_gk20a.h"
#include "hw_ram_gk20a.h"
#include "hw_pri_ringmaster_gk20a.h"
#include "hw_pri_ringstation_sys_gk20a.h"
#include "hw_pri_ringstation_gpc_gk20a.h"
#include "hw_pri_ringstation_fbp_gk20a.h"
#include "hw_proj_gk20a.h"
#include "hw_top_gk20a.h"
#include "hw_ltc_gk20a.h"
#include "hw_fb_gk20a.h"
#include "hw_therm_gk20a.h"
#include "hw_pbdma_gk20a.h"
#include "chip_support.h"
#include "nvhost_memmgr.h"
#include "gk20a_gating_reglist.h"

static int gr_gk20a_commit_inst(struct channel_gk20a *c, u64 gpu_va);
static int gr_gk20a_ctx_patch_write(struct gk20a *g, struct channel_gk20a *c,
				    u32 addr, u32 data, u32 patch);

/* global ctx buffer */
static int  gr_gk20a_alloc_global_ctx_buffers(struct gk20a *g);
static void gr_gk20a_free_global_ctx_buffers(struct gk20a *g);
static int  gr_gk20a_map_global_ctx_buffers(struct gk20a *g,
					    struct channel_gk20a *c);
static void gr_gk20a_unmap_global_ctx_buffers(struct channel_gk20a *c);

/* channel gr ctx buffer */
static int  gr_gk20a_alloc_channel_gr_ctx(struct gk20a *g,
					struct channel_gk20a *c);
static void gr_gk20a_free_channel_gr_ctx(struct channel_gk20a *c);

/* channel patch ctx buffer */
static int  gr_gk20a_alloc_channel_patch_ctx(struct gk20a *g,
					struct channel_gk20a *c);
static void gr_gk20a_free_channel_patch_ctx(struct channel_gk20a *c);

/* golden ctx image */
static int gr_gk20a_init_golden_ctx_image(struct gk20a *g,
					  struct channel_gk20a *c);
static int gr_gk20a_load_golden_ctx_image(struct gk20a *g,
					  struct channel_gk20a *c);

static void gr_gk20a_load_falcon_dmem(struct gk20a *g)
{
	u32 i, ucode_u32_size;
	const u32 *ucode_u32_data;
	u32 checksum;

	nvhost_dbg_fn("");

	gk20a_writel(g, gr_gpccs_dmemc_r(0), (gr_gpccs_dmemc_offs_f(0) |
					      gr_gpccs_dmemc_blk_f(0)  |
					      gr_gpccs_dmemc_aincw_f(1)));

	ucode_u32_size = g->gr.ctx_vars.ucode.gpccs.data.count;
	ucode_u32_data = (const u32 *)g->gr.ctx_vars.ucode.gpccs.data.l;

	for (i = 0, checksum = 0; i < ucode_u32_size; i++) {
		gk20a_writel(g, gr_gpccs_dmemd_r(0), ucode_u32_data[i]);
		checksum += ucode_u32_data[i];
	}

	gk20a_writel(g, gr_fecs_dmemc_r(0), (gr_fecs_dmemc_offs_f(0) |
					     gr_fecs_dmemc_blk_f(0)  |
					     gr_fecs_dmemc_aincw_f(1)));

	ucode_u32_size = g->gr.ctx_vars.ucode.fecs.data.count;
	ucode_u32_data = (const u32 *)g->gr.ctx_vars.ucode.fecs.data.l;

	for (i = 0, checksum = 0; i < ucode_u32_size; i++) {
		gk20a_writel(g, gr_fecs_dmemd_r(0), ucode_u32_data[i]);
		checksum += ucode_u32_data[i];
	}
	nvhost_dbg_fn("done");
}

static void gr_gk20a_load_falcon_imem(struct gk20a *g)
{
	u32 cfg, fecs_imem_size, gpccs_imem_size, ucode_u32_size;
	const u32 *ucode_u32_data;
	u32 tag, i, pad_start, pad_end;
	u32 checksum;

	nvhost_dbg_fn("");

	cfg = gk20a_readl(g, gr_fecs_cfg_r());
	fecs_imem_size = gr_fecs_cfg_imem_sz_v(cfg);

	cfg = gk20a_readl(g, gr_gpc0_cfg_r());
	gpccs_imem_size = gr_gpc0_cfg_imem_sz_v(cfg);

	/* Use the broadcast address to access all of the GPCCS units. */
	gk20a_writel(g, gr_gpccs_imemc_r(0), (gr_gpccs_imemc_offs_f(0) |
					      gr_gpccs_imemc_blk_f(0) |
					      gr_gpccs_imemc_aincw_f(1)));

	/* Setup the tags for the instruction memory. */
	tag = 0;
	gk20a_writel(g, gr_gpccs_imemt_r(0), gr_gpccs_imemt_tag_f(tag));

	ucode_u32_size = g->gr.ctx_vars.ucode.gpccs.inst.count;
	ucode_u32_data = (const u32 *)g->gr.ctx_vars.ucode.gpccs.inst.l;

	for (i = 0, checksum = 0; i < ucode_u32_size; i++) {
		if (i && ((i % (256/sizeof(u32))) == 0)) {
			tag++;
			gk20a_writel(g, gr_gpccs_imemt_r(0),
				      gr_gpccs_imemt_tag_f(tag));
		}
		gk20a_writel(g, gr_gpccs_imemd_r(0), ucode_u32_data[i]);
		checksum += ucode_u32_data[i];
	}

	pad_start = i*4;
	pad_end = pad_start+(256-pad_start%256)+256;
	for (i = pad_start;
	     (i < gpccs_imem_size * 256) && (i < pad_end);
	     i += 4) {
		if (i && ((i % 256) == 0)) {
			tag++;
			gk20a_writel(g, gr_gpccs_imemt_r(0),
				      gr_gpccs_imemt_tag_f(tag));
		}
		gk20a_writel(g, gr_gpccs_imemd_r(0), 0);
	}

	gk20a_writel(g, gr_fecs_imemc_r(0), (gr_fecs_imemc_offs_f(0) |
					     gr_fecs_imemc_blk_f(0) |
					     gr_fecs_imemc_aincw_f(1)));

	/* Setup the tags for the instruction memory. */
	tag = 0;
	gk20a_writel(g, gr_fecs_imemt_r(0), gr_fecs_imemt_tag_f(tag));

	ucode_u32_size = g->gr.ctx_vars.ucode.fecs.inst.count;
	ucode_u32_data = (const u32 *)g->gr.ctx_vars.ucode.fecs.inst.l;

	for (i = 0, checksum = 0; i < ucode_u32_size; i++) {
		if (i && ((i % (256/sizeof(u32))) == 0)) {
			tag++;
			gk20a_writel(g, gr_fecs_imemt_r(0),
				      gr_fecs_imemt_tag_f(tag));
		}
		gk20a_writel(g, gr_fecs_imemd_r(0), ucode_u32_data[i]);
		checksum += ucode_u32_data[i];
	}

	pad_start = i*4;
	pad_end = pad_start+(256-pad_start%256)+256;
	for (i = pad_start; (i < fecs_imem_size * 256) && i < pad_end; i += 4) {
		if (i && ((i % 256) == 0)) {
			tag++;
			gk20a_writel(g, gr_fecs_imemt_r(0),
				      gr_fecs_imemt_tag_f(tag));
		}
		gk20a_writel(g, gr_fecs_imemd_r(0), 0);
	}
}

static int gr_gk20a_wait_idle(struct gk20a *g, unsigned long end_jiffies,
		u32 expect_delay)
{
	u32 delay = expect_delay;
	bool gr_enabled;
	bool ctxsw_active;
	bool gr_busy;

	nvhost_dbg_fn("");

	do {
		/* fmodel: host gets fifo_engine_status(gr) from gr
		   only when gr_status is read */
		gk20a_readl(g, gr_status_r());

		gr_enabled = gk20a_readl(g, mc_enable_r()) &
			mc_enable_pgraph_enabled_f();

		ctxsw_active = gk20a_readl(g,
			fifo_engine_status_r(ENGINE_GR_GK20A)) &
			fifo_engine_status_ctxsw_in_progress_f();

		gr_busy = gk20a_readl(g, gr_engine_status_r()) &
			gr_engine_status_value_busy_f();

		if (!gr_enabled || (!gr_busy && !ctxsw_active)) {
			nvhost_dbg_fn("done");
			return 0;
		}

		usleep_range(delay, delay * 2);
		delay = min_t(u32, delay << 1, GR_IDLE_CHECK_MAX);

	} while (time_before(jiffies, end_jiffies));

	nvhost_err(dev_from_gk20a(g),
		"timeout, ctxsw busy : %d, gr busy : %d",
		ctxsw_active, gr_busy);

	return -EAGAIN;
}

static int gr_gk20a_ctx_reset(struct gk20a *g, u32 rst_mask)
{
	u32 delay = GR_IDLE_CHECK_DEFAULT;
	unsigned long end_jiffies = jiffies +
		msecs_to_jiffies(gk20a_get_gr_idle_timeout(g));
	u32 reg;

	nvhost_dbg_fn("");

	/* Force clocks on */
	gk20a_writel(g, NV_PGRAPH_PRI_FE_PWR_MODE,
			NV_PGRAPH_PRI_FE_PWR_MODE_REQ_SEND |
			NV_PGRAPH_PRI_FE_PWR_MODE_MODE_FORCE_ON);

	/* Wait for the clocks to indicate that they are on */
	do {
		reg = gk20a_readl(g, NV_PGRAPH_PRI_FE_PWR_MODE);

		if ((reg & NV_PGRAPH_PRI_FE_PWR_MODE_REQ_MASK) ==
			NV_PGRAPH_PRI_FE_PWR_MODE_REQ_DONE)
			break;

		usleep_range(delay, delay * 2);
		delay = min_t(u32, delay << 1, GR_IDLE_CHECK_MAX);

	} while (time_before(jiffies, end_jiffies));

	if (!time_before(jiffies, end_jiffies)) {
		nvhost_err(dev_from_gk20a(g),
			   "failed to force the clocks on\n");
		WARN_ON(1);
	}

	if (rst_mask) {
		gk20a_writel(g, gr_fecs_ctxsw_reset_ctl_r(), rst_mask);
	} else {
		gk20a_writel(g, gr_fecs_ctxsw_reset_ctl_r(),
			     gr_fecs_ctxsw_reset_ctl_sys_halt_disabled_f() |
			     gr_fecs_ctxsw_reset_ctl_gpc_halt_disabled_f() |
			     gr_fecs_ctxsw_reset_ctl_be_halt_disabled_f()  |
			     gr_fecs_ctxsw_reset_ctl_sys_engine_reset_disabled_f() |
			     gr_fecs_ctxsw_reset_ctl_gpc_engine_reset_disabled_f() |
			     gr_fecs_ctxsw_reset_ctl_be_engine_reset_disabled_f()  |
			     gr_fecs_ctxsw_reset_ctl_sys_context_reset_enabled_f() |
			     gr_fecs_ctxsw_reset_ctl_gpc_context_reset_enabled_f() |
			     gr_fecs_ctxsw_reset_ctl_be_context_reset_enabled_f());
	}

	/* Delay for > 10 nvclks after writing reset. */
	gk20a_readl(g, gr_fecs_ctxsw_reset_ctl_r());

	gk20a_writel(g, gr_fecs_ctxsw_reset_ctl_r(),
		     gr_fecs_ctxsw_reset_ctl_sys_halt_disabled_f() |
		     gr_fecs_ctxsw_reset_ctl_gpc_halt_disabled_f() |
		     gr_fecs_ctxsw_reset_ctl_be_halt_disabled_f()  |
		     gr_fecs_ctxsw_reset_ctl_sys_engine_reset_disabled_f() |
		     gr_fecs_ctxsw_reset_ctl_gpc_engine_reset_disabled_f() |
		     gr_fecs_ctxsw_reset_ctl_be_engine_reset_disabled_f()  |
		     gr_fecs_ctxsw_reset_ctl_sys_context_reset_disabled_f() |
		     gr_fecs_ctxsw_reset_ctl_gpc_context_reset_disabled_f() |
		     gr_fecs_ctxsw_reset_ctl_be_context_reset_disabled_f());

	/* Delay for > 10 nvclks after writing reset. */
	gk20a_readl(g, gr_fecs_ctxsw_reset_ctl_r());

	end_jiffies = jiffies + msecs_to_jiffies(gk20a_get_gr_idle_timeout(g));

	/* Set power mode back to auto */
	gk20a_writel(g, NV_PGRAPH_PRI_FE_PWR_MODE,
			NV_PGRAPH_PRI_FE_PWR_MODE_REQ_SEND |
			NV_PGRAPH_PRI_FE_PWR_MODE_MODE_AUTO);

	/* Wait for the request to complete */
	do {
		reg = gk20a_readl(g, NV_PGRAPH_PRI_FE_PWR_MODE);

		if ((reg & NV_PGRAPH_PRI_FE_PWR_MODE_REQ_MASK) ==
			NV_PGRAPH_PRI_FE_PWR_MODE_REQ_DONE)
			break;

		usleep_range(delay, delay * 2);
		delay = min_t(u32, delay << 1, GR_IDLE_CHECK_MAX);

	} while (time_before(jiffies, end_jiffies));

	if (!time_before(jiffies, end_jiffies)) {
		nvhost_err(dev_from_gk20a(g),
			   "failed to set power mode to auto\n");
		WARN_ON(1);
	}

	return 0;
}

static int gr_gk20a_ctx_wait_ucode(struct gk20a *g, u32 mailbox_id,
				   u32 *mailbox_ret, u32 opc_success,
				   u32 mailbox_ok, u32 opc_fail,
				   u32 mailbox_fail)
{
	unsigned long end_jiffies = jiffies +
		msecs_to_jiffies(gk20a_get_gr_idle_timeout(g));
	u32 delay = GR_IDLE_CHECK_DEFAULT;
	u32 check = WAIT_UCODE_LOOP;
	u32 reg;

	nvhost_dbg_fn("");

	while (check == WAIT_UCODE_LOOP) {
		if (!time_before(jiffies, end_jiffies) &&
				tegra_platform_is_silicon())
			check = WAIT_UCODE_TIMEOUT;

		reg = gk20a_readl(g, gr_fecs_ctxsw_mailbox_r(mailbox_id));

		if (mailbox_ret)
			*mailbox_ret = reg;

		switch (opc_success) {
		case GR_IS_UCODE_OP_EQUAL:
			if (reg == mailbox_ok)
				check = WAIT_UCODE_OK;
			break;
		case GR_IS_UCODE_OP_NOT_EQUAL:
			if (reg != mailbox_ok)
				check = WAIT_UCODE_OK;
			break;
		case GR_IS_UCODE_OP_AND:
			if (reg & mailbox_ok)
				check = WAIT_UCODE_OK;
			break;
		case GR_IS_UCODE_OP_LESSER:
			if (reg < mailbox_ok)
				check = WAIT_UCODE_OK;
			break;
		case GR_IS_UCODE_OP_LESSER_EQUAL:
			if (reg <= mailbox_ok)
				check = WAIT_UCODE_OK;
			break;
		case GR_IS_UCODE_OP_SKIP:
			/* do no success check */
			break;
		default:
			nvhost_err(dev_from_gk20a(g),
				   "invalid success opcode 0x%x", opc_success);

			check = WAIT_UCODE_ERROR;
			break;
		}

		switch (opc_fail) {
		case GR_IS_UCODE_OP_EQUAL:
			if (reg == mailbox_fail)
				check = WAIT_UCODE_ERROR;
			break;
		case GR_IS_UCODE_OP_NOT_EQUAL:
			if (reg != mailbox_fail)
				check = WAIT_UCODE_ERROR;
			break;
		case GR_IS_UCODE_OP_AND:
			if (reg & mailbox_fail)
				check = WAIT_UCODE_ERROR;
			break;
		case GR_IS_UCODE_OP_LESSER:
			if (reg < mailbox_fail)
				check = WAIT_UCODE_ERROR;
			break;
		case GR_IS_UCODE_OP_LESSER_EQUAL:
			if (reg <= mailbox_fail)
				check = WAIT_UCODE_ERROR;
			break;
		case GR_IS_UCODE_OP_SKIP:
			/* do no check on fail*/
			break;
		default:
			nvhost_err(dev_from_gk20a(g),
				   "invalid fail opcode 0x%x", opc_fail);
			check = WAIT_UCODE_ERROR;
			break;
		}

		usleep_range(delay, delay * 2);
		delay = min_t(u32, delay << 1, GR_IDLE_CHECK_MAX);
	}

	if (check == WAIT_UCODE_TIMEOUT) {
		nvhost_err(dev_from_gk20a(g),
			   "timeout waiting on ucode response");
		return -1;
	} else if (check == WAIT_UCODE_ERROR) {
		nvhost_err(dev_from_gk20a(g),
			   "ucode method failed on mailbox=%d value=0x%08x",
			   mailbox_id, reg);
		return -1;
	}

	nvhost_dbg_fn("done");
	return 0;
}

int gr_gk20a_submit_fecs_method(struct gk20a *g,
			u32 mb_id, u32 mb_data, u32 mb_clr,
			u32 mtd_data, u32 mtd_adr, u32 *mb_ret,
			u32 opc_ok, u32 mb_ok, u32 opc_fail, u32 mb_fail)
{
	struct gr_gk20a *gr = &g->gr;
	int ret;

	mutex_lock(&gr->fecs_mutex);

	if (mb_id != 0)
		gk20a_writel(g, gr_fecs_ctxsw_mailbox_r(mb_id),
			mb_data);

	gk20a_writel(g, gr_fecs_ctxsw_mailbox_clear_r(0),
		gr_fecs_ctxsw_mailbox_clear_value_f(mb_clr));

	gk20a_writel(g, gr_fecs_method_data_r(), mtd_data);
	gk20a_writel(g, gr_fecs_method_push_r(),
		gr_fecs_method_push_adr_f(mtd_adr));

	ret = gr_gk20a_ctx_wait_ucode(g, 0, mb_ret,
		opc_ok, mb_ok, opc_fail, mb_fail);

	mutex_unlock(&gr->fecs_mutex);

	return ret;
}

static int gr_gk20a_commit_inst(struct channel_gk20a *c, u64 gpu_va)
{
	u32 addr_lo;
	u32 addr_hi;
	u32 ret = 0;
	void *inst_ptr = NULL;

	nvhost_dbg_fn("");

	/* flush gpu_va before commit */
	gk20a_mm_fb_flush(c->g);
	gk20a_mm_l2_flush(c->g, true);

	inst_ptr = nvhost_memmgr_mmap(c->inst_block.mem.ref);
	if (!inst_ptr) {
		ret = -ENOMEM;
		goto clean_up;
	}

	addr_lo = u64_lo32(gpu_va) >> 12;
	addr_hi = u64_hi32(gpu_va);

	mem_wr32(inst_ptr, ram_in_gr_wfi_target_w(),
		 ram_in_gr_cs_wfi_f() | ram_in_gr_wfi_mode_virtual_f() |
		 ram_in_gr_wfi_ptr_lo_f(addr_lo));

	mem_wr32(inst_ptr, ram_in_gr_wfi_ptr_hi_w(),
		 ram_in_gr_wfi_ptr_hi_f(addr_hi));

	nvhost_memmgr_munmap(c->inst_block.mem.ref, inst_ptr);

	gk20a_mm_l2_invalidate(c->g);

	return 0;

clean_up:
	if (inst_ptr)
		nvhost_memmgr_munmap(c->inst_block.mem.ref, inst_ptr);

	return ret;
}

static int gr_gk20a_ctx_patch_write(struct gk20a *g, struct channel_gk20a *c,
				    u32 addr, u32 data, u32 patch)
{
	struct channel_ctx_gk20a *ch_ctx;
	u32 patch_slot = 0;
	void *patch_ptr = NULL;

	BUG_ON(patch != 0 && c == NULL);

	if (patch) {
		ch_ctx = &c->ch_ctx;
		patch_ptr = nvhost_memmgr_mmap(ch_ctx->patch_ctx.mem.ref);
		if (!patch_ptr)
			return -ENOMEM;

		patch_slot = ch_ctx->patch_ctx.data_count * 2;

		mem_wr32(patch_ptr, patch_slot++, addr);
		mem_wr32(patch_ptr, patch_slot++, data);

		nvhost_memmgr_munmap(ch_ctx->patch_ctx.mem.ref, patch_ptr);
		gk20a_mm_l2_invalidate(g);

		ch_ctx->patch_ctx.data_count++;
	} else {
		gk20a_writel(g, addr, data);
	}

	return 0;
}

static int gr_gk20a_fecs_ctx_bind_channel(struct gk20a *g,
					struct channel_gk20a *c)
{
	u32 inst_base_ptr = u64_lo32(sg_phys(c->inst_block.mem.sgt->sgl)
				     >> ram_in_base_shift_v());
	u32 ret;

	nvhost_dbg_info("bind channel %d inst ptr 0x%08x",
		   c->hw_chid, inst_base_ptr);

	ret = gr_gk20a_submit_fecs_method(g, 0, 0, 0x30,
			gr_fecs_current_ctx_ptr_f(inst_base_ptr) |
			gr_fecs_current_ctx_target_vid_mem_f() |
			gr_fecs_current_ctx_valid_f(1),
			gr_fecs_method_push_adr_bind_pointer_f(),
			0, GR_IS_UCODE_OP_AND, 0x10, GR_IS_UCODE_OP_AND, 0x20);
	if (ret)
		nvhost_err(dev_from_gk20a(g),
			"bind channel instance failed");

	return ret;
}

static int gr_gk20a_ctx_zcull_setup(struct gk20a *g, struct channel_gk20a *c,
				    bool disable_fifo)
{
	struct channel_ctx_gk20a *ch_ctx = &c->ch_ctx;
	struct fifo_gk20a *f = &g->fifo;
	struct fifo_engine_info_gk20a *gr_info = f->engine_info + ENGINE_GR_GK20A;
	u32 va_lo, va_hi, va;
	int ret = 0;
	void *ctx_ptr = NULL;

	nvhost_dbg_fn("");

	ctx_ptr = nvhost_memmgr_mmap(ch_ctx->gr_ctx.mem.ref);
	if (!ctx_ptr)
		return -ENOMEM;

	if (ch_ctx->zcull_ctx.gpu_va == 0 &&
	    ch_ctx->zcull_ctx.ctx_sw_mode ==
		ctxsw_prog_main_image_zcull_mode_separate_buffer_v()) {
		ret = -EINVAL;
		goto clean_up;
	}

	va_lo = u64_lo32(ch_ctx->zcull_ctx.gpu_va);
	va_hi = u64_hi32(ch_ctx->zcull_ctx.gpu_va);
	va = ((va_lo >> 8) & 0x00FFFFFF) | ((va_hi << 24) & 0xFF000000);

	if (disable_fifo) {
		ret = gk20a_fifo_disable_engine_activity(g, gr_info, true);
		if (ret) {
			nvhost_err(dev_from_gk20a(g),
				"failed to disable gr engine activity\n");
			goto clean_up;
		}
	}

	/* Channel gr_ctx buffer is gpu cacheable.
	   Flush and invalidate before cpu update. */
	gk20a_mm_fb_flush(g);
	gk20a_mm_l2_flush(g, true);

	mem_wr32(ctx_ptr + ctxsw_prog_main_image_zcull_v(), 0,
		 ch_ctx->zcull_ctx.ctx_sw_mode);

	mem_wr32(ctx_ptr + ctxsw_prog_main_image_zcull_ptr_v(), 0, va);

	if (disable_fifo) {
		ret = gk20a_fifo_enable_engine_activity(g, gr_info);
		if (ret) {
			nvhost_err(dev_from_gk20a(g),
				"failed to enable gr engine activity\n");
			goto clean_up;
		}
	}
	gk20a_mm_l2_invalidate(g);

clean_up:
	nvhost_memmgr_munmap(ch_ctx->gr_ctx.mem.ref, ctx_ptr);

	return ret;
}

static int gr_gk20a_commit_global_cb_manager(struct gk20a *g,
			struct channel_gk20a *c, u32 patch)
{
	struct gr_gk20a *gr = &g->gr;
	u32 attrib_offset_in_chunk = 0;
	u32 alpha_offset_in_chunk = 0;
	u32 pd_ab_max_output;
	u32 gpc_index, ppc_index;
	u32 temp;
	u32 cbm_cfg_size1, cbm_cfg_size2;

	nvhost_dbg_fn("");

	gr_gk20a_ctx_patch_write(g, c, gr_ds_tga_constraintlogic_r(),
		gr_ds_tga_constraintlogic_beta_cbsize_f(gr->attrib_cb_default_size) |
		gr_ds_tga_constraintlogic_alpha_cbsize_f(gr->alpha_cb_default_size),
		patch);

	pd_ab_max_output = (gr->alpha_cb_default_size *
		gr_gpc0_ppc0_cbm_cfg_size_granularity_v()) /
		gr_pd_ab_dist_cfg1_max_output_granularity_v();

	gr_gk20a_ctx_patch_write(g, c, gr_pd_ab_dist_cfg1_r(),
		gr_pd_ab_dist_cfg1_max_output_f(pd_ab_max_output) |
		gr_pd_ab_dist_cfg1_max_batches_init_f(), patch);

	alpha_offset_in_chunk = attrib_offset_in_chunk +
		gr->tpc_count * gr->attrib_cb_size;

	for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++) {
		temp = proj_gpc_stride_v() * gpc_index;
		for (ppc_index = 0; ppc_index < gr->gpc_ppc_count[gpc_index];
		     ppc_index++) {
			cbm_cfg_size1 = gr->attrib_cb_default_size *
				gr->pes_tpc_count[ppc_index][gpc_index];
			cbm_cfg_size2 = gr->alpha_cb_default_size *
				gr->pes_tpc_count[ppc_index][gpc_index];

			gr_gk20a_ctx_patch_write(g, c,
				gr_gpc0_ppc0_cbm_cfg_r() + temp +
				proj_ppc_in_gpc_stride_v() * ppc_index,
				gr_gpc0_ppc0_cbm_cfg_timeslice_mode_f(gr->timeslice_mode) |
				gr_gpc0_ppc0_cbm_cfg_start_offset_f(attrib_offset_in_chunk) |
				gr_gpc0_ppc0_cbm_cfg_size_f(cbm_cfg_size1), patch);

			attrib_offset_in_chunk += gr->attrib_cb_size *
				gr->pes_tpc_count[ppc_index][gpc_index];

			gr_gk20a_ctx_patch_write(g, c,
				gr_gpc0_ppc0_cbm_cfg2_r() + temp +
				proj_ppc_in_gpc_stride_v() * ppc_index,
				gr_gpc0_ppc0_cbm_cfg2_start_offset_f(alpha_offset_in_chunk) |
				gr_gpc0_ppc0_cbm_cfg2_size_f(cbm_cfg_size2), patch);

			alpha_offset_in_chunk += gr->alpha_cb_size *
				gr->pes_tpc_count[ppc_index][gpc_index];
		}
	}

	return 0;
}

static int gr_gk20a_commit_global_ctx_buffers(struct gk20a *g,
			struct channel_gk20a *c, u32 patch)
{
	struct gr_gk20a *gr = &g->gr;
	struct channel_ctx_gk20a *ch_ctx = &c->ch_ctx;
	u64 addr;
	u32 size;
	u32 data;

	nvhost_dbg_fn("");

	/* global pagepool buffer */
	addr = (u64_lo32(ch_ctx->global_ctx_buffer_va[PAGEPOOL_VA]) >>
		gr_scc_pagepool_base_addr_39_8_align_bits_v()) |
		(u64_hi32(ch_ctx->global_ctx_buffer_va[PAGEPOOL_VA]) <<
		 (32 - gr_scc_pagepool_base_addr_39_8_align_bits_v()));

	size = gr->global_ctx_buffer[PAGEPOOL].size /
		gr_scc_pagepool_total_pages_byte_granularity_v();

	if (size == gr_scc_pagepool_total_pages_hwmax_value_v())
		size = gr_scc_pagepool_total_pages_hwmax_v();

	nvhost_dbg_info("pagepool buffer addr : 0x%016llx, size : %d",
		addr, size);

	gr_gk20a_ctx_patch_write(g, c, gr_scc_pagepool_base_r(),
		gr_scc_pagepool_base_addr_39_8_f(addr), patch);

	gr_gk20a_ctx_patch_write(g, c, gr_scc_pagepool_r(),
		gr_scc_pagepool_total_pages_f(size) |
		gr_scc_pagepool_valid_true_f(), patch);

	gr_gk20a_ctx_patch_write(g, c, gr_gpcs_gcc_pagepool_base_r(),
		gr_gpcs_gcc_pagepool_base_addr_39_8_f(addr), patch);

	gr_gk20a_ctx_patch_write(g, c, gr_gpcs_gcc_pagepool_r(),
		gr_gpcs_gcc_pagepool_total_pages_f(size), patch);

	gr_gk20a_ctx_patch_write(g, c, gr_pd_pagepool_r(),
		gr_pd_pagepool_total_pages_f(size) |
		gr_pd_pagepool_valid_true_f(), patch);

	/* global bundle cb */
	addr = (u64_lo32(ch_ctx->global_ctx_buffer_va[CIRCULAR_VA]) >>
		gr_scc_bundle_cb_base_addr_39_8_align_bits_v()) |
		(u64_hi32(ch_ctx->global_ctx_buffer_va[CIRCULAR_VA]) <<
		 (32 - gr_scc_bundle_cb_base_addr_39_8_align_bits_v()));

	size = gr->bundle_cb_default_size;

	nvhost_dbg_info("bundle cb addr : 0x%016llx, size : %d",
		addr, size);

	gr_gk20a_ctx_patch_write(g, c, gr_scc_bundle_cb_base_r(),
		gr_scc_bundle_cb_base_addr_39_8_f(addr), patch);

	gr_gk20a_ctx_patch_write(g, c, gr_scc_bundle_cb_size_r(),
		gr_scc_bundle_cb_size_div_256b_f(size) |
		gr_scc_bundle_cb_size_valid_true_f(), patch);

	gr_gk20a_ctx_patch_write(g, c, gr_gpcs_setup_bundle_cb_base_r(),
		gr_gpcs_setup_bundle_cb_base_addr_39_8_f(addr), patch);

	gr_gk20a_ctx_patch_write(g, c, gr_gpcs_setup_bundle_cb_size_r(),
		gr_gpcs_setup_bundle_cb_size_div_256b_f(size) |
		gr_gpcs_setup_bundle_cb_size_valid_true_f(), patch);

	/* data for state_limit */
	data = (gr->bundle_cb_default_size *
		gr_scc_bundle_cb_size_div_256b_byte_granularity_v()) /
		gr_pd_ab_dist_cfg2_state_limit_scc_bundle_granularity_v();

	data = min_t(u32, data, gr->min_gpm_fifo_depth);

	nvhost_dbg_info("bundle cb token limit : %d, state limit : %d",
		   gr->bundle_cb_token_limit, data);

	gr_gk20a_ctx_patch_write(g, c, gr_pd_ab_dist_cfg2_r(),
		gr_pd_ab_dist_cfg2_token_limit_f(gr->bundle_cb_token_limit) |
		gr_pd_ab_dist_cfg2_state_limit_f(data), patch);

	/* global attrib cb */
	addr = (u64_lo32(ch_ctx->global_ctx_buffer_va[ATTRIBUTE_VA]) >>
		gr_gpcs_setup_attrib_cb_base_addr_39_12_align_bits_v()) |
		(u64_hi32(ch_ctx->global_ctx_buffer_va[ATTRIBUTE_VA]) <<
		 (32 - gr_gpcs_setup_attrib_cb_base_addr_39_12_align_bits_v()));

	nvhost_dbg_info("attrib cb addr : 0x%016llx", addr);

	gr_gk20a_ctx_patch_write(g, c, gr_gpcs_setup_attrib_cb_base_r(),
		gr_gpcs_setup_attrib_cb_base_addr_39_12_f(addr) |
		gr_gpcs_setup_attrib_cb_base_valid_true_f(), patch);

	gr_gk20a_ctx_patch_write(g, c, gr_gpcs_tpcs_pe_pin_cb_global_base_addr_r(),
		gr_gpcs_tpcs_pe_pin_cb_global_base_addr_v_f(addr) |
		gr_gpcs_tpcs_pe_pin_cb_global_base_addr_valid_true_f(), patch);

	return 0;
}

static int gr_gk20a_commit_global_timeslice(struct gk20a *g, struct channel_gk20a *c, u32 patch)
{
	struct gr_gk20a *gr = &g->gr;
	u32 gpm_pd_cfg;
	u32 pd_ab_dist_cfg0;
	u32 ds_debug;
	u32 mpc_vtg_debug;
	u32 pe_vaf;
	u32 pe_vsc_vpc;

	nvhost_dbg_fn("");

	gpm_pd_cfg = gk20a_readl(g, gr_gpcs_gpm_pd_cfg_r());
	pd_ab_dist_cfg0 = gk20a_readl(g, gr_pd_ab_dist_cfg0_r());
	ds_debug = gk20a_readl(g, gr_ds_debug_r());
	mpc_vtg_debug = gk20a_readl(g, gr_gpcs_tpcs_mpc_vtg_debug_r());

	if (gr->timeslice_mode == gr_gpcs_ppcs_cbm_cfg_timeslice_mode_enable_v()) {
		pe_vaf = gk20a_readl(g, gr_gpcs_tpcs_pe_vaf_r());
		pe_vsc_vpc = gk20a_readl(g, gr_gpcs_tpcs_pes_vsc_vpc_r());

		gpm_pd_cfg = gr_gpcs_gpm_pd_cfg_timeslice_mode_enable_f() | gpm_pd_cfg;
		pe_vaf = gr_gpcs_tpcs_pe_vaf_fast_mode_switch_true_f() | pe_vaf;
		pe_vsc_vpc = gr_gpcs_tpcs_pes_vsc_vpc_fast_mode_switch_true_f() | pe_vsc_vpc;
		pd_ab_dist_cfg0 = gr_pd_ab_dist_cfg0_timeslice_enable_en_f() | pd_ab_dist_cfg0;
		ds_debug = gr_ds_debug_timeslice_mode_enable_f() | ds_debug;
		mpc_vtg_debug = gr_gpcs_tpcs_mpc_vtg_debug_timeslice_mode_enabled_f() | mpc_vtg_debug;

		gr_gk20a_ctx_patch_write(g, c, gr_gpcs_gpm_pd_cfg_r(), gpm_pd_cfg, patch);
		gr_gk20a_ctx_patch_write(g, c, gr_gpcs_tpcs_pe_vaf_r(), pe_vaf, patch);
		gr_gk20a_ctx_patch_write(g, c, gr_gpcs_tpcs_pes_vsc_vpc_r(), pe_vsc_vpc, patch);
		gr_gk20a_ctx_patch_write(g, c, gr_pd_ab_dist_cfg0_r(), pd_ab_dist_cfg0, patch);
		gr_gk20a_ctx_patch_write(g, c, gr_ds_debug_r(), ds_debug, patch);
		gr_gk20a_ctx_patch_write(g, c, gr_gpcs_tpcs_mpc_vtg_debug_r(), mpc_vtg_debug, patch);
	} else {
		gpm_pd_cfg = gr_gpcs_gpm_pd_cfg_timeslice_mode_disable_f() | gpm_pd_cfg;
		pd_ab_dist_cfg0 = gr_pd_ab_dist_cfg0_timeslice_enable_dis_f() | pd_ab_dist_cfg0;
		ds_debug = gr_ds_debug_timeslice_mode_disable_f() | ds_debug;
		mpc_vtg_debug = gr_gpcs_tpcs_mpc_vtg_debug_timeslice_mode_disabled_f() | mpc_vtg_debug;

		gr_gk20a_ctx_patch_write(g, c, gr_gpcs_gpm_pd_cfg_r(), gpm_pd_cfg, patch);
		gr_gk20a_ctx_patch_write(g, c, gr_pd_ab_dist_cfg0_r(), pd_ab_dist_cfg0, patch);
		gr_gk20a_ctx_patch_write(g, c, gr_ds_debug_r(), ds_debug, patch);
		gr_gk20a_ctx_patch_write(g, c, gr_gpcs_tpcs_mpc_vtg_debug_r(), mpc_vtg_debug, patch);
	}

	return 0;
}

static int gr_gk20a_setup_rop_mapping(struct gk20a *g,
				struct gr_gk20a *gr)
{
	u32 norm_entries, norm_shift;
	u32 coeff5_mod, coeff6_mod, coeff7_mod, coeff8_mod, coeff9_mod, coeff10_mod, coeff11_mod;
	u32 map0, map1, map2, map3, map4, map5;

	if (!gr->map_tiles)
		return -1;

	nvhost_dbg_fn("");

	gk20a_writel(g, gr_crstr_map_table_cfg_r(),
		     gr_crstr_map_table_cfg_row_offset_f(gr->map_row_offset) |
		     gr_crstr_map_table_cfg_num_entries_f(gr->tpc_count));

	map0 =  gr_crstr_gpc_map0_tile0_f(gr->map_tiles[0]) |
		gr_crstr_gpc_map0_tile1_f(gr->map_tiles[1]) |
		gr_crstr_gpc_map0_tile2_f(gr->map_tiles[2]) |
		gr_crstr_gpc_map0_tile3_f(gr->map_tiles[3]) |
		gr_crstr_gpc_map0_tile4_f(gr->map_tiles[4]) |
		gr_crstr_gpc_map0_tile5_f(gr->map_tiles[5]);

	map1 =  gr_crstr_gpc_map1_tile6_f(gr->map_tiles[6]) |
		gr_crstr_gpc_map1_tile7_f(gr->map_tiles[7]) |
		gr_crstr_gpc_map1_tile8_f(gr->map_tiles[8]) |
		gr_crstr_gpc_map1_tile9_f(gr->map_tiles[9]) |
		gr_crstr_gpc_map1_tile10_f(gr->map_tiles[10]) |
		gr_crstr_gpc_map1_tile11_f(gr->map_tiles[11]);

	map2 =  gr_crstr_gpc_map2_tile12_f(gr->map_tiles[12]) |
		gr_crstr_gpc_map2_tile13_f(gr->map_tiles[13]) |
		gr_crstr_gpc_map2_tile14_f(gr->map_tiles[14]) |
		gr_crstr_gpc_map2_tile15_f(gr->map_tiles[15]) |
		gr_crstr_gpc_map2_tile16_f(gr->map_tiles[16]) |
		gr_crstr_gpc_map2_tile17_f(gr->map_tiles[17]);

	map3 =  gr_crstr_gpc_map3_tile18_f(gr->map_tiles[18]) |
		gr_crstr_gpc_map3_tile19_f(gr->map_tiles[19]) |
		gr_crstr_gpc_map3_tile20_f(gr->map_tiles[20]) |
		gr_crstr_gpc_map3_tile21_f(gr->map_tiles[21]) |
		gr_crstr_gpc_map3_tile22_f(gr->map_tiles[22]) |
		gr_crstr_gpc_map3_tile23_f(gr->map_tiles[23]);

	map4 =  gr_crstr_gpc_map4_tile24_f(gr->map_tiles[24]) |
		gr_crstr_gpc_map4_tile25_f(gr->map_tiles[25]) |
		gr_crstr_gpc_map4_tile26_f(gr->map_tiles[26]) |
		gr_crstr_gpc_map4_tile27_f(gr->map_tiles[27]) |
		gr_crstr_gpc_map4_tile28_f(gr->map_tiles[28]) |
		gr_crstr_gpc_map4_tile29_f(gr->map_tiles[29]);

	map5 =  gr_crstr_gpc_map5_tile30_f(gr->map_tiles[30]) |
		gr_crstr_gpc_map5_tile31_f(gr->map_tiles[31]) |
		gr_crstr_gpc_map5_tile32_f(0) |
		gr_crstr_gpc_map5_tile33_f(0) |
		gr_crstr_gpc_map5_tile34_f(0) |
		gr_crstr_gpc_map5_tile35_f(0);

	gk20a_writel(g, gr_crstr_gpc_map0_r(), map0);
	gk20a_writel(g, gr_crstr_gpc_map1_r(), map1);
	gk20a_writel(g, gr_crstr_gpc_map2_r(), map2);
	gk20a_writel(g, gr_crstr_gpc_map3_r(), map3);
	gk20a_writel(g, gr_crstr_gpc_map4_r(), map4);
	gk20a_writel(g, gr_crstr_gpc_map5_r(), map5);

	switch (gr->tpc_count) {
	case 1:
		norm_shift = 4;
		break;
	case 2:
	case 3:
		norm_shift = 3;
		break;
	case 4:
	case 5:
	case 6:
	case 7:
		norm_shift = 2;
		break;
	case 8:
	case 9:
	case 10:
	case 11:
	case 12:
	case 13:
	case 14:
	case 15:
		norm_shift = 1;
		break;
	default:
		norm_shift = 0;
		break;
	}

	norm_entries = gr->tpc_count << norm_shift;
	coeff5_mod = (1 << 5) % norm_entries;
	coeff6_mod = (1 << 6) % norm_entries;
	coeff7_mod = (1 << 7) % norm_entries;
	coeff8_mod = (1 << 8) % norm_entries;
	coeff9_mod = (1 << 9) % norm_entries;
	coeff10_mod = (1 << 10) % norm_entries;
	coeff11_mod = (1 << 11) % norm_entries;

	gk20a_writel(g, gr_ppcs_wwdx_map_table_cfg_r(),
		     gr_ppcs_wwdx_map_table_cfg_row_offset_f(gr->map_row_offset) |
		     gr_ppcs_wwdx_map_table_cfg_normalized_num_entries_f(norm_entries) |
		     gr_ppcs_wwdx_map_table_cfg_normalized_shift_value_f(norm_shift) |
		     gr_ppcs_wwdx_map_table_cfg_coeff5_mod_value_f(coeff5_mod) |
		     gr_ppcs_wwdx_map_table_cfg_num_entries_f(gr->tpc_count));

	gk20a_writel(g, gr_ppcs_wwdx_map_table_cfg2_r(),
		     gr_ppcs_wwdx_map_table_cfg2_coeff6_mod_value_f(coeff6_mod) |
		     gr_ppcs_wwdx_map_table_cfg2_coeff7_mod_value_f(coeff7_mod) |
		     gr_ppcs_wwdx_map_table_cfg2_coeff8_mod_value_f(coeff8_mod) |
		     gr_ppcs_wwdx_map_table_cfg2_coeff9_mod_value_f(coeff9_mod) |
		     gr_ppcs_wwdx_map_table_cfg2_coeff10_mod_value_f(coeff10_mod) |
		     gr_ppcs_wwdx_map_table_cfg2_coeff11_mod_value_f(coeff11_mod));

	gk20a_writel(g, gr_ppcs_wwdx_map_gpc_map0_r(), map0);
	gk20a_writel(g, gr_ppcs_wwdx_map_gpc_map1_r(), map1);
	gk20a_writel(g, gr_ppcs_wwdx_map_gpc_map2_r(), map2);
	gk20a_writel(g, gr_ppcs_wwdx_map_gpc_map3_r(), map3);
	gk20a_writel(g, gr_ppcs_wwdx_map_gpc_map4_r(), map4);
	gk20a_writel(g, gr_ppcs_wwdx_map_gpc_map5_r(), map5);

	gk20a_writel(g, gr_rstr2d_map_table_cfg_r(),
		     gr_rstr2d_map_table_cfg_row_offset_f(gr->map_row_offset) |
		     gr_rstr2d_map_table_cfg_num_entries_f(gr->tpc_count));

	gk20a_writel(g, gr_rstr2d_gpc_map0_r(), map0);
	gk20a_writel(g, gr_rstr2d_gpc_map1_r(), map1);
	gk20a_writel(g, gr_rstr2d_gpc_map2_r(), map2);
	gk20a_writel(g, gr_rstr2d_gpc_map3_r(), map3);
	gk20a_writel(g, gr_rstr2d_gpc_map4_r(), map4);
	gk20a_writel(g, gr_rstr2d_gpc_map5_r(), map5);

	return 0;
}

static inline u32 count_bits(u32 mask)
{
	u32 temp = mask;
	u32 count;
	for (count = 0; temp != 0; count++)
		temp &= temp - 1;

	return count;
}

static inline u32 clear_count_bits(u32 num, u32 clear_count)
{
	u32 count = clear_count;
	for (; (num != 0) && (count != 0); count--)
		num &= num - 1;

	return num;
}

static int gr_gk20a_setup_alpha_beta_tables(struct gk20a *g,
					struct gr_gk20a *gr)
{
	u32 table_index_bits = 5;
	u32 rows = (1 << table_index_bits);
	u32 row_stride = gr_pd_alpha_ratio_table__size_1_v() / rows;

	u32 row;
	u32 index;
	u32 gpc_index;
	u32 gpcs_per_reg = 4;
	u32 pes_index;
	u32 tpc_count_pes;
	u32 num_pes_per_gpc = proj_scal_litter_num_pes_per_gpc_v();

	u32 alpha_target, beta_target;
	u32 alpha_bits, beta_bits;
	u32 alpha_mask, beta_mask, partial_mask;
	u32 reg_offset;
	bool assign_alpha;

	u32 map_alpha[gr_pd_alpha_ratio_table__size_1_v()];
	u32 map_beta[gr_pd_alpha_ratio_table__size_1_v()];
	u32 map_reg_used[gr_pd_alpha_ratio_table__size_1_v()];

	nvhost_dbg_fn("");

	memset(map_alpha, 0, gr_pd_alpha_ratio_table__size_1_v() * sizeof(u32));
	memset(map_beta, 0, gr_pd_alpha_ratio_table__size_1_v() * sizeof(u32));
	memset(map_reg_used, 0, gr_pd_alpha_ratio_table__size_1_v() * sizeof(u32));

	for (row = 0; row < rows; ++row) {
		alpha_target = max_t(u32, gr->tpc_count * row / rows, 1);
		beta_target = gr->tpc_count - alpha_target;

		assign_alpha = (alpha_target < beta_target);

		for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++) {
			reg_offset = (row * row_stride) + (gpc_index / gpcs_per_reg);
			alpha_mask = beta_mask = 0;

			for (pes_index = 0; pes_index < num_pes_per_gpc; pes_index++) {
				tpc_count_pes = gr->pes_tpc_count[pes_index][gpc_index];

				if (assign_alpha) {
					alpha_bits = (alpha_target == 0) ? 0 : tpc_count_pes;
					beta_bits = tpc_count_pes - alpha_bits;
				} else {
					beta_bits = (beta_target == 0) ? 0 : tpc_count_pes;
					alpha_bits = tpc_count_pes - beta_bits;
				}

				partial_mask = gr->pes_tpc_mask[pes_index][gpc_index];
				partial_mask = clear_count_bits(partial_mask, tpc_count_pes - alpha_bits);
				alpha_mask |= partial_mask;

				partial_mask = gr->pes_tpc_mask[pes_index][gpc_index] ^ partial_mask;
				beta_mask |= partial_mask;

				alpha_target -= min(alpha_bits, alpha_target);
				beta_target -= min(beta_bits, beta_target);

				if ((alpha_bits > 0) || (beta_bits > 0))
					assign_alpha = !assign_alpha;
			}

			switch (gpc_index % gpcs_per_reg) {
			case 0:
				map_alpha[reg_offset] |= gr_pd_alpha_ratio_table_gpc_4n0_mask_f(alpha_mask);
				map_beta[reg_offset] |= gr_pd_beta_ratio_table_gpc_4n0_mask_f(beta_mask);
				break;
			case 1:
				map_alpha[reg_offset] |= gr_pd_alpha_ratio_table_gpc_4n1_mask_f(alpha_mask);
				map_beta[reg_offset] |= gr_pd_beta_ratio_table_gpc_4n1_mask_f(beta_mask);
				break;
			case 2:
				map_alpha[reg_offset] |= gr_pd_alpha_ratio_table_gpc_4n2_mask_f(alpha_mask);
				map_beta[reg_offset] |= gr_pd_beta_ratio_table_gpc_4n2_mask_f(beta_mask);
				break;
			case 3:
				map_alpha[reg_offset] |= gr_pd_alpha_ratio_table_gpc_4n3_mask_f(alpha_mask);
				map_beta[reg_offset] |= gr_pd_beta_ratio_table_gpc_4n3_mask_f(beta_mask);
				break;
			}
			map_reg_used[reg_offset] = true;
		}
	}

	for (index = 0; index < gr_pd_alpha_ratio_table__size_1_v(); index++) {
		if (map_reg_used[index]) {
			gk20a_writel(g, gr_pd_alpha_ratio_table_r(index), map_alpha[index]);
			gk20a_writel(g, gr_pd_beta_ratio_table_r(index), map_beta[index]);
		}
	}

	return 0;
}

static int gr_gk20a_ctx_state_floorsweep(struct gk20a *g)
{
	struct gr_gk20a *gr = &g->gr;
	u32 tpc_index, gpc_index;
	u32 tpc_offset, gpc_offset;
	u32 sm_id = 0, gpc_id = 0;
	u32 sm_id_to_gpc_id[proj_scal_max_gpcs_v() * proj_scal_max_tpc_per_gpc_v()];
	u32 tpc_per_gpc;
	u32 max_ways_evict = INVALID_MAX_WAYS;

	nvhost_dbg_fn("");

	for (tpc_index = 0; tpc_index < gr->max_tpc_per_gpc_count; tpc_index++) {
		for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++) {
			gpc_offset = proj_gpc_stride_v() * gpc_index;
			if (tpc_index < gr->gpc_tpc_count[gpc_index]) {
				tpc_offset = proj_tpc_in_gpc_stride_v() * tpc_index;

				gk20a_writel(g, gr_gpc0_tpc0_sm_cfg_r() + gpc_offset + tpc_offset,
					     gr_gpc0_tpc0_sm_cfg_sm_id_f(sm_id));
				gk20a_writel(g, gr_gpc0_tpc0_l1c_cfg_smid_r() + gpc_offset + tpc_offset,
					     gr_gpc0_tpc0_l1c_cfg_smid_value_f(sm_id));
				gk20a_writel(g, gr_gpc0_gpm_pd_sm_id_r(tpc_index) + gpc_offset,
					     gr_gpc0_gpm_pd_sm_id_id_f(sm_id));
				gk20a_writel(g, gr_gpc0_tpc0_pe_cfg_smid_r() + gpc_offset + tpc_offset,
					     gr_gpc0_tpc0_pe_cfg_smid_value_f(sm_id));

				sm_id_to_gpc_id[sm_id] = gpc_index;
				sm_id++;
			}

			gk20a_writel(g, gr_gpc0_gpm_pd_active_tpcs_r() + gpc_offset,
				     gr_gpc0_gpm_pd_active_tpcs_num_f(gr->gpc_tpc_count[gpc_index]));
			gk20a_writel(g, gr_gpc0_gpm_sd_active_tpcs_r() + gpc_offset,
				     gr_gpc0_gpm_sd_active_tpcs_num_f(gr->gpc_tpc_count[gpc_index]));
		}
	}

	for (tpc_index = 0, gpc_id = 0;
	     tpc_index < gr_pd_num_tpc_per_gpc__size_1_v();
	     tpc_index++, gpc_id += 8) {

		if (gpc_id >= gr->gpc_count)
			gpc_id = 0;

		tpc_per_gpc =
			gr_pd_num_tpc_per_gpc_count0_f(gr->gpc_tpc_count[gpc_id + 0]) |
			gr_pd_num_tpc_per_gpc_count1_f(gr->gpc_tpc_count[gpc_id + 1]) |
			gr_pd_num_tpc_per_gpc_count2_f(gr->gpc_tpc_count[gpc_id + 2]) |
			gr_pd_num_tpc_per_gpc_count3_f(gr->gpc_tpc_count[gpc_id + 3]) |
			gr_pd_num_tpc_per_gpc_count4_f(gr->gpc_tpc_count[gpc_id + 4]) |
			gr_pd_num_tpc_per_gpc_count5_f(gr->gpc_tpc_count[gpc_id + 5]) |
			gr_pd_num_tpc_per_gpc_count6_f(gr->gpc_tpc_count[gpc_id + 6]) |
			gr_pd_num_tpc_per_gpc_count7_f(gr->gpc_tpc_count[gpc_id + 7]);

		gk20a_writel(g, gr_pd_num_tpc_per_gpc_r(tpc_index), tpc_per_gpc);
		gk20a_writel(g, gr_ds_num_tpc_per_gpc_r(tpc_index), tpc_per_gpc);
	}

	/* grSetupPDMapping stubbed for gk20a */
	gr_gk20a_setup_rop_mapping(g, gr);
	gr_gk20a_setup_alpha_beta_tables(g, gr);

	if (gr->num_fbps == 1)
		max_ways_evict = 9;

	if (max_ways_evict != INVALID_MAX_WAYS)
		gk20a_writel(g, ltc_ltcs_ltss_tstg_set_mgmt_r(),
			     ((gk20a_readl(g, ltc_ltcs_ltss_tstg_set_mgmt_r()) &
			       ~(ltc_ltcs_ltss_tstg_set_mgmt_max_ways_evict_last_f(~0))) |
			      ltc_ltcs_ltss_tstg_set_mgmt_max_ways_evict_last_f(max_ways_evict)));

	for (gpc_index = 0;
	     gpc_index < gr_pd_dist_skip_table__size_1_v() * 4;
	     gpc_index += 4) {

		gk20a_writel(g, gr_pd_dist_skip_table_r(gpc_index/4),
			     gr_pd_dist_skip_table_gpc_4n0_mask_f(gr->gpc_skip_mask[gpc_index]) ||
			     gr_pd_dist_skip_table_gpc_4n1_mask_f(gr->gpc_skip_mask[gpc_index + 1]) ||
			     gr_pd_dist_skip_table_gpc_4n2_mask_f(gr->gpc_skip_mask[gpc_index + 2]) ||
			     gr_pd_dist_skip_table_gpc_4n3_mask_f(gr->gpc_skip_mask[gpc_index + 3]));
	}

	gk20a_writel(g, gr_cwd_fs_r(),
		     gr_cwd_fs_num_gpcs_f(gr->gpc_count) |
		     gr_cwd_fs_num_tpcs_f(gr->tpc_count));

	gk20a_writel(g, gr_bes_zrop_settings_r(),
		     gr_bes_zrop_settings_num_active_fbps_f(gr->num_fbps));
	gk20a_writel(g, gr_bes_crop_settings_r(),
		     gr_bes_crop_settings_num_active_fbps_f(gr->num_fbps));

	return 0;
}

static int gr_gk20a_fecs_ctx_image_save(struct channel_gk20a *c, u32 save_type)
{
	struct gk20a *g = c->g;
	int ret;

	u32 inst_base_ptr =
		u64_lo32(sg_phys(c->inst_block.mem.sgt->sgl)
		>> ram_in_base_shift_v());

	nvhost_dbg_fn("");

	ret = gr_gk20a_submit_fecs_method(g, 0, 0, 3,
			gr_fecs_current_ctx_ptr_f(inst_base_ptr) |
			gr_fecs_current_ctx_target_vid_mem_f() |
			gr_fecs_current_ctx_valid_f(1), save_type, 0,
			GR_IS_UCODE_OP_AND, 1, GR_IS_UCODE_OP_AND, 2);
	if (ret)
		nvhost_err(dev_from_gk20a(g), "save context image failed");

	return ret;
}

/* init global golden image from a fresh gr_ctx in channel ctx.
   save a copy in local_golden_image in ctx_vars */
static int gr_gk20a_init_golden_ctx_image(struct gk20a *g,
					  struct channel_gk20a *c)
{
	struct gr_gk20a *gr = &g->gr;
	struct channel_ctx_gk20a *ch_ctx = &c->ch_ctx;
	u32 ctx_header_bytes = ctxsw_prog_fecs_header_v();
	u32 ctx_header_words;
	u32 i;
	u32 data;
	void *ctx_ptr = NULL;
	void *gold_ptr = NULL;
	u32 err = 0;

	nvhost_dbg_fn("");

	/* golden ctx is global to all channels. Although only the first
	   channel initializes golden image, driver needs to prevent multiple
	   channels from initializing golden ctx at the same time */
	mutex_lock(&gr->ctx_mutex);

	if (gr->ctx_vars.golden_image_initialized)
		goto clean_up;

	err = gr_gk20a_fecs_ctx_bind_channel(g, c);
	if (err)
		goto clean_up;

	err = gr_gk20a_commit_global_ctx_buffers(g, c, 0);
	if (err)
		goto clean_up;

	gold_ptr = nvhost_memmgr_mmap(gr->global_ctx_buffer[GOLDEN_CTX].ref);
	if (!gold_ptr)
		goto clean_up;

	ctx_ptr = nvhost_memmgr_mmap(ch_ctx->gr_ctx.mem.ref);
	if (!ctx_ptr)
		goto clean_up;

	ctx_header_words =  roundup(ctx_header_bytes, sizeof(u32));
	ctx_header_words >>= 2;

	/* Channel gr_ctx buffer is gpu cacheable.
	   Flush before cpu read. */
	gk20a_mm_fb_flush(g);
	gk20a_mm_l2_flush(g, false);

	for (i = 0; i < ctx_header_words; i++) {
		data = mem_rd32(ctx_ptr, i);
		mem_wr32(gold_ptr, i, data);
	}

	mem_wr32(gold_ptr + ctxsw_prog_main_image_zcull_v(), 0,
		 ctxsw_prog_main_image_zcull_mode_no_ctxsw_v());

	mem_wr32(gold_ptr + ctxsw_prog_main_image_zcull_ptr_v(), 0, 0);

	gr_gk20a_commit_inst(c, ch_ctx->global_ctx_buffer_va[GOLDEN_CTX_VA]);

	gr_gk20a_fecs_ctx_image_save(c, gr_fecs_method_push_adr_wfi_golden_save_f());

	if (gr->ctx_vars.local_golden_image == NULL) {

		gr->ctx_vars.local_golden_image =
			kzalloc(gr->ctx_vars.golden_image_size, GFP_KERNEL);

		if (gr->ctx_vars.local_golden_image == NULL) {
			err = -ENOMEM;
			goto clean_up;
		}

		for (i = 0; i < gr->ctx_vars.golden_image_size / 4; i++)
			gr->ctx_vars.local_golden_image[i] =
				mem_rd32(gold_ptr, i);
	}

	gr_gk20a_commit_inst(c, ch_ctx->gr_ctx.gpu_va);

	gr->ctx_vars.golden_image_initialized = true;

	gk20a_mm_l2_invalidate(g);

	gk20a_writel(g, gr_fecs_current_ctx_r(),
		gr_fecs_current_ctx_valid_false_f());

clean_up:
	if (err)
		nvhost_dbg(dbg_fn | dbg_err, "fail");
	else
		nvhost_dbg_fn("done");

	if (gold_ptr)
		nvhost_memmgr_munmap(gr->global_ctx_buffer[GOLDEN_CTX].ref,
				     gold_ptr);
	if (ctx_ptr)
		nvhost_memmgr_munmap(ch_ctx->gr_ctx.mem.ref, ctx_ptr);

	mutex_unlock(&gr->ctx_mutex);
	return err;
}

/* load saved fresh copy of gloden image into channel gr_ctx */
static int gr_gk20a_load_golden_ctx_image(struct gk20a *g,
					struct channel_gk20a *c)
{
	struct gr_gk20a *gr = &g->gr;
	struct channel_ctx_gk20a *ch_ctx = &c->ch_ctx;
	u32 virt_addr_lo;
	u32 virt_addr_hi;
	u32 i;
	int ret = 0;
	void *ctx_ptr = NULL;

	nvhost_dbg_fn("");

	if (gr->ctx_vars.local_golden_image == NULL)
		return -1;

	/* Channel gr_ctx buffer is gpu cacheable.
	   Flush and invalidate before cpu update. */
	gk20a_mm_fb_flush(g);
	gk20a_mm_l2_flush(g, true);

	ctx_ptr = nvhost_memmgr_mmap(ch_ctx->gr_ctx.mem.ref);
	if (!ctx_ptr)
		return -ENOMEM;

	for (i = 0; i < gr->ctx_vars.golden_image_size / 4; i++)
		mem_wr32(ctx_ptr, i, gr->ctx_vars.local_golden_image[i]);

	mem_wr32(ctx_ptr + ctxsw_prog_main_image_num_save_ops_v(), 0, 0);
	mem_wr32(ctx_ptr + ctxsw_prog_main_image_num_restore_ops_v(), 0, 0);

	virt_addr_lo = u64_lo32(ch_ctx->patch_ctx.gpu_va);
	virt_addr_hi = u64_hi32(ch_ctx->patch_ctx.gpu_va);

	mem_wr32(ctx_ptr + ctxsw_prog_main_image_patch_count_v(), 0,
		 ch_ctx->patch_ctx.data_count);
	mem_wr32(ctx_ptr + ctxsw_prog_main_image_patch_adr_lo_v(), 0,
		 virt_addr_lo);
	mem_wr32(ctx_ptr + ctxsw_prog_main_image_patch_adr_hi_v(), 0,
		 virt_addr_hi);

	/* no user for client managed performance counter ctx */
	ch_ctx->pm_ctx.ctx_sw_mode =
		ctxsw_prog_main_image_pm_mode_no_ctxsw_v();

	mem_wr32(ctx_ptr + ctxsw_prog_main_image_pm_v(), 0,
		ch_ctx->pm_ctx.ctx_sw_mode);
	mem_wr32(ctx_ptr + ctxsw_prog_main_image_pm_ptr_v(), 0, 0);

	nvhost_memmgr_munmap(ch_ctx->gr_ctx.mem.ref, ctx_ptr);

	gk20a_mm_l2_invalidate(g);

	if (tegra_platform_is_linsim()) {
		u32 inst_base_ptr =
			u64_lo32(sg_phys(c->inst_block.mem.sgt->sgl)
			>> ram_in_base_shift_v());

		ret = gr_gk20a_submit_fecs_method(g, 0, 0, ~0,
				gr_fecs_current_ctx_ptr_f(inst_base_ptr) |
				gr_fecs_current_ctx_target_vid_mem_f() |
				gr_fecs_current_ctx_valid_f(1),
				gr_fecs_method_push_adr_restore_golden_f(), 0,
				GR_IS_UCODE_OP_EQUAL, gr_fecs_ctxsw_mailbox_value_pass_v(),
				GR_IS_UCODE_OP_SKIP, 0);
		if (ret)
			nvhost_err(dev_from_gk20a(g),
				   "restore context image failed");
	}

	return ret;
}

static void gr_gk20a_start_falcon_ucode(struct gk20a *g)
{
	nvhost_dbg_fn("");

	gk20a_writel(g, gr_fecs_ctxsw_mailbox_clear_r(0),
		     gr_fecs_ctxsw_mailbox_clear_value_f(~0));

	gk20a_writel(g, gr_gpccs_dmactl_r(), gr_gpccs_dmactl_require_ctx_f(0));
	gk20a_writel(g, gr_fecs_dmactl_r(), gr_fecs_dmactl_require_ctx_f(0));

	gk20a_writel(g, gr_gpccs_cpuctl_r(), gr_gpccs_cpuctl_startcpu_f(1));
	gk20a_writel(g, gr_fecs_cpuctl_r(), gr_fecs_cpuctl_startcpu_f(1));

	nvhost_dbg_fn("done");
}

static int gr_gk20a_load_ctxsw_ucode(struct gk20a *g, struct gr_gk20a *gr)
{
	u32 ret;

	nvhost_dbg_fn("");

	if (tegra_platform_is_linsim()) {
		gk20a_writel(g, gr_fecs_ctxsw_mailbox_r(7),
			gr_fecs_ctxsw_mailbox_value_f(0xc0de7777));
		gk20a_writel(g, gr_gpccs_ctxsw_mailbox_r(7),
			gr_gpccs_ctxsw_mailbox_value_f(0xc0de7777));
	}

	gr_gk20a_load_falcon_dmem(g);
	gr_gk20a_load_falcon_imem(g);

	gr_gk20a_start_falcon_ucode(g);

	ret = gr_gk20a_ctx_wait_ucode(g, 0, 0,
				      GR_IS_UCODE_OP_EQUAL,
				      eUcodeHandshakeInitComplete,
				      GR_IS_UCODE_OP_SKIP, 0);
	if (ret) {
		nvhost_err(dev_from_gk20a(g), "falcon ucode init timeout");
		return ret;
	}

	gk20a_writel(g, gr_fecs_ctxsw_mailbox_clear_r(0), 0xffffffff);
	gk20a_writel(g, gr_fecs_method_data_r(), 0x7fffffff);
	gk20a_writel(g, gr_fecs_method_push_r(),
		     gr_fecs_method_push_adr_set_watchdog_timeout_f());

	nvhost_dbg_fn("done");
	return 0;
}

static int gr_gk20a_init_ctx_state(struct gk20a *g, struct gr_gk20a *gr)
{
	u32 golden_ctx_image_size = 0;
	u32 zcull_ctx_image_size = 0;
	u32 pm_ctx_image_size = 0;
	u32 ret;

	nvhost_dbg_fn("");

	ret = gr_gk20a_submit_fecs_method(g, 0, 0, ~0, 0,
			gr_fecs_method_push_adr_discover_image_size_f(),
			&golden_ctx_image_size,
			GR_IS_UCODE_OP_NOT_EQUAL, 0, GR_IS_UCODE_OP_SKIP, 0);
	if (ret) {
		nvhost_err(dev_from_gk20a(g),
			   "query golden image size failed");
		return ret;
	}

	ret = gr_gk20a_submit_fecs_method(g, 0, 0, ~0, 0,
			gr_fecs_method_push_adr_discover_zcull_image_size_f(),
			&zcull_ctx_image_size,
			GR_IS_UCODE_OP_NOT_EQUAL, 0, GR_IS_UCODE_OP_SKIP, 0);
	if (ret) {
		nvhost_err(dev_from_gk20a(g),
			   "query zcull ctx image size failed");
		return ret;
	}

	ret = gr_gk20a_submit_fecs_method(g, 0, 0, ~0, 0,
			gr_fecs_method_push_adr_discover_pm_image_size_f(),
			&pm_ctx_image_size,
			GR_IS_UCODE_OP_NOT_EQUAL, 0, GR_IS_UCODE_OP_SKIP, 0);
	if (ret) {
		nvhost_err(dev_from_gk20a(g),
			   "query pm ctx image size failed");
		return ret;
	}

	if (!g->gr.ctx_vars.golden_image_size &&
	    !g->gr.ctx_vars.zcull_ctxsw_image_size) {
		g->gr.ctx_vars.golden_image_size = golden_ctx_image_size;
		g->gr.ctx_vars.zcull_ctxsw_image_size = zcull_ctx_image_size;
	} else {
		/* hw is different after railgating? */
		BUG_ON(g->gr.ctx_vars.golden_image_size != golden_ctx_image_size);
		BUG_ON(g->gr.ctx_vars.zcull_ctxsw_image_size != zcull_ctx_image_size);
	}

	nvhost_dbg_fn("done");
	return 0;
}

static int gr_gk20a_alloc_global_ctx_buffers(struct gk20a *g)
{
	struct gr_gk20a *gr = &g->gr;
	struct mem_mgr *memmgr = mem_mgr_from_g(g);
	struct mem_handle *mem;
	u32 i, attr_buffer_size;

	u32 cb_buffer_size = gr_scc_bundle_cb_size_div_256b__prod_v() *
		gr_scc_bundle_cb_size_div_256b_byte_granularity_v();

	u32 pagepool_buffer_size = gr_scc_pagepool_total_pages_hwmax_value_v() *
		gr_scc_pagepool_total_pages_byte_granularity_v();

	u32 attr_cb_default_size = gr_gpc0_ppc0_cbm_cfg_size_default_v();
	u32 alpha_cb_default_size = gr_gpc0_ppc0_cbm_cfg2_size_default_v();

	u32 attr_cb_size =
		attr_cb_default_size + (attr_cb_default_size >> 1);
	u32 alpha_cb_size =
		alpha_cb_default_size + (alpha_cb_default_size >> 1);

	u32 num_tpcs_per_pes = proj_scal_litter_num_tpcs_per_pes_v();
	u32 attr_max_size_per_tpc =
		gr_gpc0_ppc0_cbm_cfg_size_v(~0) / num_tpcs_per_pes;
	u32 alpha_max_size_per_tpc =
		gr_gpc0_ppc0_cbm_cfg2_size_v(~0) / num_tpcs_per_pes;


	nvhost_dbg_fn("");

	attr_cb_size =
		(attr_cb_size > attr_max_size_per_tpc) ?
			attr_max_size_per_tpc : attr_cb_size;
	attr_cb_default_size =
		(attr_cb_default_size > attr_cb_size) ?
			attr_cb_size : attr_cb_default_size;
	alpha_cb_size =
		(alpha_cb_size > alpha_max_size_per_tpc) ?
			alpha_max_size_per_tpc : alpha_cb_size;
	alpha_cb_default_size =
		(alpha_cb_default_size > alpha_cb_size) ?
			alpha_cb_size : alpha_cb_default_size;

	attr_buffer_size =
		(gr_gpc0_ppc0_cbm_cfg_size_granularity_v() * alpha_cb_size +
		 gr_gpc0_ppc0_cbm_cfg2_size_granularity_v() * alpha_cb_size) *
		 gr->gpc_count;

	nvhost_dbg_info("cb_buffer_size : %d", cb_buffer_size);

	mem = nvhost_memmgr_alloc(memmgr, cb_buffer_size,
				  DEFAULT_ALLOC_ALIGNMENT,
				  DEFAULT_ALLOC_FLAGS,
				  0);
	if (IS_ERR(mem))
		goto clean_up;

	gr->global_ctx_buffer[CIRCULAR].ref = mem;
	gr->global_ctx_buffer[CIRCULAR].size = cb_buffer_size;

	mem = nvhost_memmgr_alloc(memmgr, cb_buffer_size,
				  DEFAULT_ALLOC_ALIGNMENT,
				  DEFAULT_ALLOC_FLAGS,
				  NVMAP_HEAP_CARVEOUT_VPR);
	if (!IS_ERR(mem)) {
		gr->global_ctx_buffer[CIRCULAR_VPR].ref = mem;
		gr->global_ctx_buffer[CIRCULAR_VPR].size = cb_buffer_size;
	}

	nvhost_dbg_info("pagepool_buffer_size : %d", pagepool_buffer_size);

	mem = nvhost_memmgr_alloc(memmgr, pagepool_buffer_size,
				  DEFAULT_ALLOC_ALIGNMENT,
				  DEFAULT_ALLOC_FLAGS,
				  0);
	if (IS_ERR(mem))
		goto clean_up;

	gr->global_ctx_buffer[PAGEPOOL].ref = mem;
	gr->global_ctx_buffer[PAGEPOOL].size = pagepool_buffer_size;

	mem = nvhost_memmgr_alloc(memmgr, pagepool_buffer_size,
				  DEFAULT_ALLOC_ALIGNMENT,
				  DEFAULT_ALLOC_FLAGS,
				  NVMAP_HEAP_CARVEOUT_VPR);
	if (!IS_ERR(mem)) {
		gr->global_ctx_buffer[PAGEPOOL_VPR].ref = mem;
		gr->global_ctx_buffer[PAGEPOOL_VPR].size = pagepool_buffer_size;
	}

	nvhost_dbg_info("attr_buffer_size : %d", attr_buffer_size);

	mem = nvhost_memmgr_alloc(memmgr, attr_buffer_size,
				  DEFAULT_ALLOC_ALIGNMENT,
				  DEFAULT_ALLOC_FLAGS,
				  0);
	if (IS_ERR(mem))
		goto clean_up;

	gr->global_ctx_buffer[ATTRIBUTE].ref = mem;
	gr->global_ctx_buffer[ATTRIBUTE].size = attr_buffer_size;

	mem = nvhost_memmgr_alloc(memmgr, attr_buffer_size,
				  DEFAULT_ALLOC_ALIGNMENT,
				  DEFAULT_ALLOC_FLAGS,
				  NVMAP_HEAP_CARVEOUT_VPR);
	if (!IS_ERR(mem)) {
		gr->global_ctx_buffer[ATTRIBUTE_VPR].ref = mem;
		gr->global_ctx_buffer[ATTRIBUTE_VPR].size = attr_buffer_size;
	}

	nvhost_dbg_info("golden_image_size : %d",
		   gr->ctx_vars.golden_image_size);

	mem = nvhost_memmgr_alloc(memmgr, gr->ctx_vars.golden_image_size,
				  DEFAULT_ALLOC_ALIGNMENT,
				  DEFAULT_ALLOC_FLAGS,
				  0);
	if (IS_ERR(mem))
		goto clean_up;

	gr->global_ctx_buffer[GOLDEN_CTX].ref = mem;
	gr->global_ctx_buffer[GOLDEN_CTX].size =
		gr->ctx_vars.golden_image_size;

	nvhost_dbg_fn("done");
	return 0;

 clean_up:
	nvhost_dbg(dbg_fn | dbg_err, "fail");
	for (i = 0; i < NR_GLOBAL_CTX_BUF; i++) {
		if (gr->global_ctx_buffer[i].ref) {
			nvhost_memmgr_put(memmgr,
					  gr->global_ctx_buffer[i].ref);
			memset(&gr->global_ctx_buffer[i],
				0, sizeof(struct mem_desc));
		}
	}
	return -ENOMEM;
}

static void gr_gk20a_free_global_ctx_buffers(struct gk20a *g)
{
	struct gr_gk20a *gr = &g->gr;
	struct mem_mgr *memmgr = mem_mgr_from_g(g);
	u32 i;

	for (i = 0; i < NR_GLOBAL_CTX_BUF; i++) {
		nvhost_memmgr_put(memmgr, gr->global_ctx_buffer[i].ref);
		memset(&gr->global_ctx_buffer[i], 0, sizeof(struct mem_desc));
	}

	nvhost_dbg_fn("done");
}

static int gr_gk20a_map_global_ctx_buffers(struct gk20a *g,
					struct channel_gk20a *c)
{
	struct vm_gk20a *ch_vm = c->vm;
	struct mem_mgr *memmgr = mem_mgr_from_g(g);
	struct mem_handle *handle_ref;
	u64 *g_bfr_va = c->ch_ctx.global_ctx_buffer_va;
	struct gr_gk20a *gr = &g->gr;
	u64 gpu_va;
	u32 i;
	nvhost_dbg_fn("");

	/* Circular Buffer */
	if (!c->vpr || (gr->global_ctx_buffer[CIRCULAR_VPR].ref == NULL))
		handle_ref = gr->global_ctx_buffer[CIRCULAR].ref;
	else
		handle_ref = gr->global_ctx_buffer[CIRCULAR_VPR].ref;

	gpu_va = ch_vm->map(ch_vm, memmgr, handle_ref,
			    /*offset_align, flags, kind*/
			    0, NVHOST_MAP_BUFFER_FLAGS_CACHEABLE_TRUE, 0,
			    NULL, false);
	if (!gpu_va)
		goto clean_up;
	g_bfr_va[CIRCULAR_VA] = gpu_va;

	/* Attribute Buffer */
	if (!c->vpr || (gr->global_ctx_buffer[ATTRIBUTE_VPR].ref == NULL))
		handle_ref = gr->global_ctx_buffer[ATTRIBUTE].ref;
	else
		handle_ref = gr->global_ctx_buffer[ATTRIBUTE_VPR].ref;

	gpu_va = ch_vm->map(ch_vm, memmgr, handle_ref,
			    /*offset_align, flags, kind*/
			    0, NVHOST_MAP_BUFFER_FLAGS_CACHEABLE_TRUE, 0,
			    NULL, false);
	if (!gpu_va)
		goto clean_up;
	g_bfr_va[ATTRIBUTE_VA] = gpu_va;

	/* Page Pool */
	if (!c->vpr || (gr->global_ctx_buffer[PAGEPOOL_VPR].ref == NULL))
		handle_ref = gr->global_ctx_buffer[PAGEPOOL].ref;
	else
		handle_ref = gr->global_ctx_buffer[PAGEPOOL_VPR].ref;

	gpu_va = ch_vm->map(ch_vm, memmgr, handle_ref,
			    /*offset_align, flags, kind*/
			    0, NVHOST_MAP_BUFFER_FLAGS_CACHEABLE_TRUE, 0,
			    NULL, false);
	if (!gpu_va)
		goto clean_up;
	g_bfr_va[PAGEPOOL_VA] = gpu_va;

	/* Golden Image */
	gpu_va = ch_vm->map(ch_vm, memmgr,
			    gr->global_ctx_buffer[GOLDEN_CTX].ref,
			    /*offset_align, flags, kind*/
			    0, 0, 0, NULL, false);
	if (!gpu_va)
		goto clean_up;
	g_bfr_va[GOLDEN_CTX_VA] = gpu_va;

	c->ch_ctx.global_ctx_buffer_mapped = true;
	return 0;

 clean_up:
	for (i = 0; i < NR_GLOBAL_CTX_BUF_VA; i++) {
		if (g_bfr_va[i]) {
			ch_vm->unmap(ch_vm, g_bfr_va[i]);
			g_bfr_va[i] = 0;
		}
	}
	return -ENOMEM;
}

static void gr_gk20a_unmap_global_ctx_buffers(struct channel_gk20a *c)
{
	struct vm_gk20a *ch_vm = c->vm;
	u64 *g_bfr_va = c->ch_ctx.global_ctx_buffer_va;
	u32 i;

	nvhost_dbg_fn("");

	for (i = 0; i < NR_GLOBAL_CTX_BUF_VA; i++) {
		if (g_bfr_va[i]) {
			ch_vm->unmap(ch_vm, g_bfr_va[i]);
			g_bfr_va[i] = 0;
		}
	}
	c->ch_ctx.global_ctx_buffer_mapped = false;
}

static int gr_gk20a_alloc_channel_gr_ctx(struct gk20a *g,
				struct channel_gk20a *c)
{
	struct gr_gk20a *gr = &g->gr;
	struct gr_ctx_desc *gr_ctx = &c->ch_ctx.gr_ctx;
	struct mem_mgr *memmgr = gk20a_channel_mem_mgr(c);
	struct vm_gk20a *ch_vm = c->vm;

	nvhost_dbg_fn("");

	if (gr->ctx_vars.buffer_size == 0)
		return 0;

	/* alloc channel gr ctx buffer */
	gr->ctx_vars.buffer_size = gr->ctx_vars.golden_image_size;
	gr->ctx_vars.buffer_total_size = gr->ctx_vars.golden_image_size;

	gr_ctx->mem.ref = nvhost_memmgr_alloc(memmgr,
					      gr->ctx_vars.buffer_total_size,
					      DEFAULT_ALLOC_ALIGNMENT,
					      DEFAULT_ALLOC_FLAGS,
					      0);

	if (IS_ERR(gr_ctx->mem.ref))
		return -ENOMEM;

	gr_ctx->gpu_va = ch_vm->map(ch_vm, memmgr,
		gr_ctx->mem.ref,
		/*offset_align, flags, kind*/
		0, NVHOST_MAP_BUFFER_FLAGS_CACHEABLE_TRUE, 0, NULL, false);
	if (!gr_ctx->gpu_va) {
		nvhost_memmgr_put(memmgr, gr_ctx->mem.ref);
		return -ENOMEM;
	}

	return 0;
}

static void gr_gk20a_free_channel_gr_ctx(struct channel_gk20a *c)
{
	struct channel_ctx_gk20a *ch_ctx = &c->ch_ctx;
	struct mem_mgr *ch_nvmap = gk20a_channel_mem_mgr(c);
	struct vm_gk20a *ch_vm = c->vm;

	nvhost_dbg_fn("");

	ch_vm->unmap(ch_vm, ch_ctx->gr_ctx.gpu_va);
	nvhost_memmgr_put(ch_nvmap, ch_ctx->gr_ctx.mem.ref);
}

static int gr_gk20a_alloc_channel_patch_ctx(struct gk20a *g,
				struct channel_gk20a *c)
{
	struct patch_desc *patch_ctx = &c->ch_ctx.patch_ctx;
	struct mem_mgr *memmgr = gk20a_channel_mem_mgr(c);
	struct vm_gk20a *ch_vm = c->vm;

	nvhost_dbg_fn("");

	patch_ctx->mem.ref = nvhost_memmgr_alloc(memmgr, 128 * sizeof(u32),
						 DEFAULT_ALLOC_ALIGNMENT,
						 DEFAULT_ALLOC_FLAGS,
						 0);
	if (IS_ERR(patch_ctx->mem.ref))
		return -ENOMEM;

	patch_ctx->gpu_va = ch_vm->map(ch_vm, memmgr,
				patch_ctx->mem.ref,
				/*offset_align, flags, kind*/
				0, 0, 0, NULL, false);
	if (!patch_ctx->gpu_va)
		goto clean_up;

	nvhost_dbg_fn("done");
	return 0;

 clean_up:
	nvhost_dbg(dbg_fn | dbg_err, "fail");
	if (patch_ctx->mem.ref) {
		nvhost_memmgr_put(memmgr, patch_ctx->mem.ref);
		patch_ctx->mem.ref = 0;
	}

	return -ENOMEM;
}

static void gr_gk20a_unmap_channel_patch_ctx(struct channel_gk20a *c)
{
	struct patch_desc *patch_ctx = &c->ch_ctx.patch_ctx;
	struct vm_gk20a *ch_vm = c->vm;

	nvhost_dbg_fn("");

	if (patch_ctx->gpu_va)
		ch_vm->unmap(ch_vm, patch_ctx->gpu_va);
	patch_ctx->gpu_va = 0;
	patch_ctx->data_count = 0;
}

static void gr_gk20a_free_channel_patch_ctx(struct channel_gk20a *c)
{
	struct patch_desc *patch_ctx = &c->ch_ctx.patch_ctx;
	struct mem_mgr *memmgr = gk20a_channel_mem_mgr(c);

	nvhost_dbg_fn("");

	gr_gk20a_unmap_channel_patch_ctx(c);

	if (patch_ctx->mem.ref) {
		nvhost_memmgr_put(memmgr, patch_ctx->mem.ref);
		patch_ctx->mem.ref = 0;
	}
}

void gk20a_free_channel_ctx(struct channel_gk20a *c)
{
	gr_gk20a_unmap_global_ctx_buffers(c);
	gr_gk20a_free_channel_patch_ctx(c);
	gr_gk20a_free_channel_gr_ctx(c);

	/* zcull_ctx, pm_ctx */

	memset(&c->ch_ctx, 0, sizeof(struct channel_ctx_gk20a));

	c->num_objects = 0;
	c->first_init = false;
}

int gk20a_alloc_obj_ctx(struct channel_gk20a  *c,
			struct nvhost_alloc_obj_ctx_args *args)
{
	struct gk20a *g = c->g;
	struct channel_ctx_gk20a *ch_ctx = &c->ch_ctx;
	bool change_to_compute_mode = false;
	int err = 0;

	nvhost_dbg_fn("");

	/* an address space needs to have been bound at this point.*/
	if (!gk20a_channel_as_bound(c)) {
		nvhost_err(dev_from_gk20a(g),
			   "not bound to address space at time"
			   " of grctx allocation");
		return -EINVAL;
	}

	switch (args->class_num) {
	case KEPLER_COMPUTE_A:
		/* tbd: NV2080_CTRL_GPU_COMPUTE_MODE_RULES_EXCLUSIVE_COMPUTE */
		/* tbd: PDB_PROP_GRAPHICS_DISTINCT_3D_AND_COMPUTE_STATE_DEF  */
		change_to_compute_mode = true;
		break;
	case KEPLER_C:
	case FERMI_TWOD_A:
	case KEPLER_DMA_COPY_A:
		break;

	default:
		nvhost_err(dev_from_gk20a(g),
			   "invalid obj class 0x%x", args->class_num);
		err = -EINVAL;
		goto out;
	}

	/* allocate gr ctx buffer */
	if (ch_ctx->gr_ctx.mem.ref == NULL) {
		err = gr_gk20a_alloc_channel_gr_ctx(g, c);
		if (err) {
			nvhost_err(dev_from_gk20a(g),
				"fail to allocate gr ctx buffer");
			goto out;
		}
	} else {
		/*TBD: needs to be more subtle about which is being allocated
		* as some are allowed to be allocated along same channel */
		nvhost_err(dev_from_gk20a(g),
			"too many classes alloc'd on same channel");
		err = -EINVAL;
		goto out;
	}

	/* commit gr ctx buffer */
	err = gr_gk20a_commit_inst(c, ch_ctx->gr_ctx.gpu_va);
	if (err) {
		nvhost_err(dev_from_gk20a(g),
			"fail to commit gr ctx buffer");
		goto out;
	}

	/* allocate patch buffer */
	if (ch_ctx->patch_ctx.mem.ref == NULL) {
		err = gr_gk20a_alloc_channel_patch_ctx(g, c);
		if (err) {
			nvhost_err(dev_from_gk20a(g),
				"fail to allocate patch buffer");
			goto out;
		}
	}

	/* map global buffer to channel gpu_va and commit */
	if (!ch_ctx->global_ctx_buffer_mapped) {
		err = gr_gk20a_map_global_ctx_buffers(g, c);
		if (err) {
			nvhost_err(dev_from_gk20a(g),
				"fail to map global ctx buffer");
			goto out;
		}
		gr_gk20a_elpg_protected_call(g,
			gr_gk20a_commit_global_ctx_buffers(g, c, 1));
	}

	/* init gloden image, ELPG enabled after this is done */
	err = gr_gk20a_init_golden_ctx_image(g, c);
	if (err) {
		nvhost_err(dev_from_gk20a(g),
			"fail to init golden ctx image");
		goto out;
	}

	/* load golden image */
	if (!c->first_init) {
		err = gr_gk20a_elpg_protected_call(g,
			gr_gk20a_load_golden_ctx_image(g, c));
		if (err) {
			nvhost_err(dev_from_gk20a(g),
				"fail to load golden ctx image");
			goto out;
		}
		c->first_init = true;
	}
	gk20a_mm_l2_invalidate(g);
	c->num_objects++;

	nvhost_dbg_fn("done");
	return 0;
out:
	/* 1. gr_ctx, patch_ctx and global ctx buffer mapping
	   can be reused so no need to release them.
	   2. golden image init and load is a one time thing so if
	   they pass, no need to undo. */
	nvhost_dbg(dbg_fn | dbg_err, "fail");
	return err;
}

int gk20a_free_obj_ctx(struct channel_gk20a  *c,
		       struct nvhost_free_obj_ctx_args *args)
{
	unsigned long timeout = gk20a_get_gr_idle_timeout(c->g);

	nvhost_dbg_fn("");

	if (c->num_objects == 0)
		return 0;

	c->num_objects--;

	if (c->num_objects == 0) {
		c->first_init = false;
		gk20a_disable_channel(c, true, /*wait for finish*/
				      timeout);
		gr_gk20a_unmap_channel_patch_ctx(c);
	}

	return 0;
}

static void gk20a_remove_gr_support(struct gr_gk20a *gr)
{
	struct gk20a *g = gr->g;
	struct mem_mgr *memmgr = mem_mgr_from_g(g);

	nvhost_dbg_fn("");

	gr_gk20a_free_global_ctx_buffers(g);

	nvhost_memmgr_free_sg_table(memmgr, gr->mmu_wr_mem.mem.ref,
			gr->mmu_wr_mem.mem.sgt);
	nvhost_memmgr_unpin(memmgr, gr->mmu_rd_mem.mem.ref,
			dev_from_gk20a(g), gr->mmu_rd_mem.mem.sgt);
	nvhost_memmgr_put(memmgr, gr->mmu_wr_mem.mem.ref);
	nvhost_memmgr_put(memmgr, gr->mmu_rd_mem.mem.ref);
	nvhost_memmgr_put(memmgr, gr->compbit_store.mem.ref);
	memset(&gr->mmu_wr_mem, 0, sizeof(struct mem_desc));
	memset(&gr->mmu_rd_mem, 0, sizeof(struct mem_desc));
	memset(&gr->compbit_store, 0, sizeof(struct compbit_store_desc));

	kfree(gr->gpc_tpc_count);
	kfree(gr->gpc_zcb_count);
	kfree(gr->gpc_ppc_count);
	kfree(gr->pes_tpc_count[0]);
	kfree(gr->pes_tpc_count[1]);
	kfree(gr->pes_tpc_mask[0]);
	kfree(gr->pes_tpc_mask[1]);
	kfree(gr->gpc_skip_mask);
	kfree(gr->map_tiles);
	gr->gpc_tpc_count = NULL;
	gr->gpc_zcb_count = NULL;
	gr->gpc_ppc_count = NULL;
	gr->pes_tpc_count[0] = NULL;
	gr->pes_tpc_count[1] = NULL;
	gr->pes_tpc_mask[0] = NULL;
	gr->pes_tpc_mask[1] = NULL;
	gr->gpc_skip_mask = NULL;
	gr->map_tiles = NULL;

	kfree(gr->ctx_vars.ucode.fecs.inst.l);
	kfree(gr->ctx_vars.ucode.fecs.data.l);
	kfree(gr->ctx_vars.ucode.gpccs.inst.l);
	kfree(gr->ctx_vars.ucode.gpccs.data.l);
	kfree(gr->ctx_vars.sw_bundle_init.l);
	kfree(gr->ctx_vars.sw_method_init.l);
	kfree(gr->ctx_vars.sw_ctx_load.l);
	kfree(gr->ctx_vars.sw_non_ctx_load.l);
	kfree(gr->ctx_vars.ctxsw_regs.sys.l);
	kfree(gr->ctx_vars.ctxsw_regs.gpc.l);
	kfree(gr->ctx_vars.ctxsw_regs.tpc.l);
	kfree(gr->ctx_vars.ctxsw_regs.zcull_gpc.l);
	kfree(gr->ctx_vars.ctxsw_regs.ppc.l);
	kfree(gr->ctx_vars.ctxsw_regs.pm_sys.l);
	kfree(gr->ctx_vars.ctxsw_regs.pm_gpc.l);
	kfree(gr->ctx_vars.ctxsw_regs.pm_tpc.l);

	kfree(gr->ctx_vars.local_golden_image);
	gr->ctx_vars.local_golden_image = NULL;

	nvhost_allocator_destroy(&gr->comp_tags);
}

static int gr_gk20a_init_gr_config(struct gk20a *g, struct gr_gk20a *gr)
{
	u32 gpc_index, pes_index;
	u32 pes_tpc_mask;
	u32 pes_tpc_count;
	u32 pes_heavy_index;
	u32 gpc_new_skip_mask;
	u32 tmp;

	tmp = gk20a_readl(g, pri_ringmaster_enum_fbp_r());
	gr->num_fbps = pri_ringmaster_enum_fbp_count_v(tmp);

	tmp = gk20a_readl(g, top_num_gpcs_r());
	gr->max_gpc_count = top_num_gpcs_value_v(tmp);

	tmp = gk20a_readl(g, top_num_fbps_r());
	gr->max_fbps_count = top_num_fbps_value_v(tmp);

	tmp = gk20a_readl(g, top_tpc_per_gpc_r());
	gr->max_tpc_per_gpc_count = top_tpc_per_gpc_value_v(tmp);

	gr->max_tpc_count = gr->max_gpc_count * gr->max_tpc_per_gpc_count;

	tmp = gk20a_readl(g, top_num_fbps_r());
	gr->sys_count = top_num_fbps_value_v(tmp);

	tmp = gk20a_readl(g, pri_ringmaster_enum_gpc_r());
	gr->gpc_count = pri_ringmaster_enum_gpc_count_v(tmp);

	gr->pe_count_per_gpc = proj_scal_litter_num_pes_per_gpc_v();
	gr->max_zcull_per_gpc_count = proj_scal_litter_num_zcull_banks_v();

	if (!gr->gpc_count) {
		nvhost_err(dev_from_gk20a(g), "gpc_count==0!");
		goto clean_up;
	}

	gr->gpc_tpc_count = kzalloc(gr->gpc_count * sizeof(u32), GFP_KERNEL);
	gr->gpc_zcb_count = kzalloc(gr->gpc_count * sizeof(u32), GFP_KERNEL);
	gr->gpc_ppc_count = kzalloc(gr->gpc_count * sizeof(u32), GFP_KERNEL);
	gr->pes_tpc_count[0] = kzalloc(gr->gpc_count * sizeof(u32), GFP_KERNEL);
	gr->pes_tpc_count[1] = kzalloc(gr->gpc_count * sizeof(u32), GFP_KERNEL);
	gr->pes_tpc_mask[0] = kzalloc(gr->gpc_count * sizeof(u32), GFP_KERNEL);
	gr->pes_tpc_mask[1] = kzalloc(gr->gpc_count * sizeof(u32), GFP_KERNEL);
	gr->gpc_skip_mask =
		kzalloc(gr_pd_dist_skip_table__size_1_v() * 4 * sizeof(u32),
			GFP_KERNEL);

	if (!gr->gpc_tpc_count || !gr->gpc_zcb_count || !gr->gpc_ppc_count ||
	    !gr->pes_tpc_count[0] || !gr->pes_tpc_count[1] ||
	    !gr->pes_tpc_mask[0] || !gr->pes_tpc_mask[1] || !gr->gpc_skip_mask)
		goto clean_up;

	gr->ppc_count = 0;
	for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++) {
		tmp = gk20a_readl(g, gr_gpc0_fs_gpc_r());

		gr->gpc_tpc_count[gpc_index] =
			gr_gpc0_fs_gpc_num_available_tpcs_v(tmp);
		gr->tpc_count += gr->gpc_tpc_count[gpc_index];

		gr->gpc_zcb_count[gpc_index] =
			gr_gpc0_fs_gpc_num_available_zculls_v(tmp);
		gr->zcb_count += gr->gpc_zcb_count[gpc_index];

		gr->gpc_ppc_count[gpc_index] = gr->pe_count_per_gpc;
		gr->ppc_count += gr->gpc_ppc_count[gpc_index];
		for (pes_index = 0; pes_index < gr->pe_count_per_gpc; pes_index++) {

			tmp = gk20a_readl(g,
				gr_gpc0_gpm_pd_pes_tpc_id_mask_r(pes_index) +
				gpc_index * proj_gpc_stride_v());

			pes_tpc_mask = gr_gpc0_gpm_pd_pes_tpc_id_mask_mask_v(tmp);
			pes_tpc_count = count_bits(pes_tpc_mask);

			gr->pes_tpc_count[pes_index][gpc_index] = pes_tpc_count;
			gr->pes_tpc_mask[pes_index][gpc_index] = pes_tpc_mask;
		}

		gpc_new_skip_mask = 0;
		if (gr->pes_tpc_count[0][gpc_index] +
		    gr->pes_tpc_count[1][gpc_index] == 5) {
			pes_heavy_index =
				gr->pes_tpc_count[0][gpc_index] >
				gr->pes_tpc_count[1][gpc_index] ? 0 : 1;

			gpc_new_skip_mask =
				gr->pes_tpc_mask[pes_heavy_index][gpc_index] ^
				   (gr->pes_tpc_mask[pes_heavy_index][gpc_index] &
				   (gr->pes_tpc_mask[pes_heavy_index][gpc_index] - 1));

		} else if ((gr->pes_tpc_count[0][gpc_index] +
			    gr->pes_tpc_count[1][gpc_index] == 4) &&
			   (gr->pes_tpc_count[0][gpc_index] !=
			    gr->pes_tpc_count[1][gpc_index])) {
				pes_heavy_index =
				    gr->pes_tpc_count[0][gpc_index] >
				    gr->pes_tpc_count[1][gpc_index] ? 0 : 1;

			gpc_new_skip_mask =
				gr->pes_tpc_mask[pes_heavy_index][gpc_index] ^
				   (gr->pes_tpc_mask[pes_heavy_index][gpc_index] &
				   (gr->pes_tpc_mask[pes_heavy_index][gpc_index] - 1));
		}
		gr->gpc_skip_mask[gpc_index] = gpc_new_skip_mask;
	}

	nvhost_dbg_info("fbps: %d", gr->num_fbps);
	nvhost_dbg_info("max_gpc_count: %d", gr->max_gpc_count);
	nvhost_dbg_info("max_fbps_count: %d", gr->max_fbps_count);
	nvhost_dbg_info("max_tpc_per_gpc_count: %d", gr->max_tpc_per_gpc_count);
	nvhost_dbg_info("max_zcull_per_gpc_count: %d", gr->max_zcull_per_gpc_count);
	nvhost_dbg_info("max_tpc_count: %d", gr->max_tpc_count);
	nvhost_dbg_info("sys_count: %d", gr->sys_count);
	nvhost_dbg_info("gpc_count: %d", gr->gpc_count);
	nvhost_dbg_info("pe_count_per_gpc: %d", gr->pe_count_per_gpc);
	nvhost_dbg_info("tpc_count: %d", gr->tpc_count);
	nvhost_dbg_info("ppc_count: %d", gr->ppc_count);

	for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++)
		nvhost_dbg_info("gpc_tpc_count[%d] : %d",
			   gpc_index, gr->gpc_tpc_count[gpc_index]);
	for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++)
		nvhost_dbg_info("gpc_zcb_count[%d] : %d",
			   gpc_index, gr->gpc_zcb_count[gpc_index]);
	for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++)
		nvhost_dbg_info("gpc_ppc_count[%d] : %d",
			   gpc_index, gr->gpc_ppc_count[gpc_index]);
	for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++)
		nvhost_dbg_info("gpc_skip_mask[%d] : %d",
			   gpc_index, gr->gpc_skip_mask[gpc_index]);
	for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++)
		for (pes_index = 0;
		     pes_index < gr->pe_count_per_gpc;
		     pes_index++)
			nvhost_dbg_info("pes_tpc_count[%d][%d] : %d",
				   pes_index, gpc_index,
				   gr->pes_tpc_count[pes_index][gpc_index]);

	for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++)
		for (pes_index = 0;
		     pes_index < gr->pe_count_per_gpc;
		     pes_index++)
			nvhost_dbg_info("pes_tpc_mask[%d][%d] : %d",
				   pes_index, gpc_index,
				   gr->pes_tpc_mask[pes_index][gpc_index]);

	gr->bundle_cb_default_size = gr_scc_bundle_cb_size_div_256b__prod_v();
	gr->min_gpm_fifo_depth = gr_pd_ab_dist_cfg2_state_limit_min_gpm_fifo_depths_v();
	gr->bundle_cb_token_limit = gr_pd_ab_dist_cfg2_token_limit_init_v();
	gr->attrib_cb_default_size = gr_gpc0_ppc0_cbm_cfg_size_default_v();
	/* gk20a has a fixed beta CB RAM, don't alloc more */
	gr->attrib_cb_size = gr->attrib_cb_default_size;
	gr->alpha_cb_default_size = gr_gpc0_ppc0_cbm_cfg2_size_default_v();
	gr->alpha_cb_size = gr->alpha_cb_default_size + (gr->alpha_cb_default_size >> 1);
	gr->timeslice_mode = gr_gpcs_ppcs_cbm_cfg_timeslice_mode_enable_v();

	nvhost_dbg_info("bundle_cb_default_size: %d",
		   gr->bundle_cb_default_size);
	nvhost_dbg_info("min_gpm_fifo_depth: %d", gr->min_gpm_fifo_depth);
	nvhost_dbg_info("bundle_cb_token_limit: %d", gr->bundle_cb_token_limit);
	nvhost_dbg_info("attrib_cb_default_size: %d",
		   gr->attrib_cb_default_size);
	nvhost_dbg_info("attrib_cb_size: %d", gr->attrib_cb_size);
	nvhost_dbg_info("alpha_cb_default_size: %d", gr->alpha_cb_default_size);
	nvhost_dbg_info("alpha_cb_size: %d", gr->alpha_cb_size);
	nvhost_dbg_info("timeslice_mode: %d", gr->timeslice_mode);

	return 0;

clean_up:
	return -ENOMEM;
}

static int gr_gk20a_init_mmu_sw(struct gk20a *g, struct gr_gk20a *gr)
{
	struct mem_mgr *memmgr = mem_mgr_from_g(g);
	void *mmu_ptr;

	gr->mmu_wr_mem_size = gr->mmu_rd_mem_size = 0x1000;

	gr->mmu_wr_mem.mem.ref = nvhost_memmgr_alloc(memmgr,
						     gr->mmu_wr_mem_size,
						     DEFAULT_ALLOC_ALIGNMENT,
						     DEFAULT_ALLOC_FLAGS,
						     0);
	if (IS_ERR(gr->mmu_wr_mem.mem.ref))
		goto clean_up;
	gr->mmu_wr_mem.mem.size = gr->mmu_wr_mem_size;

	gr->mmu_rd_mem.mem.ref = nvhost_memmgr_alloc(memmgr,
						     gr->mmu_rd_mem_size,
						     DEFAULT_ALLOC_ALIGNMENT,
						     DEFAULT_ALLOC_FLAGS,
						     0);
	if (IS_ERR(gr->mmu_rd_mem.mem.ref))
		goto clean_up;
	gr->mmu_rd_mem.mem.size = gr->mmu_rd_mem_size;

	mmu_ptr = nvhost_memmgr_mmap(gr->mmu_wr_mem.mem.ref);
	if (!mmu_ptr)
		goto clean_up;
	memset(mmu_ptr, 0, gr->mmu_wr_mem.mem.size);
	nvhost_memmgr_munmap(gr->mmu_wr_mem.mem.ref, mmu_ptr);

	mmu_ptr = nvhost_memmgr_mmap(gr->mmu_rd_mem.mem.ref);
	if (!mmu_ptr)
		goto clean_up;
	memset(mmu_ptr, 0, gr->mmu_rd_mem.mem.size);
	nvhost_memmgr_munmap(gr->mmu_rd_mem.mem.ref, mmu_ptr);

	gr->mmu_wr_mem.mem.sgt =
		nvhost_memmgr_sg_table(memmgr, gr->mmu_wr_mem.mem.ref);
	if (IS_ERR(gr->mmu_wr_mem.mem.sgt))
		goto clean_up;

	gr->mmu_rd_mem.mem.sgt =
		nvhost_memmgr_sg_table(memmgr, gr->mmu_rd_mem.mem.ref);
	if (IS_ERR(gr->mmu_rd_mem.mem.sgt))
		goto clean_up;
	return 0;

clean_up:
	return -ENOMEM;
}

static u32 prime_set[18] = {
	2, 3, 5, 7, 11, 13, 17, 19, 23, 39, 31, 37, 41, 43, 47, 53, 59, 61 };

static int gr_gk20a_init_map_tiles(struct gk20a *g, struct gr_gk20a *gr)
{
	s32 comm_denom;
	s32 mul_factor;
	s32 *init_frac = NULL;
	s32 *init_err = NULL;
	s32 *run_err = NULL;
	s32 *sorted_num_tpcs = NULL;
	s32 *sorted_to_unsorted_gpc_map = NULL;
	u32 gpc_index;
	u32 gpc_mark = 0;
	u32 num_tpc;
	u32 max_tpc_count = 0;
	u32 swap;
	u32 tile_count;
	u32 index;
	bool delete_map = false;
	bool gpc_sorted;
	int ret = 0;

	init_frac = kzalloc(proj_scal_max_gpcs_v() * sizeof(s32), GFP_KERNEL);
	init_err = kzalloc(proj_scal_max_gpcs_v() * sizeof(s32), GFP_KERNEL);
	run_err = kzalloc(proj_scal_max_gpcs_v() * sizeof(s32), GFP_KERNEL);
	sorted_num_tpcs =
		kzalloc(proj_scal_max_gpcs_v() *
			proj_scal_max_tpc_per_gpc_v() * sizeof(s32),
			GFP_KERNEL);
	sorted_to_unsorted_gpc_map =
		kzalloc(proj_scal_max_gpcs_v() * sizeof(s32), GFP_KERNEL);

	if (!(init_frac && init_err && run_err && sorted_num_tpcs &&
	      sorted_to_unsorted_gpc_map)) {
		ret = -ENOMEM;
		goto clean_up;
	}

	gr->map_row_offset = INVALID_SCREEN_TILE_ROW_OFFSET;

	if (gr->tpc_count == 3)
		gr->map_row_offset = 2;
	else if (gr->tpc_count < 3)
		gr->map_row_offset = 1;
	else {
		gr->map_row_offset = 3;

		for (index = 1; index < 18; index++) {
			u32 prime = prime_set[index];
			if ((gr->tpc_count % prime) != 0) {
				gr->map_row_offset = prime;
				break;
			}
		}
	}

	switch (gr->tpc_count) {
	case 15:
		gr->map_row_offset = 6;
		break;
	case 14:
		gr->map_row_offset = 5;
		break;
	case 13:
		gr->map_row_offset = 2;
		break;
	case 11:
		gr->map_row_offset = 7;
		break;
	case 10:
		gr->map_row_offset = 6;
		break;
	case 7:
	case 5:
		gr->map_row_offset = 1;
		break;
	default:
		break;
	}

	if (gr->map_tiles) {
		if (gr->map_tile_count != gr->tpc_count)
			delete_map = true;

		for (tile_count = 0; tile_count < gr->map_tile_count; tile_count++) {
			if ((u32)gr->map_tiles[tile_count] >= gr->tpc_count)
				delete_map = true;
		}

		if (delete_map) {
			kfree(gr->map_tiles);
			gr->map_tiles = NULL;
			gr->map_tile_count = 0;
		}
	}

	if (gr->map_tiles == NULL) {
		gr->map_tile_count = proj_scal_max_gpcs_v();

		gr->map_tiles = kzalloc(proj_scal_max_gpcs_v() * sizeof(u8), GFP_KERNEL);
		if (gr->map_tiles == NULL) {
			ret = -ENOMEM;
			goto clean_up;
		}

		for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++) {
			sorted_num_tpcs[gpc_index] = gr->gpc_tpc_count[gpc_index];
			sorted_to_unsorted_gpc_map[gpc_index] = gpc_index;
		}

		gpc_sorted = false;
		while (!gpc_sorted) {
			gpc_sorted = true;
			for (gpc_index = 0; gpc_index < gr->gpc_count - 1; gpc_index++) {
				if (sorted_num_tpcs[gpc_index + 1] > sorted_num_tpcs[gpc_index]) {
					gpc_sorted = false;
					swap = sorted_num_tpcs[gpc_index];
					sorted_num_tpcs[gpc_index] = sorted_num_tpcs[gpc_index + 1];
					sorted_num_tpcs[gpc_index + 1] = swap;
					swap = sorted_to_unsorted_gpc_map[gpc_index];
					sorted_to_unsorted_gpc_map[gpc_index] =
						sorted_to_unsorted_gpc_map[gpc_index + 1];
					sorted_to_unsorted_gpc_map[gpc_index + 1] = swap;
				}
			}
		}

		for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++)
			if (gr->gpc_tpc_count[gpc_index] > max_tpc_count)
				max_tpc_count = gr->gpc_tpc_count[gpc_index];

		mul_factor = gr->gpc_count * max_tpc_count;
		if (mul_factor & 0x1)
			mul_factor = 2;
		else
			mul_factor = 1;

		comm_denom = gr->gpc_count * max_tpc_count * mul_factor;

		for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++) {
			num_tpc = sorted_num_tpcs[gpc_index];

			init_frac[gpc_index] = num_tpc * gr->gpc_count * mul_factor;

			if (num_tpc != 0)
				init_err[gpc_index] = gpc_index * max_tpc_count * mul_factor - comm_denom/2;
			else
				init_err[gpc_index] = 0;

			run_err[gpc_index] = init_frac[gpc_index] + init_err[gpc_index];
		}

		while (gpc_mark < gr->tpc_count) {
			for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++) {
				if ((run_err[gpc_index] * 2) >= comm_denom) {
					gr->map_tiles[gpc_mark++] = (u8)sorted_to_unsorted_gpc_map[gpc_index];
					run_err[gpc_index] += init_frac[gpc_index] - comm_denom;
				} else
					run_err[gpc_index] += init_frac[gpc_index];
			}
		}
	}

clean_up:
	kfree(init_frac);
	kfree(init_err);
	kfree(run_err);
	kfree(sorted_num_tpcs);
	kfree(sorted_to_unsorted_gpc_map);

	if (ret)
		nvhost_dbg(dbg_fn | dbg_err, "fail");
	else
		nvhost_dbg_fn("done");

	return ret;
}

static int gr_gk20a_init_comptag(struct gk20a *g, struct gr_gk20a *gr)
{
	struct mem_mgr *memmgr = mem_mgr_from_g(g);

	/* max memory size (MB) to cover */
	u32 max_size = gr->max_comptag_mem;
	/* one tag line covers 128KB */
	u32 max_comptag_lines = max_size << 3;

	u32 hw_max_comptag_lines =
		ltc_ltcs_ltss_cbc_ctrl3_clear_upper_bound_init_v();

	u32 cbc_param =
		gk20a_readl(g, ltc_ltcs_ltss_cbc_param_r());
	u32 comptags_per_cacheline =
		ltc_ltcs_ltss_cbc_param_comptags_per_cache_line_v(cbc_param);
	u32 slices_per_fbp =
		ltc_ltcs_ltss_cbc_param_slices_per_fbp_v(cbc_param);
	u32 cacheline_size =
		512 << ltc_ltcs_ltss_cbc_param_cache_line_size_v(cbc_param);

	u32 compbit_backing_size;
	int ret = 0;

	nvhost_dbg_fn("");

	if (max_comptag_lines == 0) {
		gr->compbit_store.mem.size = 0;
		return 0;
	}

	if (max_comptag_lines > hw_max_comptag_lines)
		max_comptag_lines = hw_max_comptag_lines;

	/* no hybird fb */
	compbit_backing_size =
		DIV_ROUND_UP(max_comptag_lines, comptags_per_cacheline) *
		cacheline_size * slices_per_fbp * gr->num_fbps;

	/* aligned to 2KB * num_fbps */
	compbit_backing_size +=
		gr->num_fbps << ltc_ltc0_lts0_cbc_base_alignment_shift_v();

	/* must be a multiple of 64KB */
	compbit_backing_size = roundup(compbit_backing_size, 64*1024);

	max_comptag_lines =
		(compbit_backing_size * comptags_per_cacheline) /
		cacheline_size * slices_per_fbp * gr->num_fbps;

	if (max_comptag_lines > hw_max_comptag_lines)
		max_comptag_lines = hw_max_comptag_lines;

	nvhost_dbg_info("compbit backing store size : %d",
		compbit_backing_size);
	nvhost_dbg_info("max comptag lines : %d",
		max_comptag_lines);

	gr->compbit_store.mem.ref =
		nvhost_memmgr_alloc(memmgr, compbit_backing_size,
				    DEFAULT_ALLOC_ALIGNMENT,
				    DEFAULT_ALLOC_FLAGS,
				    0);
	if (IS_ERR(gr->compbit_store.mem.ref)) {
		nvhost_err(dev_from_gk20a(g), "failed to allocate"
			   "backing store for compbit : size %d",
			   compbit_backing_size);
		return PTR_ERR(gr->compbit_store.mem.ref);
	}
	gr->compbit_store.mem.size = compbit_backing_size;

	gr->compbit_store.mem.sgt =
		nvhost_memmgr_pin(memmgr, gr->compbit_store.mem.ref,
				dev_from_gk20a(g));
	if (IS_ERR(gr->compbit_store.mem.sgt)) {
		ret = PTR_ERR(gr->compbit_store.mem.sgt);
		goto clean_up;
	}
	gr->compbit_store.base_pa =
		gk20a_mm_iova_addr(gr->compbit_store.mem.sgt->sgl);

	nvhost_allocator_init(&gr->comp_tags, "comptag",
			      1, /* start */
			      max_comptag_lines - 1, /* length*/
			      1); /* align */

	return 0;

clean_up:
	if (gr->compbit_store.mem.sgt)
		nvhost_memmgr_free_sg_table(memmgr, gr->compbit_store.mem.ref,
				gr->compbit_store.mem.sgt);
	nvhost_memmgr_put(memmgr, gr->compbit_store.mem.ref);
	return ret;
}

int gk20a_gr_clear_comptags(struct gk20a *g, u32 min, u32 max)
{
	struct gr_gk20a *gr = &g->gr;
	u32 fbp, slice, ctrl1, val;
	unsigned long end_jiffies = jiffies +
		msecs_to_jiffies(gk20a_get_gr_idle_timeout(g));
	u32 delay = GR_IDLE_CHECK_DEFAULT;
	u32 slices_per_fbp =
		ltc_ltcs_ltss_cbc_param_slices_per_fbp_v(
			gk20a_readl(g, ltc_ltcs_ltss_cbc_param_r()));

	nvhost_dbg_fn("");

	if (gr->compbit_store.mem.size == 0)
		return 0;

	gk20a_writel(g, ltc_ltcs_ltss_cbc_ctrl2_r(),
		     ltc_ltcs_ltss_cbc_ctrl2_clear_lower_bound_f(min));
	gk20a_writel(g, ltc_ltcs_ltss_cbc_ctrl3_r(),
		     ltc_ltcs_ltss_cbc_ctrl3_clear_upper_bound_f(max));
	gk20a_writel(g, ltc_ltcs_ltss_cbc_ctrl1_r(),
		     gk20a_readl(g, ltc_ltcs_ltss_cbc_ctrl1_r()) |
		     ltc_ltcs_ltss_cbc_ctrl1_clear_active_f());

	for (fbp = 0; fbp < gr->num_fbps; fbp++) {
		for (slice = 0; slice < slices_per_fbp; slice++) {

			delay = GR_IDLE_CHECK_DEFAULT;

			ctrl1 = ltc_ltc0_lts0_cbc_ctrl1_r() +
				fbp * proj_ltc_pri_stride_v() +
				slice * proj_lts_pri_stride_v();

			do {
				val = gk20a_readl(g, ctrl1);
				if (ltc_ltc0_lts0_cbc_ctrl1_clear_v(val) !=
				    ltc_ltc0_lts0_cbc_ctrl1_clear_active_v())
					break;

				usleep_range(delay, delay * 2);
				delay = min_t(u32, delay << 1,
					GR_IDLE_CHECK_MAX);

			} while (time_before(jiffies, end_jiffies) |
					!tegra_platform_is_silicon());

			if (!time_before(jiffies, end_jiffies)) {
				nvhost_err(dev_from_gk20a(g),
					   "comp tag clear timeout\n");
				return -EBUSY;
			}
		}
	}

	return 0;
}

static int gr_gk20a_init_zcull(struct gk20a *g, struct gr_gk20a *gr)
{
	struct gr_zcull_gk20a *zcull = &gr->zcull;

	zcull->aliquot_width = gr->tpc_count * 16;
	zcull->aliquot_height = 16;

	zcull->width_align_pixels = gr->tpc_count * 16;
	zcull->height_align_pixels = 32;

	zcull->aliquot_size =
		zcull->aliquot_width * zcull->aliquot_height;

	/* assume no floor sweeping since we only have 1 tpc in 1 gpc */
	zcull->pixel_squares_by_aliquots =
		gr->zcb_count * 16 * 16 * gr->tpc_count /
		(gr->gpc_count * gr->gpc_tpc_count[0]);

	zcull->total_aliquots =
		gr_gpc0_zcull_total_ram_size_num_aliquots_f(
			gk20a_readl(g, gr_gpc0_zcull_total_ram_size_r()));

	return 0;
}

u32 gr_gk20a_get_ctxsw_zcull_size(struct gk20a *g, struct gr_gk20a *gr)
{
	/* assuming gr has already been initialized */
	return gr->ctx_vars.zcull_ctxsw_image_size;
}

int gr_gk20a_bind_ctxsw_zcull(struct gk20a *g, struct gr_gk20a *gr,
			struct channel_gk20a *c, u64 zcull_va, u32 mode)
{
	struct zcull_ctx_desc *zcull_ctx = &c->ch_ctx.zcull_ctx;

	zcull_ctx->ctx_sw_mode = mode;
	zcull_ctx->gpu_va = zcull_va;

	/* TBD: don't disable channel in sw method processing */
	return gr_gk20a_ctx_zcull_setup(g, c, true);
}

int gr_gk20a_get_zcull_info(struct gk20a *g, struct gr_gk20a *gr,
			struct gr_zcull_info *zcull_params)
{
	struct gr_zcull_gk20a *zcull = &gr->zcull;

	zcull_params->width_align_pixels = zcull->width_align_pixels;
	zcull_params->height_align_pixels = zcull->height_align_pixels;
	zcull_params->pixel_squares_by_aliquots =
		zcull->pixel_squares_by_aliquots;
	zcull_params->aliquot_total = zcull->total_aliquots;

	zcull_params->region_byte_multiplier =
		gr->gpc_count * gr_zcull_bytes_per_aliquot_per_gpu_v();
	zcull_params->region_header_size =
		proj_scal_litter_num_gpcs_v() *
		gr_zcull_save_restore_header_bytes_per_gpc_v();

	zcull_params->subregion_header_size =
		proj_scal_litter_num_gpcs_v() *
		gr_zcull_save_restore_subregion_header_bytes_per_gpc_v();

	zcull_params->subregion_width_align_pixels =
		gr->tpc_count * gr_gpc0_zcull_zcsize_width_subregion__multiple_v();
	zcull_params->subregion_height_align_pixels =
		gr_gpc0_zcull_zcsize_height_subregion__multiple_v();
	zcull_params->subregion_count = gr_zcull_subregion_qty_v();

	return 0;
}

static int gr_gk20a_add_zbc_color(struct gk20a *g, struct gr_gk20a *gr,
				struct zbc_entry *color_val, u32 index)
{
	struct fifo_gk20a *f = &g->fifo;
	struct fifo_engine_info_gk20a *gr_info = f->engine_info + ENGINE_GR_GK20A;
	u32 i;
	unsigned long end_jiffies = jiffies +
		msecs_to_jiffies(gk20a_get_gr_idle_timeout(g));
	u32 ret;

	ret = gk20a_fifo_disable_engine_activity(g, gr_info, true);
	if (ret) {
		nvhost_err(dev_from_gk20a(g),
			"failed to disable gr engine activity\n");
		return ret;
	}

	ret = gr_gk20a_wait_idle(g, end_jiffies, GR_IDLE_CHECK_DEFAULT);
	if (ret) {
		nvhost_err(dev_from_gk20a(g),
			"failed to idle graphics\n");
		goto clean_up;
	}

	/* update l2 table */
	gk20a_writel(g, ltc_ltcs_ltss_dstg_zbc_index_r(),
			(gk20a_readl(g, ltc_ltcs_ltss_dstg_zbc_index_r()) &
			 ~ltc_ltcs_ltss_dstg_zbc_index_address_f(~0)) |
				ltc_ltcs_ltss_dstg_zbc_index_address_f(index +
					GK20A_STARTOF_ZBC_TABLE));

	for (i = 0; i < ltc_ltcs_ltss_dstg_zbc_color_clear_value__size_1_v(); i++)
		gk20a_writel(g, ltc_ltcs_ltss_dstg_zbc_color_clear_value_r(i),
			color_val->color_l2[i]);

	/* update ds table */
	gk20a_writel(g, gr_ds_zbc_color_r_r(),
		gr_ds_zbc_color_r_val_f(color_val->color_ds[0]));
	gk20a_writel(g, gr_ds_zbc_color_g_r(),
		gr_ds_zbc_color_g_val_f(color_val->color_ds[1]));
	gk20a_writel(g, gr_ds_zbc_color_b_r(),
		gr_ds_zbc_color_b_val_f(color_val->color_ds[2]));
	gk20a_writel(g, gr_ds_zbc_color_a_r(),
		gr_ds_zbc_color_a_val_f(color_val->color_ds[3]));

	gk20a_writel(g, gr_ds_zbc_color_fmt_r(),
		gr_ds_zbc_color_fmt_val_f(color_val->format));

	gk20a_writel(g, gr_ds_zbc_tbl_index_r(),
		gr_ds_zbc_tbl_index_val_f(index + GK20A_STARTOF_ZBC_TABLE));

	/* trigger the write */
	gk20a_writel(g, gr_ds_zbc_tbl_ld_r(),
		gr_ds_zbc_tbl_ld_select_c_f() |
		gr_ds_zbc_tbl_ld_action_write_f() |
		gr_ds_zbc_tbl_ld_trigger_active_f());

	/* update local copy */
	for (i = 0; i < ltc_ltcs_ltss_dstg_zbc_color_clear_value__size_1_v(); i++) {
		gr->zbc_col_tbl[index].color_l2[i] = color_val->color_l2[i];
		gr->zbc_col_tbl[index].color_ds[i] = color_val->color_ds[i];
	}
	gr->zbc_col_tbl[index].format = color_val->format;
	gr->zbc_col_tbl[index].ref_cnt++;

clean_up:
	ret = gk20a_fifo_enable_engine_activity(g, gr_info);
	if (ret) {
		nvhost_err(dev_from_gk20a(g),
			"failed to enable gr engine activity\n");
	}

	return ret;
}

static int gr_gk20a_add_zbc_depth(struct gk20a *g, struct gr_gk20a *gr,
				struct zbc_entry *depth_val, u32 index)
{
	struct fifo_gk20a *f = &g->fifo;
	struct fifo_engine_info_gk20a *gr_info = f->engine_info + ENGINE_GR_GK20A;
	unsigned long end_jiffies = jiffies +
		msecs_to_jiffies(gk20a_get_gr_idle_timeout(g));
	u32 ret;

	ret = gk20a_fifo_disable_engine_activity(g, gr_info, true);
	if (ret) {
		nvhost_err(dev_from_gk20a(g),
			"failed to disable gr engine activity\n");
		return ret;
	}

	ret = gr_gk20a_wait_idle(g, end_jiffies, GR_IDLE_CHECK_DEFAULT);
	if (ret) {
		nvhost_err(dev_from_gk20a(g),
			"failed to idle graphics\n");
		goto clean_up;
	}

	/* update l2 table */
	gk20a_writel(g, ltc_ltcs_ltss_dstg_zbc_index_r(),
			(gk20a_readl(g, ltc_ltcs_ltss_dstg_zbc_index_r()) &
			 ~ltc_ltcs_ltss_dstg_zbc_index_address_f(~0)) |
				ltc_ltcs_ltss_dstg_zbc_index_address_f(index +
					GK20A_STARTOF_ZBC_TABLE));

	gk20a_writel(g, ltc_ltcs_ltss_dstg_zbc_depth_clear_value_r(),
			depth_val->depth);

	/* update ds table */
	gk20a_writel(g, gr_ds_zbc_z_r(),
		gr_ds_zbc_z_val_f(depth_val->depth));

	gk20a_writel(g, gr_ds_zbc_z_fmt_r(),
		gr_ds_zbc_z_fmt_val_f(depth_val->format));

	gk20a_writel(g, gr_ds_zbc_tbl_index_r(),
		gr_ds_zbc_tbl_index_val_f(index + GK20A_STARTOF_ZBC_TABLE));

	/* trigger the write */
	gk20a_writel(g, gr_ds_zbc_tbl_ld_r(),
		gr_ds_zbc_tbl_ld_select_z_f() |
		gr_ds_zbc_tbl_ld_action_write_f() |
		gr_ds_zbc_tbl_ld_trigger_active_f());

	/* update local copy */
	gr->zbc_dep_tbl[index].depth = depth_val->depth;
	gr->zbc_dep_tbl[index].format = depth_val->format;
	gr->zbc_dep_tbl[index].ref_cnt++;

clean_up:
	ret = gk20a_fifo_enable_engine_activity(g, gr_info);
	if (ret) {
		nvhost_err(dev_from_gk20a(g),
			"failed to enable gr engine activity\n");
	}

	return ret;
}

int gr_gk20a_add_zbc(struct gk20a *g, struct gr_gk20a *gr,
		     struct zbc_entry *zbc_val)
{
	struct zbc_color_table *c_tbl;
	struct zbc_depth_table *d_tbl;
	u32 i, ret = -ENOMEM;
	bool added = false;

	/* no endian swap ? */

	switch (zbc_val->type) {
	case GK20A_ZBC_TYPE_COLOR:
		/* search existing tables */
		for (i = 0; i < gr->max_used_color_index; i++) {

			c_tbl = &gr->zbc_col_tbl[i];

			if (c_tbl->ref_cnt && c_tbl->format == zbc_val->format &&
			    memcmp(c_tbl->color_ds, zbc_val->color_ds,
				sizeof(zbc_val->color_ds)) == 0) {

				if (memcmp(c_tbl->color_l2, zbc_val->color_l2,
				    sizeof(zbc_val->color_l2))) {
					nvhost_err(dev_from_gk20a(g),
						"zbc l2 and ds color don't match with existing entries");
					return -EINVAL;
				}
				added = true;
				c_tbl->ref_cnt++;
				ret = 0;
				break;
			}
		}
		/* add new table */
		if (!added &&
		    gr->max_used_color_index < GK20A_ZBC_TABLE_SIZE) {

			c_tbl =
			    &gr->zbc_col_tbl[gr->max_used_color_index];
			WARN_ON(c_tbl->ref_cnt != 0);

			ret = gr_gk20a_add_zbc_color(g, gr,
				zbc_val, gr->max_used_color_index);

			if (!ret)
				gr->max_used_color_index++;
		}
		break;
	case GK20A_ZBC_TYPE_DEPTH:
		/* search existing tables */
		for (i = 0; i < gr->max_used_depth_index; i++) {

			d_tbl = &gr->zbc_dep_tbl[i];

			if (d_tbl->ref_cnt &&
			    d_tbl->depth == zbc_val->depth &&
			    d_tbl->format == zbc_val->format) {
				added = true;
				d_tbl->ref_cnt++;
				ret = 0;
				break;
			}
		}
		/* add new table */
		if (!added &&
		    gr->max_used_depth_index < GK20A_ZBC_TABLE_SIZE) {

			d_tbl =
			    &gr->zbc_dep_tbl[gr->max_used_depth_index];
			WARN_ON(d_tbl->ref_cnt != 0);

			ret = gr_gk20a_add_zbc_depth(g, gr,
				zbc_val, gr->max_used_depth_index);

			if (!ret)
				gr->max_used_depth_index++;
		}
		break;
	default:
		nvhost_err(dev_from_gk20a(g),
			"invalid zbc table type %d", zbc_val->type);
		return -EINVAL;
	}

	if (added && ret == 0) {
		/* update zbc for elpg */
	}

	return ret;
}

int gr_gk20a_clear_zbc_table(struct gk20a *g, struct gr_gk20a *gr)
{
	struct fifo_gk20a *f = &g->fifo;
	struct fifo_engine_info_gk20a *gr_info = f->engine_info + ENGINE_GR_GK20A;
	u32 i, j;
	unsigned long end_jiffies = jiffies +
		msecs_to_jiffies(gk20a_get_gr_idle_timeout(g));
	u32 ret;

	ret = gk20a_fifo_disable_engine_activity(g, gr_info, true);
	if (ret) {
		nvhost_err(dev_from_gk20a(g),
			"failed to disable gr engine activity\n");
		return ret;
	}

	ret = gr_gk20a_wait_idle(g, end_jiffies, GR_IDLE_CHECK_DEFAULT);
	if (ret) {
		nvhost_err(dev_from_gk20a(g),
			"failed to idle graphics\n");
		goto clean_up;
	}

	for (i = 0; i < GK20A_ZBC_TABLE_SIZE; i++) {
		gr->zbc_col_tbl[i].format = 0;
		gr->zbc_col_tbl[i].ref_cnt = 0;

		gk20a_writel(g, gr_ds_zbc_color_fmt_r(),
			gr_ds_zbc_color_fmt_val_invalid_f());
		gk20a_writel(g, gr_ds_zbc_tbl_index_r(),
			gr_ds_zbc_tbl_index_val_f(i + GK20A_STARTOF_ZBC_TABLE));

		/* trigger the write */
		gk20a_writel(g, gr_ds_zbc_tbl_ld_r(),
			gr_ds_zbc_tbl_ld_select_c_f() |
			gr_ds_zbc_tbl_ld_action_write_f() |
			gr_ds_zbc_tbl_ld_trigger_active_f());

		/* clear l2 table */
		gk20a_writel(g, ltc_ltcs_ltss_dstg_zbc_index_r(),
			(gk20a_readl(g, ltc_ltcs_ltss_dstg_zbc_index_r()) &
			 ~ltc_ltcs_ltss_dstg_zbc_index_address_f(~0)) |
				ltc_ltcs_ltss_dstg_zbc_index_address_f(i +
					GK20A_STARTOF_ZBC_TABLE));

		for (j = 0; j < ltc_ltcs_ltss_dstg_zbc_color_clear_value__size_1_v(); j++) {
			gk20a_writel(g, ltc_ltcs_ltss_dstg_zbc_color_clear_value_r(j), 0);
			gr->zbc_col_tbl[i].color_l2[j] = 0;
			gr->zbc_col_tbl[i].color_ds[j] = 0;
		}
	}
	gr->max_used_color_index = 0;
	gr->max_default_color_index = 0;

	for (i = 0; i < GK20A_ZBC_TABLE_SIZE; i++) {
		gr->zbc_dep_tbl[i].depth = 0;
		gr->zbc_dep_tbl[i].format = 0;
		gr->zbc_dep_tbl[i].ref_cnt = 0;

		gk20a_writel(g, gr_ds_zbc_z_fmt_r(),
			gr_ds_zbc_z_fmt_val_invalid_f());
		gk20a_writel(g, gr_ds_zbc_tbl_index_r(),
			gr_ds_zbc_tbl_index_val_f(i + GK20A_STARTOF_ZBC_TABLE));

		/* trigger the write */
		gk20a_writel(g, gr_ds_zbc_tbl_ld_r(),
			gr_ds_zbc_tbl_ld_select_z_f() |
			gr_ds_zbc_tbl_ld_action_write_f() |
			gr_ds_zbc_tbl_ld_trigger_active_f());

		/* clear l2 table */
		gk20a_writel(g, ltc_ltcs_ltss_dstg_zbc_index_r(),
			(gk20a_readl(g, ltc_ltcs_ltss_dstg_zbc_index_r()) &
			 ~ltc_ltcs_ltss_dstg_zbc_index_address_f(~0)) |
				ltc_ltcs_ltss_dstg_zbc_index_address_f(i +
					GK20A_STARTOF_ZBC_TABLE));

		gk20a_writel(g, ltc_ltcs_ltss_dstg_zbc_depth_clear_value_r(), 0);
	}
	gr->max_used_depth_index = 0;
	gr->max_default_depth_index = 0;

clean_up:
	ret = gk20a_fifo_enable_engine_activity(g, gr_info);
	if (ret) {
		nvhost_err(dev_from_gk20a(g),
			"failed to enable gr engine activity\n");
	}

	/* elpg stuff */

	return ret;
}

/* get a zbc table entry specified by index
 * return table size when type is invalid */
int gr_gk20a_query_zbc(struct gk20a *g, struct gr_gk20a *gr,
			struct zbc_query_params *query_params)
{
	u32 index = query_params->index_size;
	u32 i;

	switch (query_params->type) {
	case GK20A_ZBC_TYPE_INVALID:
		query_params->index_size = GK20A_ZBC_TABLE_SIZE;
		break;
	case GK20A_ZBC_TYPE_COLOR:
		if (index >= GK20A_ZBC_TABLE_SIZE) {
			nvhost_err(dev_from_gk20a(g),
				"invalid zbc color table index\n");
			return -EINVAL;
		}
		for (i = 0; i < GK20A_ZBC_COLOR_VALUE_SIZE; i++) {
			query_params->color_l2[i] =
				gr->zbc_col_tbl[index].color_l2[i];
			query_params->color_ds[i] =
				gr->zbc_col_tbl[index].color_ds[i];
		}
		query_params->format = gr->zbc_col_tbl[index].format;
		query_params->ref_cnt = gr->zbc_col_tbl[index].ref_cnt;
		break;
	case GK20A_ZBC_TYPE_DEPTH:
		if (index >= GK20A_ZBC_TABLE_SIZE) {
			nvhost_err(dev_from_gk20a(g),
				"invalid zbc depth table index\n");
			return -EINVAL;
		}
		query_params->depth = gr->zbc_dep_tbl[index].depth;
		query_params->format = gr->zbc_dep_tbl[index].format;
		query_params->ref_cnt = gr->zbc_dep_tbl[index].ref_cnt;
		break;
	default:
		nvhost_err(dev_from_gk20a(g),
				"invalid zbc table type\n");
		return -EINVAL;
	}

	return 0;
}

static int gr_gk20a_load_zbc_default_table(struct gk20a *g, struct gr_gk20a *gr)
{
	struct zbc_entry zbc_val;
	u32 i, err;

	/* load default color table */
	zbc_val.type = GK20A_ZBC_TYPE_COLOR;

	zbc_val.format = gr_ds_zbc_color_fmt_val_zero_v();
	for (i = 0; i < GK20A_ZBC_COLOR_VALUE_SIZE; i++) {
		zbc_val.color_ds[i] = 0;
		zbc_val.color_l2[i] = 0;
	}
	err = gr_gk20a_add_zbc(g, gr, &zbc_val);

	zbc_val.format = gr_ds_zbc_color_fmt_val_unorm_one_v();
	for (i = 0; i < GK20A_ZBC_COLOR_VALUE_SIZE; i++) {
		zbc_val.color_ds[i] = 0xffffffff;
		zbc_val.color_l2[i] = 0x3f800000;
	}
	err |= gr_gk20a_add_zbc(g, gr, &zbc_val);

	zbc_val.format = gr_ds_zbc_color_fmt_val_rf32_gf32_bf32_af32_v();
	for (i = 0; i < GK20A_ZBC_COLOR_VALUE_SIZE; i++) {
		zbc_val.color_ds[i] = 0;
		zbc_val.color_l2[i] = 0;
	}
	err |= gr_gk20a_add_zbc(g, gr, &zbc_val);

	zbc_val.format = gr_ds_zbc_color_fmt_val_rf32_gf32_bf32_af32_v();
	for (i = 0; i < GK20A_ZBC_COLOR_VALUE_SIZE; i++) {
		zbc_val.color_ds[i] = 0x3f800000;
		zbc_val.color_l2[i] = 0x3f800000;
	}
	err |= gr_gk20a_add_zbc(g, gr, &zbc_val);

	if (!err)
		gr->max_default_color_index = 4;
	else {
		nvhost_err(dev_from_gk20a(g),
			   "fail to load default zbc color table\n");
		return err;
	}

	/* load default depth table */
	zbc_val.type = GK20A_ZBC_TYPE_DEPTH;

	zbc_val.format = gr_ds_zbc_z_fmt_val_fp32_v();
	zbc_val.depth = 0;
	err = gr_gk20a_add_zbc(g, gr, &zbc_val);

	zbc_val.format = gr_ds_zbc_z_fmt_val_fp32_v();
	zbc_val.depth = 0x3f800000;
	err |= gr_gk20a_add_zbc(g, gr, &zbc_val);

	if (!err)
		gr->max_default_depth_index = 2;
	else {
		nvhost_err(dev_from_gk20a(g),
			   "fail to load default zbc depth table\n");
		return err;
	}

	return 0;
}

static int gr_gk20a_init_zbc(struct gk20a *g, struct gr_gk20a *gr)
{
	u32 i, j;

	/* reset zbc clear */
	for (i = 0; i < GK20A_SIZEOF_ZBC_TABLE -
	    GK20A_STARTOF_ZBC_TABLE; i++) {
		gk20a_writel(g, ltc_ltcs_ltss_dstg_zbc_index_r(),
			(gk20a_readl(g, ltc_ltcs_ltss_dstg_zbc_index_r()) &
			 ~ltc_ltcs_ltss_dstg_zbc_index_address_f(~0)) |
				ltc_ltcs_ltss_dstg_zbc_index_address_f(
					i + GK20A_STARTOF_ZBC_TABLE));
		for (j = 0; j < ltc_ltcs_ltss_dstg_zbc_color_clear_value__size_1_v(); j++)
			gk20a_writel(g, ltc_ltcs_ltss_dstg_zbc_color_clear_value_r(j), 0);
		gk20a_writel(g, ltc_ltcs_ltss_dstg_zbc_depth_clear_value_r(), 0);
	}

	gr_gk20a_clear_zbc_table(g, gr);

	gr_gk20a_load_zbc_default_table(g, gr);

	return 0;
}

int gk20a_gr_zbc_set_table(struct gk20a *g, struct gr_gk20a *gr,
			struct zbc_entry *zbc_val)
{
	nvhost_dbg_fn("");

	return gr_gk20a_elpg_protected_call(g,
		gr_gk20a_add_zbc(g, gr, zbc_val));
}

void gr_gk20a_init_elcg_mode(struct gk20a *g, u32 mode, u32 engine)
{
	u32 gate_ctrl, idle_filter;

	gate_ctrl = gk20a_readl(g, therm_gate_ctrl_r(engine));

	switch (mode) {
	case ELCG_RUN:
		gate_ctrl = set_field(gate_ctrl,
				therm_gate_ctrl_eng_clk_m(),
				therm_gate_ctrl_eng_clk_run_f());
		gate_ctrl = set_field(gate_ctrl,
				therm_gate_ctrl_eng_pwr_m(),
				/* set elpg to auto to meet hw expectation */
				therm_gate_ctrl_eng_pwr_auto_f());
		break;
	case ELCG_STOP:
		gate_ctrl = set_field(gate_ctrl,
				therm_gate_ctrl_eng_clk_m(),
				therm_gate_ctrl_eng_clk_stop_f());
		break;
	case ELCG_AUTO:
		gate_ctrl = set_field(gate_ctrl,
				therm_gate_ctrl_eng_clk_m(),
				therm_gate_ctrl_eng_clk_auto_f());
		break;
	default:
		nvhost_err(dev_from_gk20a(g),
			"invalid elcg mode %d", mode);
	}

	if (tegra_platform_is_linsim()) {
		gate_ctrl = set_field(gate_ctrl,
			therm_gate_ctrl_eng_delay_after_m(),
			therm_gate_ctrl_eng_delay_after_f(4));
	}

	/* 2 * (1 << 9) = 1024 clks */
	gate_ctrl = set_field(gate_ctrl,
		therm_gate_ctrl_eng_idle_filt_exp_m(),
		therm_gate_ctrl_eng_idle_filt_exp_f(9));
	gate_ctrl = set_field(gate_ctrl,
		therm_gate_ctrl_eng_idle_filt_mant_m(),
		therm_gate_ctrl_eng_idle_filt_mant_f(2));
	gk20a_writel(g, therm_gate_ctrl_r(engine), gate_ctrl);

	/* default fecs_idle_filter to 0 */
	idle_filter = gk20a_readl(g, therm_fecs_idle_filter_r());
	idle_filter &= ~therm_fecs_idle_filter_value_m();
	gk20a_writel(g, therm_fecs_idle_filter_r(), idle_filter);
	/* default hubmmu_idle_filter to 0 */
	idle_filter = gk20a_readl(g, therm_hubmmu_idle_filter_r());
	idle_filter &= ~therm_hubmmu_idle_filter_value_m();
	gk20a_writel(g, therm_hubmmu_idle_filter_r(), idle_filter);
}

static int gr_gk20a_zcull_init_hw(struct gk20a *g, struct gr_gk20a *gr)
{
	u32 gpc_index, gpc_tpc_count, gpc_zcull_count;
	u32 *zcull_map_tiles, *zcull_bank_counters;
	u32 map_counter;
	u32 rcp_conserv;
	u32 offset;
	bool floorsweep = false;

	if (!gr->map_tiles)
		return -1;

	zcull_map_tiles = kzalloc(proj_scal_max_gpcs_v() *
			proj_scal_max_tpc_per_gpc_v() * sizeof(u32), GFP_KERNEL);
	zcull_bank_counters = kzalloc(proj_scal_max_gpcs_v() *
			proj_scal_max_tpc_per_gpc_v() * sizeof(u32), GFP_KERNEL);

	if (!zcull_map_tiles && !zcull_bank_counters) {
		nvhost_err(dev_from_gk20a(g),
			"failed to allocate zcull temp buffers");
		return -ENOMEM;
	}

	for (map_counter = 0; map_counter < gr->tpc_count; map_counter++) {
		zcull_map_tiles[map_counter] =
			zcull_bank_counters[gr->map_tiles[map_counter]];
		zcull_bank_counters[gr->map_tiles[map_counter]]++;
	}

	gk20a_writel(g, gr_gpcs_zcull_sm_in_gpc_number_map0_r(),
		gr_gpcs_zcull_sm_in_gpc_number_map0_tile_0_f(zcull_map_tiles[0]) |
		gr_gpcs_zcull_sm_in_gpc_number_map0_tile_1_f(zcull_map_tiles[1]) |
		gr_gpcs_zcull_sm_in_gpc_number_map0_tile_2_f(zcull_map_tiles[2]) |
		gr_gpcs_zcull_sm_in_gpc_number_map0_tile_3_f(zcull_map_tiles[3]) |
		gr_gpcs_zcull_sm_in_gpc_number_map0_tile_4_f(zcull_map_tiles[4]) |
		gr_gpcs_zcull_sm_in_gpc_number_map0_tile_5_f(zcull_map_tiles[5]) |
		gr_gpcs_zcull_sm_in_gpc_number_map0_tile_6_f(zcull_map_tiles[6]) |
		gr_gpcs_zcull_sm_in_gpc_number_map0_tile_7_f(zcull_map_tiles[7]));

	gk20a_writel(g, gr_gpcs_zcull_sm_in_gpc_number_map1_r(),
		gr_gpcs_zcull_sm_in_gpc_number_map1_tile_8_f(zcull_map_tiles[8]) |
		gr_gpcs_zcull_sm_in_gpc_number_map1_tile_9_f(zcull_map_tiles[9]) |
		gr_gpcs_zcull_sm_in_gpc_number_map1_tile_10_f(zcull_map_tiles[10]) |
		gr_gpcs_zcull_sm_in_gpc_number_map1_tile_11_f(zcull_map_tiles[11]) |
		gr_gpcs_zcull_sm_in_gpc_number_map1_tile_12_f(zcull_map_tiles[12]) |
		gr_gpcs_zcull_sm_in_gpc_number_map1_tile_13_f(zcull_map_tiles[13]) |
		gr_gpcs_zcull_sm_in_gpc_number_map1_tile_14_f(zcull_map_tiles[14]) |
		gr_gpcs_zcull_sm_in_gpc_number_map1_tile_15_f(zcull_map_tiles[15]));

	gk20a_writel(g, gr_gpcs_zcull_sm_in_gpc_number_map2_r(),
		gr_gpcs_zcull_sm_in_gpc_number_map2_tile_16_f(zcull_map_tiles[16]) |
		gr_gpcs_zcull_sm_in_gpc_number_map2_tile_17_f(zcull_map_tiles[17]) |
		gr_gpcs_zcull_sm_in_gpc_number_map2_tile_18_f(zcull_map_tiles[18]) |
		gr_gpcs_zcull_sm_in_gpc_number_map2_tile_19_f(zcull_map_tiles[19]) |
		gr_gpcs_zcull_sm_in_gpc_number_map2_tile_20_f(zcull_map_tiles[20]) |
		gr_gpcs_zcull_sm_in_gpc_number_map2_tile_21_f(zcull_map_tiles[21]) |
		gr_gpcs_zcull_sm_in_gpc_number_map2_tile_22_f(zcull_map_tiles[22]) |
		gr_gpcs_zcull_sm_in_gpc_number_map2_tile_23_f(zcull_map_tiles[23]));

	gk20a_writel(g, gr_gpcs_zcull_sm_in_gpc_number_map3_r(),
		gr_gpcs_zcull_sm_in_gpc_number_map3_tile_24_f(zcull_map_tiles[24]) |
		gr_gpcs_zcull_sm_in_gpc_number_map3_tile_25_f(zcull_map_tiles[25]) |
		gr_gpcs_zcull_sm_in_gpc_number_map3_tile_26_f(zcull_map_tiles[26]) |
		gr_gpcs_zcull_sm_in_gpc_number_map3_tile_27_f(zcull_map_tiles[27]) |
		gr_gpcs_zcull_sm_in_gpc_number_map3_tile_28_f(zcull_map_tiles[28]) |
		gr_gpcs_zcull_sm_in_gpc_number_map3_tile_29_f(zcull_map_tiles[29]) |
		gr_gpcs_zcull_sm_in_gpc_number_map3_tile_30_f(zcull_map_tiles[30]) |
		gr_gpcs_zcull_sm_in_gpc_number_map3_tile_31_f(zcull_map_tiles[31]));

	kfree(zcull_map_tiles);
	kfree(zcull_bank_counters);

	for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++) {
		gpc_tpc_count = gr->gpc_tpc_count[gpc_index];
		gpc_zcull_count = gr->gpc_zcb_count[gpc_index];

		if (gpc_zcull_count != gr->max_zcull_per_gpc_count &&
		    gpc_zcull_count < gpc_tpc_count) {
			nvhost_err(dev_from_gk20a(g),
				"zcull_banks (%d) less than tpcs (%d) for gpc (%d)",
				gpc_zcull_count, gpc_tpc_count, gpc_index);
			return -EINVAL;
		}
		if (gpc_zcull_count != gr->max_zcull_per_gpc_count &&
		    gpc_zcull_count != 0)
			floorsweep = true;
	}

	/* 1.0f / 1.0f * gr_gpc0_zcull_sm_num_rcp_conservative__max_v() */
	rcp_conserv = gr_gpc0_zcull_sm_num_rcp_conservative__max_v();

	for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++) {
		offset = gpc_index * proj_gpc_stride_v();

		if (floorsweep) {
			gk20a_writel(g, gr_gpc0_zcull_ram_addr_r() + offset,
				gr_gpc0_zcull_ram_addr_row_offset_f(gr->map_row_offset) |
				gr_gpc0_zcull_ram_addr_tiles_per_hypertile_row_per_gpc_f(
					gr->max_zcull_per_gpc_count));
		} else {
			gk20a_writel(g, gr_gpc0_zcull_ram_addr_r() + offset,
				gr_gpc0_zcull_ram_addr_row_offset_f(gr->map_row_offset) |
				gr_gpc0_zcull_ram_addr_tiles_per_hypertile_row_per_gpc_f(
					gr->gpc_tpc_count[gpc_index]));
		}

		gk20a_writel(g, gr_gpc0_zcull_fs_r() + offset,
			gr_gpc0_zcull_fs_num_active_banks_f(gr->gpc_zcb_count[gpc_index]) |
			gr_gpc0_zcull_fs_num_sms_f(gr->tpc_count));

		gk20a_writel(g, gr_gpc0_zcull_sm_num_rcp_r() + offset,
			gr_gpc0_zcull_sm_num_rcp_conservative_f(rcp_conserv));
	}

	gk20a_writel(g, gr_gpcs_ppcs_wwdx_sm_num_rcp_r(),
		gr_gpcs_ppcs_wwdx_sm_num_rcp_conservative_f(rcp_conserv));

	return 0;
}

static int gk20a_init_gr_setup_hw(struct gk20a *g)
{
	struct gr_gk20a *gr = &g->gr;
	struct aiv_list_gk20a *sw_ctx_load = &g->gr.ctx_vars.sw_ctx_load;
	struct av_list_gk20a *sw_bundle_init = &g->gr.ctx_vars.sw_bundle_init;
	struct av_list_gk20a *sw_method_init = &g->gr.ctx_vars.sw_method_init;
	u32 data;
	u32 addr_lo, addr_hi, addr;
	u32 compbit_base_post_divide;
	u64 compbit_base_post_multiply64;
	unsigned long end_jiffies = jiffies +
		msecs_to_jiffies(gk20a_get_gr_idle_timeout(g));
	u32 fe_go_idle_timeout_save;
	u32 last_bundle_data = 0;
	u32 last_method_data = 0;
	u32 i, err;
	u32 l1c_dbg_reg_val;

	nvhost_dbg_fn("");

	/* slcg prod values */
	gr_gk20a_slcg_gr_load_gating_prod(g, g->slcg_enabled);
	gr_gk20a_slcg_perf_load_gating_prod(g, g->slcg_enabled);

	/* init mmu debug buffer */
	addr = gk20a_mm_iova_addr(gr->mmu_wr_mem.mem.sgt->sgl);
	addr_lo = u64_lo32(addr);
	addr_hi = u64_hi32(addr);
	addr = (addr_lo >> fb_mmu_debug_wr_addr_alignment_v()) |
		(addr_hi << (32 - fb_mmu_debug_wr_addr_alignment_v()));

	gk20a_writel(g, fb_mmu_debug_wr_r(),
		     fb_mmu_debug_wr_aperture_vid_mem_f() |
		     fb_mmu_debug_wr_vol_false_f() |
		     fb_mmu_debug_wr_addr_v(addr));

	addr = gk20a_mm_iova_addr(gr->mmu_rd_mem.mem.sgt->sgl);
	addr_lo = u64_lo32(addr);
	addr_hi = u64_hi32(addr);
	addr = (addr_lo >> fb_mmu_debug_rd_addr_alignment_v()) |
		(addr_hi << (32 - fb_mmu_debug_rd_addr_alignment_v()));

	gk20a_writel(g, fb_mmu_debug_rd_r(),
		     fb_mmu_debug_rd_aperture_vid_mem_f() |
		     fb_mmu_debug_rd_vol_false_f() |
		     fb_mmu_debug_rd_addr_v(addr));

	/* load gr floorsweeping registers */
	data = gk20a_readl(g, gr_gpc0_ppc0_pes_vsc_strem_r());
	data = set_field(data, gr_gpc0_ppc0_pes_vsc_strem_master_pe_m(),
			gr_gpc0_ppc0_pes_vsc_strem_master_pe_true_f());
	gk20a_writel(g, gr_gpc0_ppc0_pes_vsc_strem_r(), data);

	gr_gk20a_zcull_init_hw(g, gr);

	gr_gk20a_blcg_gr_load_gating_prod(g, g->blcg_enabled);
	gr_gk20a_pg_gr_load_gating_prod(g, true);

	if (g->elcg_enabled) {
		gr_gk20a_init_elcg_mode(g, ELCG_AUTO, ENGINE_GR_GK20A);
		gr_gk20a_init_elcg_mode(g, ELCG_AUTO, ENGINE_CE2_GK20A);
	} else {
		gr_gk20a_init_elcg_mode(g, ELCG_RUN, ENGINE_GR_GK20A);
		gr_gk20a_init_elcg_mode(g, ELCG_RUN, ENGINE_CE2_GK20A);
	}

	/* Bug 1340570: increase the clock timeout to avoid potential
	 * operation failure at high gpcclk rate. Default values are 0x400.
	 */
	gk20a_writel(g, pri_ringstation_sys_master_config_r(0x15), 0x800);
	gk20a_writel(g, pri_ringstation_gpc_master_config_r(0xa), 0x800);
	gk20a_writel(g, pri_ringstation_fbp_master_config_r(0x8), 0x800);

	/* enable fifo access */
	gk20a_writel(g, gr_gpfifo_ctl_r(),
		     gr_gpfifo_ctl_access_enabled_f() |
		     gr_gpfifo_ctl_semaphore_access_enabled_f());

	/* TBD: reload gr ucode when needed */

	/* enable interrupts */
	gk20a_writel(g, gr_intr_r(), 0xFFFFFFFF);
	gk20a_writel(g, gr_intr_en_r(), 0xFFFFFFFF);

	/* enable fecs error interrupts */
	gk20a_writel(g, gr_fecs_host_int_enable_r(),
		     gr_fecs_host_int_enable_fault_during_ctxsw_enable_f() |
		     gr_fecs_host_int_enable_umimp_firmware_method_enable_f() |
		     gr_fecs_host_int_enable_umimp_illegal_method_enable_f() |
		     gr_fecs_host_int_enable_watchdog_enable_f());

	/* enable exceptions */
	gk20a_writel(g, gr_fe_hww_esr_r(),
		     gr_fe_hww_esr_en_enable_f() |
		     gr_fe_hww_esr_reset_active_f());
	gk20a_writel(g, gr_memfmt_hww_esr_r(),
		     gr_memfmt_hww_esr_en_enable_f() |
		     gr_memfmt_hww_esr_reset_active_f());
	gk20a_writel(g, gr_scc_hww_esr_r(),
		     gr_scc_hww_esr_en_enable_f() |
		     gr_scc_hww_esr_reset_active_f());
	gk20a_writel(g, gr_mme_hww_esr_r(),
		     gr_mme_hww_esr_en_enable_f() |
		     gr_mme_hww_esr_reset_active_f());
	gk20a_writel(g, gr_pd_hww_esr_r(),
		     gr_pd_hww_esr_en_enable_f() |
		     gr_pd_hww_esr_reset_active_f());
	gk20a_writel(g, gr_sked_hww_esr_r(), /* enabled by default */
		     gr_sked_hww_esr_reset_active_f());
	gk20a_writel(g, gr_ds_hww_esr_r(),
		     gr_ds_hww_esr_en_enabled_f() |
		     gr_ds_hww_esr_reset_task_f());
	gk20a_writel(g, gr_ds_hww_report_mask_r(),
		     gr_ds_hww_report_mask_sph0_err_report_f() |
		     gr_ds_hww_report_mask_sph1_err_report_f() |
		     gr_ds_hww_report_mask_sph2_err_report_f() |
		     gr_ds_hww_report_mask_sph3_err_report_f() |
		     gr_ds_hww_report_mask_sph4_err_report_f() |
		     gr_ds_hww_report_mask_sph5_err_report_f() |
		     gr_ds_hww_report_mask_sph6_err_report_f() |
		     gr_ds_hww_report_mask_sph7_err_report_f() |
		     gr_ds_hww_report_mask_sph8_err_report_f() |
		     gr_ds_hww_report_mask_sph9_err_report_f() |
		     gr_ds_hww_report_mask_sph10_err_report_f() |
		     gr_ds_hww_report_mask_sph11_err_report_f() |
		     gr_ds_hww_report_mask_sph12_err_report_f() |
		     gr_ds_hww_report_mask_sph13_err_report_f() |
		     gr_ds_hww_report_mask_sph14_err_report_f() |
		     gr_ds_hww_report_mask_sph15_err_report_f() |
		     gr_ds_hww_report_mask_sph16_err_report_f() |
		     gr_ds_hww_report_mask_sph17_err_report_f() |
		     gr_ds_hww_report_mask_sph18_err_report_f() |
		     gr_ds_hww_report_mask_sph19_err_report_f() |
		     gr_ds_hww_report_mask_sph20_err_report_f() |
		     gr_ds_hww_report_mask_sph21_err_report_f() |
		     gr_ds_hww_report_mask_sph22_err_report_f() |
		     gr_ds_hww_report_mask_sph23_err_report_f());

	/* TBD: ECC for L1/SM */
	/* TBD: enable per GPC exceptions */
	/* TBD: enable per BE exceptions */

	/* reset and enable all exceptions */
	gk20a_writel(g, gr_exception_r(), 0xFFFFFFFF);
	gk20a_writel(g, gr_exception_en_r(), 0xFFFFFFFF);
	gk20a_writel(g, gr_exception1_r(), 0xFFFFFFFF);
	gk20a_writel(g, gr_exception1_en_r(), 0xFFFFFFFF);
	gk20a_writel(g, gr_exception2_r(), 0xFFFFFFFF);
	gk20a_writel(g, gr_exception2_en_r(), 0xFFFFFFFF);

	/* ignore status from some units */
	data = gk20a_readl(g, gr_status_mask_r());
	gk20a_writel(g, gr_status_mask_r(), data & gr->status_disable_mask);

	gr_gk20a_init_zbc(g, gr);

	{
		u64 compbit_base_post_divide64 = (gr->compbit_store.base_pa >>
				ltc_ltc0_lts0_cbc_base_alignment_shift_v());
		do_div(compbit_base_post_divide64, gr->num_fbps);
		compbit_base_post_divide = u64_lo32(compbit_base_post_divide64);
	}

	compbit_base_post_multiply64 = ((u64)compbit_base_post_divide *
		gr->num_fbps) << ltc_ltc0_lts0_cbc_base_alignment_shift_v();

	if (compbit_base_post_multiply64 < gr->compbit_store.base_pa)
		compbit_base_post_divide++;

	gk20a_writel(g, ltc_ltcs_ltss_cbc_base_r(),
		compbit_base_post_divide);

	nvhost_dbg(dbg_info | dbg_map | dbg_pte,
		   "compbit base.pa: 0x%x,%08x cbc_base:0x%08x\n",
		   (u32)(gr->compbit_store.base_pa>>32),
		   (u32)(gr->compbit_store.base_pa & 0xffffffff),
		   compbit_base_post_divide);

	/* load ctx init */
	for (i = 0; i < sw_ctx_load->count; i++)
		gk20a_writel(g, sw_ctx_load->l[i].addr,
			     sw_ctx_load->l[i].value);

	/* TBD: add gr ctx overrides */

	err = gr_gk20a_wait_idle(g, end_jiffies, GR_IDLE_CHECK_DEFAULT);
	if (err)
		goto out;

	/* save and disable fe_go_idle */
	fe_go_idle_timeout_save =
		gk20a_readl(g, gr_fe_go_idle_timeout_r());
	gk20a_writel(g, gr_fe_go_idle_timeout_r(),
		(fe_go_idle_timeout_save & gr_fe_go_idle_timeout_count_f(0)) |
		gr_fe_go_idle_timeout_count_disabled_f());

	/* override a few ctx state registers */
	gr_gk20a_commit_global_cb_manager(g, NULL, 0);
	gr_gk20a_commit_global_timeslice(g, NULL, 0);

	/* floorsweep anything left */
	gr_gk20a_ctx_state_floorsweep(g);

	err = gr_gk20a_wait_idle(g, end_jiffies, GR_IDLE_CHECK_DEFAULT);
	if (err)
		goto restore_fe_go_idle;

	/* enable pipe mode override */
	gk20a_writel(g, gr_pipe_bundle_config_r(),
		gr_pipe_bundle_config_override_pipe_mode_enabled_f());

	/* load bundle init */
	err = 0;
	for (i = 0; i < sw_bundle_init->count; i++) {

		if (i == 0 || last_bundle_data != sw_bundle_init->l[i].value) {
			gk20a_writel(g, gr_pipe_bundle_data_r(),
				sw_bundle_init->l[i].value);
			last_bundle_data = sw_bundle_init->l[i].value;
		}

		gk20a_writel(g, gr_pipe_bundle_address_r(),
			     sw_bundle_init->l[i].addr);

		if (gr_pipe_bundle_address_value_v(sw_bundle_init->l[i].addr) ==
		    GR_GO_IDLE_BUNDLE)
			err |= gr_gk20a_wait_idle(g, end_jiffies,
					GR_IDLE_CHECK_DEFAULT);
		else if (0) { /* IS_SILICON */
			u32 delay = GR_IDLE_CHECK_DEFAULT;
			do {
				u32 gr_status = gk20a_readl(g, gr_status_r());

				if (gr_status_fe_method_lower_v(gr_status) ==
				    gr_status_fe_method_lower_idle_v())
					break;

				usleep_range(delay, delay * 2);
				delay = min_t(u32, delay << 1,
					GR_IDLE_CHECK_MAX);

			} while (time_before(jiffies, end_jiffies) |
					!tegra_platform_is_silicon());
		}
	}

	/* disable pipe mode override */
	gk20a_writel(g, gr_pipe_bundle_config_r(),
		     gr_pipe_bundle_config_override_pipe_mode_disabled_f());

restore_fe_go_idle:
	/* restore fe_go_idle */
	gk20a_writel(g, gr_fe_go_idle_timeout_r(), fe_go_idle_timeout_save);

	if (err || gr_gk20a_wait_idle(g, end_jiffies, GR_IDLE_CHECK_DEFAULT))
		goto out;

	/* load method init */
	if (sw_method_init->count) {
		gk20a_writel(g, gr_pri_mme_shadow_raw_data_r(),
			     sw_method_init->l[0].value);
		gk20a_writel(g, gr_pri_mme_shadow_raw_index_r(),
			     gr_pri_mme_shadow_raw_index_write_trigger_f() |
			     sw_method_init->l[0].addr);
		last_method_data = sw_method_init->l[0].value;
	}
	for (i = 1; i < sw_method_init->count; i++) {
		if (sw_method_init->l[i].value != last_method_data) {
			gk20a_writel(g, gr_pri_mme_shadow_raw_data_r(),
				sw_method_init->l[i].value);
			last_method_data = sw_method_init->l[i].value;
		}
		gk20a_writel(g, gr_pri_mme_shadow_raw_index_r(),
			gr_pri_mme_shadow_raw_index_write_trigger_f() |
			sw_method_init->l[i].addr);
	}

	gk20a_mm_l2_invalidate(g);

    /* hack: using hard-coded bits for now until
     * the reg l1c_dbg reg makes it into hw_gr_gk20a.h
     */
    {

        l1c_dbg_reg_val = gk20a_readl(g, 0x005044b0);
        // set the cya15 bit (27:27) to 1
        l1c_dbg_reg_val = l1c_dbg_reg_val | 0x08000000;
        gk20a_writel(g, 0x005044b0, l1c_dbg_reg_val);
    }

	err = gr_gk20a_wait_idle(g, end_jiffies, GR_IDLE_CHECK_DEFAULT);
	if (err)
		goto out;

out:
	nvhost_dbg_fn("done");
	return 0;
}

static int gk20a_init_gr_prepare(struct gk20a *g)
{
	u32 gpfifo_ctrl, pmc_en;
	u32 err = 0;

	/* disable fifo access */
	gpfifo_ctrl = gk20a_readl(g, gr_gpfifo_ctl_r());
	gpfifo_ctrl &= ~gr_gpfifo_ctl_access_enabled_f();
	gk20a_writel(g, gr_gpfifo_ctl_r(), gpfifo_ctrl);

	/* reset gr engine */
	pmc_en = gk20a_readl(g, mc_enable_r());
	pmc_en &= ~mc_enable_pgraph_enabled_f();
	pmc_en &= ~mc_enable_blg_enabled_f();
	pmc_en &= ~mc_enable_perfmon_enabled_f();
	gk20a_writel(g, mc_enable_r(), pmc_en);

	pmc_en = gk20a_readl(g, mc_enable_r());
	pmc_en |= mc_enable_pgraph_enabled_f();
	pmc_en |= mc_enable_blg_enabled_f();
	pmc_en |= mc_enable_perfmon_enabled_f();
	gk20a_writel(g, mc_enable_r(), pmc_en);
	pmc_en = gk20a_readl(g, mc_enable_r());

	/* enable fifo access */
	gk20a_writel(g, gr_gpfifo_ctl_r(),
		gr_gpfifo_ctl_access_enabled_f() |
		gr_gpfifo_ctl_semaphore_access_enabled_f());

	if (!g->gr.ctx_vars.valid) {
		err = gr_gk20a_init_ctx_vars(g, &g->gr);
		if (err)
			nvhost_err(dev_from_gk20a(g),
				"fail to load gr init ctx");
	}
	return err;
}

static int gk20a_init_gr_reset_enable_hw(struct gk20a *g)
{
	struct gr_gk20a *gr = &g->gr;
	struct av_list_gk20a *sw_non_ctx_load = &g->gr.ctx_vars.sw_non_ctx_load;
	unsigned long end_jiffies = jiffies +
		msecs_to_jiffies(gk20a_get_gr_idle_timeout(g));
	u32 i, err = 0;

	nvhost_dbg_fn("");

	/* enable interrupts */
	gk20a_writel(g, gr_intr_r(), ~0);
	gk20a_writel(g, gr_intr_en_r(), ~0);

	/* reset ctx switch state */
	gr_gk20a_ctx_reset(g, 0);

	/* clear scc ram */
	gk20a_writel(g, gr_scc_init_r(),
		gr_scc_init_ram_trigger_f());

	/* load non_ctx init */
	for (i = 0; i < sw_non_ctx_load->count; i++)
		gk20a_writel(g, sw_non_ctx_load->l[i].addr,
			sw_non_ctx_load->l[i].value);

	err = gr_gk20a_wait_idle(g, end_jiffies, GR_IDLE_CHECK_DEFAULT);
	if (err)
		goto out;

	err = gr_gk20a_load_ctxsw_ucode(g, gr);
	if (err)
		goto out;

	/* this appears query for sw states but fecs actually init
	   ramchain, etc so this is hw init */
	err = gr_gk20a_init_ctx_state(g, gr);
	if (err)
		goto out;

out:
	if (err)
		nvhost_dbg(dbg_fn | dbg_err, "fail");
	else
		nvhost_dbg_fn("done");

	return 0;
}

static int gk20a_init_gr_setup_sw(struct gk20a *g)
{
	struct gr_gk20a *gr = &g->gr;
	int err;

	nvhost_dbg_fn("");

	if (gr->sw_ready) {
		nvhost_dbg_fn("skip init");
		return 0;
	}

	gr->g = g;

	err = gr_gk20a_init_gr_config(g, gr);
	if (err)
		goto clean_up;

	err = gr_gk20a_init_mmu_sw(g, gr);
	if (err)
		goto clean_up;

	err = gr_gk20a_init_map_tiles(g, gr);
	if (err)
		goto clean_up;

	if (tegra_cpu_is_asim())
		gr->max_comptag_mem = 1; /* MBs worth of comptag coverage */
	else {
		nvhost_dbg_info("total ram pages : %lu", totalram_pages);
		gr->max_comptag_mem = totalram_pages
					 >> (10 - (PAGE_SHIFT - 10));
	}
	err = gr_gk20a_init_comptag(g, gr);
	if (err)
		goto clean_up;

	err = gr_gk20a_init_zcull(g, gr);
	if (err)
		goto clean_up;

	err = gr_gk20a_alloc_global_ctx_buffers(g);
	if (err)
		goto clean_up;

	mutex_init(&gr->ctx_mutex);
	spin_lock_init(&gr->ch_tlb_lock);

	gr->remove_support = gk20a_remove_gr_support;
	gr->sw_ready = true;

	nvhost_dbg_fn("done");
	return 0;

clean_up:
	nvhost_dbg(dbg_fn | dbg_err, "fail");
	gk20a_remove_gr_support(gr);
	return err;
}

int gk20a_init_gr_support(struct gk20a *g)
{
	u32 err;

	nvhost_dbg_fn("");

	err = gk20a_init_gr_prepare(g);
	if (err)
		return err;

	/* this is required before gr_gk20a_init_ctx_state */
	mutex_init(&g->gr.fecs_mutex);

	err = gk20a_init_gr_reset_enable_hw(g);
	if (err)
		return err;

	err = gk20a_init_gr_setup_sw(g);
	if (err)
		return err;

	err = gk20a_init_gr_setup_hw(g);
	if (err)
		return err;

	return 0;
}

#define NVA297_SET_ALPHA_CIRCULAR_BUFFER_SIZE	0x02dc
#define NVA297_SET_CIRCULAR_BUFFER_SIZE		0x1280
#define NVA297_SET_SHADER_EXCEPTIONS		0x1528
#define NVA0C0_SET_SHADER_EXCEPTIONS		0x1528

#define NVA297_SET_SHADER_EXCEPTIONS_ENABLE_FALSE 0

struct gr_isr_data {
	u32 addr;
	u32 data_lo;
	u32 data_hi;
	u32 curr_ctx;
	u32 chid;
	u32 offset;
	u32 sub_chan;
	u32 class_num;
};

static void gk20a_gr_set_shader_exceptions(struct gk20a *g,
					   struct gr_isr_data *isr_data)
{
	u32 val;

	nvhost_dbg_fn("");

	if (isr_data->data_lo ==
	    NVA297_SET_SHADER_EXCEPTIONS_ENABLE_FALSE)
		val = 0;
	else
		val = ~0;

	gk20a_writel(g,
		gr_gpcs_tpcs_sm_hww_warp_esr_report_mask_r(),
		val);
	gk20a_writel(g,
		gr_gpcs_tpcs_sm_hww_global_esr_report_mask_r(),
		val);
}

static void gk20a_gr_set_circular_buffer_size(struct gk20a *g,
			struct gr_isr_data *isr_data)
{
	struct gr_gk20a *gr = &g->gr;
	u32 gpc_index, ppc_index, stride, val, offset;
	u32 cb_size = isr_data->data_lo * 4;

	nvhost_dbg_fn("");

	if (cb_size > gr->attrib_cb_size)
		cb_size = gr->attrib_cb_size;

	gk20a_writel(g, gr_ds_tga_constraintlogic_r(),
		(gk20a_readl(g, gr_ds_tga_constraintlogic_r()) &
		 ~gr_ds_tga_constraintlogic_beta_cbsize_f(~0)) |
		 gr_ds_tga_constraintlogic_beta_cbsize_f(cb_size));

	for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++) {
		stride = proj_gpc_stride_v() * gpc_index;

		for (ppc_index = 0; ppc_index < gr->gpc_ppc_count[gpc_index];
			ppc_index++) {

			val = gk20a_readl(g, gr_gpc0_ppc0_cbm_cfg_r() +
				stride +
				proj_ppc_in_gpc_stride_v() * ppc_index);

			offset = gr_gpc0_ppc0_cbm_cfg_start_offset_v(val);

			val = set_field(val,
				gr_gpc0_ppc0_cbm_cfg_size_m(),
				gr_gpc0_ppc0_cbm_cfg_size_f(cb_size *
					gr->pes_tpc_count[ppc_index][gpc_index]));
			val = set_field(val,
				gr_gpc0_ppc0_cbm_cfg_start_offset_m(),
				(offset + 1));

			gk20a_writel(g, gr_gpc0_ppc0_cbm_cfg_r() +
				stride +
				proj_ppc_in_gpc_stride_v() * ppc_index, val);

			val = set_field(val,
				gr_gpc0_ppc0_cbm_cfg_start_offset_m(),
				offset);

			gk20a_writel(g, gr_gpc0_ppc0_cbm_cfg_r() +
				stride +
				proj_ppc_in_gpc_stride_v() * ppc_index, val);
		}
	}
}

static void gk20a_gr_set_alpha_circular_buffer_size(struct gk20a *g,
						struct gr_isr_data *isr_data)
{
	struct gr_gk20a *gr = &g->gr;
	u32 gpc_index, ppc_index, stride, val;
	u32 pd_ab_max_output;
	u32 alpha_cb_size = isr_data->data_lo * 4;

	nvhost_dbg_fn("");
	/* if (NO_ALPHA_BETA_TIMESLICE_SUPPORT_DEF)
		return; */

	if (alpha_cb_size > gr->alpha_cb_size)
		alpha_cb_size = gr->alpha_cb_size;

	gk20a_writel(g, gr_ds_tga_constraintlogic_r(),
		(gk20a_readl(g, gr_ds_tga_constraintlogic_r()) &
		 ~gr_ds_tga_constraintlogic_alpha_cbsize_f(~0)) |
		 gr_ds_tga_constraintlogic_alpha_cbsize_f(alpha_cb_size));

	pd_ab_max_output = alpha_cb_size *
		gr_gpc0_ppc0_cbm_cfg_size_granularity_v() /
		gr_pd_ab_dist_cfg1_max_output_granularity_v();

	gk20a_writel(g, gr_pd_ab_dist_cfg1_r(),
		gr_pd_ab_dist_cfg1_max_output_f(pd_ab_max_output));

	for (gpc_index = 0; gpc_index < gr->gpc_count; gpc_index++) {
		stride = proj_gpc_stride_v() * gpc_index;

		for (ppc_index = 0; ppc_index < gr->gpc_ppc_count[gpc_index];
			ppc_index++) {

			val = gk20a_readl(g, gr_gpc0_ppc0_cbm_cfg2_r() +
				stride +
				proj_ppc_in_gpc_stride_v() * ppc_index);

			val = set_field(val, gr_gpc0_ppc0_cbm_cfg2_size_m(),
					gr_gpc0_ppc0_cbm_cfg2_size_f(alpha_cb_size *
						gr->pes_tpc_count[ppc_index][gpc_index]));

			gk20a_writel(g, gr_gpc0_ppc0_cbm_cfg2_r() +
				stride +
				proj_ppc_in_gpc_stride_v() * ppc_index, val);
		}
	}
}

void gk20a_gr_reset(struct gk20a *g)
{
	int err;
	err = gk20a_init_gr_prepare(g);
	BUG_ON(err);
	err = gk20a_init_gr_reset_enable_hw(g);
	BUG_ON(err);
	err = gk20a_init_gr_setup_hw(g);
	BUG_ON(err);
}

static void gk20a_gr_nop_method(struct gk20a *g)
{
	/* Reset method in PBDMA 0 */
	gk20a_writel(g, pbdma_method0_r(0),
			pbdma_udma_nop_r());
	gk20a_writel(g, pbdma_data0_r(0), 0);
}

static int gk20a_gr_handle_illegal_method(struct gk20a *g,
					  struct gr_isr_data *isr_data)
{
	nvhost_dbg_fn("");

	if (isr_data->class_num == KEPLER_COMPUTE_A) {
		switch (isr_data->offset << 2) {
		case NVA0C0_SET_SHADER_EXCEPTIONS:
			gk20a_gr_set_shader_exceptions(g, isr_data);
			break;
		default:
			goto fail;
		}
	}

	if (isr_data->class_num == KEPLER_C) {
		switch (isr_data->offset << 2) {
		case NVA297_SET_SHADER_EXCEPTIONS:
			gk20a_gr_set_shader_exceptions(g, isr_data);
			break;
		case NVA297_SET_CIRCULAR_BUFFER_SIZE:
			gk20a_gr_set_circular_buffer_size(g, isr_data);
			break;
		case NVA297_SET_ALPHA_CIRCULAR_BUFFER_SIZE:
			gk20a_gr_set_alpha_circular_buffer_size(g, isr_data);
			break;
		default:
			goto fail;
		}
	}
	return 0;

fail:
	gk20a_gr_reset(g);
	gk20a_gr_nop_method(g);
	nvhost_err(dev_from_gk20a(g), "invalid method class 0x%08x"
		", offset 0x%08x address 0x%08x\n",
		isr_data->class_num, isr_data->offset, isr_data->addr);
	return -EINVAL;
}

static int gk20a_gr_handle_illegal_class(struct gk20a *g,
					  struct gr_isr_data *isr_data)
{
	nvhost_dbg_fn("");

	gk20a_gr_reset(g);

	gk20a_gr_nop_method(g);
	nvhost_err(dev_from_gk20a(g),
		   "invalid class 0x%08x, offset 0x%08x",
		   isr_data->class_num, isr_data->offset);
	return -EINVAL;
}

static int gk20a_gr_handle_class_error(struct gk20a *g,
					  struct gr_isr_data *isr_data)
{
	nvhost_dbg_fn("");

	gk20a_gr_reset(g);

	gk20a_gr_nop_method(g);
	nvhost_err(dev_from_gk20a(g),
		   "class error 0x%08x, offset 0x%08x",
		   isr_data->class_num, isr_data->offset);
	return -EINVAL;
}

static int gk20a_gr_handle_notify_pending(struct gk20a *g,
					  struct gr_isr_data *isr_data)
{
	struct fifo_gk20a *f = &g->fifo;
	struct channel_gk20a *ch = &f->channel[isr_data->chid];

#if defined(CONFIG_TEGRA_GPU_CYCLE_STATS)
	void *virtual_address;
	u32 buffer_size;
	u32 offset;
	u32 new_offset;
	bool exit;
	struct share_buffer_head *sh_hdr;
	u32 raw_reg;
	u64 mask_orig;
	u64 v = 0;
	struct gk20a_cyclestate_buffer_elem *op_elem;
	/* GL will never use payload 0 for cycle state */
	if ((ch->cyclestate.cyclestate_buffer == NULL) || (isr_data->data_lo == 0))
		return 0;

	mutex_lock(&ch->cyclestate.cyclestate_buffer_mutex);

	virtual_address = ch->cyclestate.cyclestate_buffer;
	buffer_size = ch->cyclestate.cyclestate_buffer_size;
	offset = isr_data->data_lo;
	exit = false;
	while (!exit) {
		if (offset >= buffer_size) {
			WARN_ON(1);
			break;
		}

		sh_hdr = (struct share_buffer_head *)
			((char *)virtual_address + offset);

		if (sh_hdr->size < sizeof(struct share_buffer_head)) {
			WARN_ON(1);
			break;
		}
		new_offset = offset + sh_hdr->size;

		switch (sh_hdr->operation) {
		case OP_END:
			exit = true;
			break;

		case BAR0_READ32:
		case BAR0_WRITE32:
		{
			op_elem =
				(struct gk20a_cyclestate_buffer_elem *)
					sh_hdr;
			if (op_elem->offset_bar0 <
				TEGRA_GK20A_BAR0_SIZE) {
				mask_orig =
					((1ULL <<
					(op_elem->last_bit + 1))
					-1)&~((1ULL <<
					op_elem->first_bit)-1);

				raw_reg =
					gk20a_readl(g,
						op_elem->offset_bar0);

				switch (sh_hdr->operation) {
				case BAR0_READ32:
					op_elem->data =
					(raw_reg & mask_orig)
						>> op_elem->first_bit;
					break;

				case BAR0_WRITE32:
					v = 0;
					if ((unsigned int)mask_orig !=
					(unsigned int)~0) {
						v = (unsigned int)
							(raw_reg & ~mask_orig);
					}

					v |= ((op_elem->data
						<< op_elem->first_bit)
						& mask_orig);

					gk20a_writel(g,
						op_elem->offset_bar0,
						(unsigned int)v);
						break;

				default:
						break;
				}
			} else {
				sh_hdr->failed = true;
				WARN_ON(1);
			}
		}
		break;
		default:
		/* no operation content case */
			exit = true;
			break;
		}
		sh_hdr->completed = true;
		offset = new_offset;
	}
	mutex_unlock(&ch->cyclestate.cyclestate_buffer_mutex);
#endif
	nvhost_dbg_fn("");
	wake_up(&ch->notifier_wq);
	return 0;
}

/* Used by sw interrupt thread to translate current ctx to chid.
 * For performance, we don't want to go through 128 channels every time.
 * A small tlb is used here to cache translation */
static int gk20a_gr_get_chid_from_ctx(struct gk20a *g, u32 curr_ctx)
{
	struct fifo_gk20a *f = &g->fifo;
	struct gr_gk20a *gr = &g->gr;
	u32 chid = -1;
	u32 i;
	struct scatterlist *ctx_sg;

	spin_lock(&gr->ch_tlb_lock);

	/* check cache first */
	for (i = 0; i < GR_CHANNEL_MAP_TLB_SIZE; i++) {
		if (gr->chid_tlb[i].curr_ctx == curr_ctx) {
			chid = gr->chid_tlb[i].hw_chid;
			goto unlock;
		}
	}

	/* slow path */
	for (chid = 0; chid < f->num_channels; chid++)
		if (f->channel[chid].in_use) {
			ctx_sg = f->channel[chid].inst_block.mem.sgt->sgl;
			if ((u32)(sg_phys(ctx_sg) >> ram_in_base_shift_v()) ==
				gr_fecs_current_ctx_ptr_v(curr_ctx))
				break;
	}

	if (chid >= f->num_channels) {
		chid = -1;
		goto unlock;
	}

	/* add to free tlb entry */
	for (i = 0; i < GR_CHANNEL_MAP_TLB_SIZE; i++) {
		if (gr->chid_tlb[i].curr_ctx == 0) {
			gr->chid_tlb[i].curr_ctx = curr_ctx;
			gr->chid_tlb[i].hw_chid = chid;
			goto unlock;
		}
	}

	/* no free entry, flush one */
	gr->chid_tlb[gr->channel_tlb_flush_index].curr_ctx = curr_ctx;
	gr->chid_tlb[gr->channel_tlb_flush_index].hw_chid = chid;

	gr->channel_tlb_flush_index =
		(gr->channel_tlb_flush_index + 1) &
		(GR_CHANNEL_MAP_TLB_SIZE - 1);

unlock:
	spin_unlock(&gr->ch_tlb_lock);
	return chid;
}

static struct channel_gk20a *
channel_from_hw_chid(struct gk20a *g, u32 hw_chid)
{
	return g->fifo.channel+hw_chid;
}

void gk20a_gr_isr(struct gk20a *g)
{
	struct gr_isr_data isr_data;
	u32 grfifo_ctl;
	u32 obj_table;
	int ret = 0;
	u32 gr_intr = gk20a_readl(g, gr_intr_r());

	nvhost_dbg_fn("");

	if (!gr_intr)
		return;

	grfifo_ctl = gk20a_readl(g, gr_gpfifo_ctl_r());
	grfifo_ctl &= ~gr_gpfifo_ctl_semaphore_access_f(1);
	grfifo_ctl &= ~gr_gpfifo_ctl_access_f(1);

	gk20a_writel(g, gr_gpfifo_ctl_r(),
		grfifo_ctl | gr_gpfifo_ctl_access_f(0) |
		gr_gpfifo_ctl_semaphore_access_f(0));

	isr_data.addr = gk20a_readl(g, gr_trapped_addr_r());
	isr_data.data_lo = gk20a_readl(g, gr_trapped_data_lo_r());
	isr_data.data_hi = gk20a_readl(g, gr_trapped_data_hi_r());
	isr_data.curr_ctx = gk20a_readl(g, gr_fecs_current_ctx_r());
	isr_data.offset = gr_trapped_addr_mthd_v(isr_data.addr);
	isr_data.sub_chan = gr_trapped_addr_subch_v(isr_data.addr);
	obj_table = gk20a_readl(g,
		gr_fe_object_table_r(isr_data.sub_chan));
	isr_data.class_num = gr_fe_object_table_nvclass_v(obj_table);

	isr_data.chid =
		gk20a_gr_get_chid_from_ctx(g, isr_data.curr_ctx);
	if (isr_data.chid == -1) {
		nvhost_err(dev_from_gk20a(g), "invalid channel ctx 0x%08x",
			   isr_data.curr_ctx);
		goto clean_up;
	}

	nvhost_dbg(dbg_intr, "channel %d: addr 0x%08x, "
		"data 0x%08x 0x%08x,"
		"ctx 0x%08x, offset 0x%08x, "
		"subchannel 0x%08x, class 0x%08x",
		isr_data.chid, isr_data.addr,
		isr_data.data_hi, isr_data.data_lo,
		isr_data.curr_ctx, isr_data.offset,
		isr_data.sub_chan, isr_data.class_num);

	if (gr_intr & gr_intr_notify_pending_f()) {
		gk20a_gr_handle_notify_pending(g, &isr_data);
		gk20a_writel(g, gr_intr_r(),
			gr_intr_notify_reset_f());
		gr_intr &= ~gr_intr_notify_pending_f();
	}

	if (gr_intr & gr_intr_illegal_method_pending_f()) {
		ret = gk20a_gr_handle_illegal_method(g, &isr_data);
		gk20a_writel(g, gr_intr_r(),
			gr_intr_illegal_method_reset_f());
		gr_intr &= ~gr_intr_illegal_method_pending_f();
	}

	if (gr_intr & gr_intr_illegal_class_pending_f()) {
		ret = gk20a_gr_handle_illegal_class(g, &isr_data);
		gk20a_writel(g, gr_intr_r(),
			gr_intr_illegal_class_reset_f());
		gr_intr &= ~gr_intr_illegal_class_pending_f();
	}

	if (gr_intr & gr_intr_class_error_pending_f()) {
		ret = gk20a_gr_handle_class_error(g, &isr_data);
		gk20a_writel(g, gr_intr_r(),
			gr_intr_class_error_reset_f());
		gr_intr &= ~gr_intr_class_error_pending_f();
	}

	if (gr_intr & gr_intr_exception_pending_f()) {
		u32 exception = gk20a_readl(g, gr_exception_r());

		nvhost_dbg(dbg_intr, "exception %08x\n", exception);

		if (exception & gr_exception_fe_m()) {
			u32 fe = gk20a_readl(g, gr_fe_hww_esr_r());
			nvhost_dbg(dbg_intr, "fe warning %08x\n", fe);
			gk20a_writel(g, gr_fe_hww_esr_r(), fe);
		}
		gk20a_writel(g, gr_intr_r(),
			gr_intr_exception_reset_f());
		gr_intr &= ~gr_intr_exception_pending_f();
	}

	if (ret) {
		struct channel_gk20a *fault_ch =
			channel_from_hw_chid(g, isr_data.chid);
		if (fault_ch && fault_ch->hwctx)
			gk20a_free_channel(fault_ch->hwctx, false);
	}

clean_up:
	gk20a_writel(g, gr_gpfifo_ctl_r(),
		grfifo_ctl | gr_gpfifo_ctl_access_f(1) |
		gr_gpfifo_ctl_semaphore_access_f(1));

	if (gr_intr)
		nvhost_err(dev_from_gk20a(g),
			   "unhandled gr interrupt 0x%08x", gr_intr);
}

int gr_gk20a_fecs_get_reglist_img_size(struct gk20a *g, u32 *size)
{
	BUG_ON(size == NULL);
	return gr_gk20a_submit_fecs_method(g, 0, 0, ~0, 1,
		gr_fecs_method_push_adr_discover_reglist_image_size_v(),
		size, GR_IS_UCODE_OP_NOT_EQUAL, 0, GR_IS_UCODE_OP_SKIP, 0);
}

int gr_gk20a_fecs_set_reglist_bind_inst(struct gk20a *g, phys_addr_t addr)
{
	return gr_gk20a_submit_fecs_method(g, 4,
		gr_fecs_current_ctx_ptr_f(addr >> 12) |
		gr_fecs_current_ctx_valid_f(1) | gr_fecs_current_ctx_target_vid_mem_f(),
		~0, 1, gr_fecs_method_push_adr_set_reglist_bind_instance_f(),
		0, GR_IS_UCODE_OP_EQUAL, 1, GR_IS_UCODE_OP_SKIP, 0);
}

int gr_gk20a_fecs_set_reglist_virual_addr(struct gk20a *g, u64 pmu_va)
{
	return gr_gk20a_submit_fecs_method(g, 4, u64_lo32(pmu_va >> 8),
		~0, 1, gr_fecs_method_push_adr_set_reglist_virtual_address_f(),
		0, GR_IS_UCODE_OP_EQUAL, 1, GR_IS_UCODE_OP_SKIP, 0);
}

int gk20a_gr_suspend(struct gk20a *g)
{
	unsigned long end_jiffies = jiffies +
		msecs_to_jiffies(gk20a_get_gr_idle_timeout(g));
	u32 ret = 0;

	nvhost_dbg_fn("");

	ret = gr_gk20a_wait_idle(g, end_jiffies, GR_IDLE_CHECK_DEFAULT);
	if (ret)
		return ret;

	gk20a_writel(g, gr_gpfifo_ctl_r(),
		gr_gpfifo_ctl_access_disabled_f());

	/* disable gr intr */
	gk20a_writel(g, gr_intr_r(), 0);
	gk20a_writel(g, gr_intr_en_r(), 0);

	/* disable all exceptions */
	gk20a_writel(g, gr_exception_r(), 0);
	gk20a_writel(g, gr_exception_en_r(), 0);
	gk20a_writel(g, gr_exception1_r(), 0);
	gk20a_writel(g, gr_exception1_en_r(), 0);
	gk20a_writel(g, gr_exception2_r(), 0);
	gk20a_writel(g, gr_exception2_en_r(), 0);

	gk20a_gr_flush_channel_tlb(&g->gr);

	nvhost_dbg_fn("done");
	return ret;
}
