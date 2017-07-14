/*
 * drivers/video/tegra/dc/nvdisp/nvdisp_t19x.c
 *
 * Copyright (c) 2017, NVIDIA CORPORATION, All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "dc_priv.h"
#include "nvdisp.h"
#include "hw_nvdisp_t19x_nvdisp.h"

#define TEGRA_WINBUF_ADDR_FLAG_BLOCKLINEAR ((dma_addr_t)0x1 << 39)

static struct tegra_dc_pd_clk_info t19x_disp_pd0_clk_info[] = {
	{
		.name = "nvdisplayhub",
		.clk = NULL,
	},
	{
		.name = "nvdisplay_disp",
		.clk = NULL,
	},
	{
		.name = "nvdisplay_p0",
		.clk = NULL,
	},
};

static struct tegra_dc_pd_clk_info t19x_disp_pd1_clk_info[] = {
	{
		.name = "nvdisplay_p1",
		.clk = NULL,
	},
};

static struct tegra_dc_pd_clk_info t19x_disp_pd2_clk_info[] = {
	{
		.name = "nvdisplay_p2",
		.clk = NULL,
	},
	{
		.name = "nvdisplay_p3",
		.clk = NULL,
	},
};

/*
 * NOTE: Keep the following power domains ordered according to their head owner.
 */
static struct tegra_dc_pd_info t19x_disp_pd_info[] = {
	/* Head0 power domain */
	{
		.of_id = {
			{ .compatible = "nvidia,tegra194-disa-pd", },
			{},
		},
		.pg_id = -1,
		.head_owner = 0,
		.head_mask = 0x1,	/* Head(s):	0 */
		.win_mask = 0x1,	/* Window(s):	0 */
		.domain_clks = t19x_disp_pd0_clk_info,
		.nclks = ARRAY_SIZE(t19x_disp_pd0_clk_info),
		.ref_cnt = 0,
	},
	/* Head1 power domain */
	{
		.of_id = {
			{ .compatible = "nvidia,tegra194-disb-pd", },
			{},
		},
		.pg_id = -1,
		.head_owner = 1,
		.head_mask = 0x2,	/* Head(s):	1 */
		.win_mask = 0x6,	/* Window(s):	1,2 */
		.domain_clks = t19x_disp_pd1_clk_info,
		.nclks = ARRAY_SIZE(t19x_disp_pd1_clk_info),
		.ref_cnt = 0,
	},
	/* Head2 power domain */
	{
		.of_id = {
			{ .compatible = "nvidia,tegra194-disc-pd", },
			{},
		},
		.pg_id = -1,
		.head_owner = 2,
		.head_mask = 0xc,	/* Head(s):	2,3 */
		.win_mask = 0x38,	/* Window(s):	3,4,5 */
		.domain_clks = t19x_disp_pd2_clk_info,
		.nclks = ARRAY_SIZE(t19x_disp_pd2_clk_info),
		.ref_cnt = 0,
	},
};

static struct tegra_dc_pd_table t19x_disp_pd_table = {
	.pd_entries = t19x_disp_pd_info,
	.npd = ARRAY_SIZE(t19x_disp_pd_info),
};

int tegra_nvdisp_set_control_t19x(struct tegra_dc *dc)
{
	u32 reg, protocol;

	if (dc->out->type == TEGRA_DC_OUT_HDMI) {

		/* sor in the function name is irrelevant */
		protocol = nvdisp_t19x_sor_control_protocol_tmdsa_f();
	} else if ((dc->out->type == TEGRA_DC_OUT_DP) ||
		   (dc->out->type == TEGRA_DC_OUT_NVSR_DP) ||
		   (dc->out->type == TEGRA_DC_OUT_FAKE_DP)) {

		/* sor in the function name is irrelevant */
		protocol = nvdisp_t19x_sor_control_protocol_dpa_f();
	} else {
		dev_err(&dc->ndev->dev, "%s: unsupported out_type=%d\n",
				__func__, dc->out->type);
		return -EINVAL;
	}

	switch (dc->out_ops->get_connector_instance(dc)) {
	case 0:
		reg = nvdisp_t19x_sor_control_r();
		break;
	case 1:
		reg = nvdisp_t19x_sor1_control_r();
		break;
	case 2:
		reg = nvdisp_t19x_sor2_control_r();
		break;
	case 3:
		reg = nvdisp_t19x_sor3_control_r();
		break;
	default:
		pr_err("%s: invalid sor_num:%d\n", __func__,
				dc->out_ops->get_connector_instance(dc));
		return -ENODEV;
	}

	tegra_dc_writel(dc, protocol, reg);
	tegra_dc_enable_general_act(dc);

	return 0;
}

void tegra_dc_enable_sor_t19x(struct tegra_dc *dc, int sor_num, bool enable)
{
	u32 enb;
	u32 reg_val = tegra_dc_readl(dc, nvdisp_t19x_win_options_r());

	switch (sor_num) {
	case 0:
		enb = nvdisp_t19x_win_options_sor_set_sor_enable_f();
		break;
	case 1:
		enb = nvdisp_t19x_win_options_sor1_set_sor1_enable_f();
		break;
	case 2:
		enb = nvdisp_t19x_win_options_sor2_set_sor2_enable_f();
		break;
	case 3:
		enb = nvdisp_t19x_win_options_sor3_set_sor3_enable_f();
		break;
	default:
		pr_err("%s: invalid sor_num:%d\n", __func__, sor_num);
		return;
	}

	reg_val = enable ? reg_val | enb : reg_val & ~enb;
	tegra_dc_writel(dc, reg_val, nvdisp_t19x_win_options_r());
}

inline bool tegra_nvdisp_is_lpf_required_t19x(struct tegra_dc *dc)
{
	int yuv_flag = dc->mode.vmode & FB_VMODE_YUV_MASK;

	if (dc->yuv_bypass)
		return false;

	return ((yuv_flag & FB_VMODE_Y422) ||
		tegra_dc_is_yuv420_8bpc(yuv_flag));
}

inline void tegra_nvdisp_set_rg_unstall_t19x(struct tegra_dc *dc)
{
	int yuv_flag = dc->mode.vmode & FB_VMODE_YUV_MASK;

	if (dc->yuv_bypass)
		return;

	if (tegra_dc_is_yuv420_8bpc(yuv_flag))
		tegra_dc_writel(dc,
			tegra_dc_readl(dc, nvdisp_t19x_rg_status_r()) |
			nvdisp_t19x_rg_status_unstall_force_even_set_enable_f(),
			nvdisp_t19x_rg_status_r());
}

void tegra_dc_populate_t19x_hw_data(struct tegra_dc_hw_data *hw_data)
{
	if (!hw_data)
		return;

	hw_data->nheads = 4;
	hw_data->nwins = 6;
	hw_data->nsors = 4;
	hw_data->pd_table = &t19x_disp_pd_table;
	hw_data->valid = true;
	hw_data->version = TEGRA_DC_HW_T19x;
}

dma_addr_t nvdisp_t19x_get_addr_flag(struct tegra_dc_win *win)
{
	dma_addr_t addr_flag = 0x0;

	if (win->flags & TEGRA_WIN_FLAG_BLOCKLINEAR)
		addr_flag |= TEGRA_WINBUF_ADDR_FLAG_BLOCKLINEAR;

	return addr_flag;
}

/*
 * tegra_dc_get_vsync_timestamp_t19x - read the vsync from
 *					the ptimer regs.
 * @dc: pointer to struct tegra_dc for the current head.
 *
 * Return : timestamp value in ns.
 */
uint64_t tegra_dc_get_vsync_timestamp_t19x(struct tegra_dc *dc)
{
	u32 lsb = 0;
	u64 msb = 0;

	lsb = tegra_dc_readl(dc, nvdisp_t19x_rg_vsync_ptimer0_r());
	msb = tegra_dc_readl(dc, nvdisp_t19x_rg_vsync_ptimer1_r());

	return (((msb << 32) | lsb) << 5);
}
