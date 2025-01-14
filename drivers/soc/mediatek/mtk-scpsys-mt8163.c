/*
 * Copyright (c) 2015 MediaTek Inc.
 * Author: James Liao <jamesjj.liao@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/infracfg.h>
#include <dt-bindings/power/mt8163-power.h>

#define SPM_VDE_PWR_CON			0x0210
#define SPM_MFG_PWR_CON			0x0214
#define SPM_VEN_PWR_CON			0x0230
#define SPM_ISP_PWR_CON			0x0238
#define SPM_DIS_PWR_CON			0x023c
#define SPM_CONN_PWR_CON		0x0280
#define SPM_AUDIO_PWR_CON		0x029c
#define SPM_MFG_ASYNC_PWR_CON		0x02c4
#define SPM_PWR_STATUS			0x060c
#define SPM_PWR_STATUS_2ND		0x0610

#define PWR_RST_B_BIT			BIT(0)
#define PWR_ISO_BIT			BIT(1)
#define PWR_ON_BIT			BIT(2)
#define PWR_ON_2ND_BIT			BIT(3)
#define PWR_CLK_DIS_BIT			BIT(4)

#define PWR_STATUS_CONN			BIT(1)
#define PWR_STATUS_DISP			BIT(3)
#define PWR_STATUS_MFG			BIT(4)
#define PWR_STATUS_ISP			BIT(5)
#define PWR_STATUS_VDEC			BIT(7)
#define PWR_STATUS_VENC			BIT(21)
#define PWR_STATUS_MFG_ASYNC		BIT(23)
#define PWR_STATUS_AUDIO		BIT(24)

enum clk_id {
	MT8163_CLK_NONE,
	MT8163_CLK_MM,
	MT8163_CLK_MFG,
	MT8163_CLK_MAX,
};

#define MAX_CLKS	2

struct scp_domain_data {
	const char *name;
	u32 sta_mask;
	int ctl_offs;
	u32 sram_pdn_bits;
	u32 sram_pdn_ack_bits;
	u32 bus_prot_mask;
	enum clk_id clk_id[MAX_CLKS];
	bool active_wakeup;
};

static const struct scp_domain_data scp_domain_data[] __initconst = {
	[MT8163_POWER_DOMAIN_VDEC] = {
		.name = "vdec",
		.sta_mask = PWR_STATUS_VDEC,
		.ctl_offs = SPM_VDE_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {MT8163_CLK_MM},
		.active_wakeup = true,
	},
	[MT8163_POWER_DOMAIN_VENC] = {
		.name = "venc",
		.sta_mask = PWR_STATUS_VENC,
		.ctl_offs = SPM_VEN_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {MT8163_CLK_MM},
		.active_wakeup = true,
	},
	[MT8163_POWER_DOMAIN_ISP] = {
		.name = "isp",
		.sta_mask = PWR_STATUS_ISP,
		.ctl_offs = SPM_ISP_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(13, 12),
		.clk_id = {MT8163_CLK_MM},
		.active_wakeup = true,
	},
	[MT8163_POWER_DOMAIN_DISP] = {
		.name = "mm",
		.sta_mask = PWR_STATUS_DISP,
		.ctl_offs = SPM_DIS_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {MT8163_CLK_MM},
		.active_wakeup = true,
		.bus_prot_mask = MT8163_TOP_AXI_PROT_EN_MM_M0,
	},
	[MT8163_POWER_DOMAIN_AUDIO] = {
		.name = "audio",
		.sta_mask = PWR_STATUS_AUDIO,
		.ctl_offs = SPM_AUDIO_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {MT8163_CLK_NONE},
		.active_wakeup = true,
	},
	[MT8163_POWER_DOMAIN_MFG_ASYNC] = {
		.name = "mfg_async",
		.sta_mask = PWR_STATUS_MFG_ASYNC,
		.ctl_offs = SPM_MFG_ASYNC_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = 0,
		.clk_id = {MT8163_CLK_NONE},
		.active_wakeup = false,
		.bus_prot_mask = MT8163_TOP_AXI_PROT_EN_MFG_M0 |
			MT8163_TOP_AXI_PROT_EN_MFG_SNOOP_OUT,
	},
	[MT8163_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = PWR_STATUS_MFG,
		.ctl_offs = SPM_MFG_PWR_CON,
		.sram_pdn_bits = GENMASK(13, 8),
		.sram_pdn_ack_bits = GENMASK(16, 16),
		.clk_id = {MT8163_CLK_MFG},
		.active_wakeup = false,
	},
	[MT8163_POWER_DOMAIN_CONN] = {
		.name = "conn",
		.sta_mask = PWR_STATUS_CONN,
		.ctl_offs = SPM_CONN_PWR_CON,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = 0,
		.clk_id = {MT8163_CLK_NONE},
		.active_wakeup = true,
		.bus_prot_mask = MT8163_TOP_AXI_PROT_EN_CCI_M2 |
			MT8163_TOP_AXI_PROT_EN_CONN2EMI |
			MT8163_TOP_AXI_PROT_EN_CONN2PERI,
	},
};

#define NUM_DOMAINS	ARRAY_SIZE(scp_domain_data)

typedef void (power_state_fn)(struct device *dev, int state);

struct scp;

struct scp_domain {
	struct generic_pm_domain genpd;
	struct scp *scp;
	struct clk *clk[MAX_CLKS];
	u32 sta_mask;
	void __iomem *ctl_addr;
	u32 sram_pdn_bits;
	u32 sram_pdn_ack_bits;
	u32 bus_prot_mask;
	bool active_wakeup;
	power_state_fn *power_state_cb;
	struct device *power_state_dev;
};

struct scp {
	struct scp_domain domains[NUM_DOMAINS];
	struct genpd_onecell_data pd_data;
	struct device *dev;
	void __iomem *base;
	struct regmap *infracfg;
};


static int scpsys_domain_is_on(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;

	u32 status = readl(scp->base + SPM_PWR_STATUS) & scpd->sta_mask;
	u32 status2 = readl(scp->base + SPM_PWR_STATUS_2ND) & scpd->sta_mask;

	/*
	 * A domain is on when both status bits are set. If only one is set
	 * return an error. This happens while powering up a domain
	 */

	if (status && status2)
		return true;
	if (!status && !status2)
		return false;

	return -EINVAL;
}

static int scpsys_power_on(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	unsigned long timeout;
	bool expired;
	void __iomem *ctl_addr = scpd->ctl_addr;
	u32 sram_pdn_ack = scpd->sram_pdn_ack_bits;
	u32 val;
	int ret;
	int i;

	for (i = 0; i < MAX_CLKS && scpd->clk[i]; i++) {
		ret = clk_prepare_enable(scpd->clk[i]);
		if (ret) {
			for (--i; i >= 0; i--)
				clk_disable_unprepare(scpd->clk[i]);

			goto err_clk;
		}
	}

	val = readl(ctl_addr);
	val |= PWR_ON_BIT;
	writel(val, ctl_addr);
	val |= PWR_ON_2ND_BIT;
	writel(val, ctl_addr);

	/* wait until PWR_ACK = 1 */
	timeout = jiffies + HZ;
	expired = false;
	while (1) {
		ret = scpsys_domain_is_on(scpd);
		if (ret > 0)
			break;

		if (expired) {
			ret = -ETIMEDOUT;
			goto err_pwr_ack;
		}

		cpu_relax();

		if (time_after(jiffies, timeout))
			expired = true;
	}

	val &= ~PWR_CLK_DIS_BIT;
	writel(val, ctl_addr);

	val &= ~PWR_ISO_BIT;
	writel(val, ctl_addr);

	val |= PWR_RST_B_BIT;
	writel(val, ctl_addr);

	val &= ~scpd->sram_pdn_bits;
	writel(val, ctl_addr);

	/* wait until SRAM_PDN_ACK all 0 */
	timeout = jiffies + HZ;
	expired = false;
	while (sram_pdn_ack && (readl(ctl_addr) & sram_pdn_ack)) {

		if (expired) {
			ret = -ETIMEDOUT;
			goto err_pwr_ack;
		}

		cpu_relax();

		if (time_after(jiffies, timeout))
			expired = true;
	}

	if (scpd->bus_prot_mask) {
		ret = mtk_infracfg_clear_bus_protection(scp->infracfg,
				scpd->bus_prot_mask,
                false);
		if (ret)
			goto err_pwr_ack;
	}

	if (scpd->power_state_cb)
		scpd->power_state_cb(scpd->power_state_dev, 1);

	return 0;

err_pwr_ack:
	for (i = MAX_CLKS - 1; i >= 0; i--) {
		if (scpd->clk[i])
			clk_disable_unprepare(scpd->clk[i]);
	}
err_clk:
	dev_err(scp->dev, "Failed to power on domain %s\n", genpd->name);

	return ret;
}

static int scpsys_power_off(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	unsigned long timeout;
	bool expired;
	void __iomem *ctl_addr = scpd->ctl_addr;
	u32 pdn_ack = scpd->sram_pdn_ack_bits;
	u32 val;
	int ret;
	int i;

	if (scpd->power_state_cb)
		scpd->power_state_cb(scpd->power_state_dev, 0);

	if (scpd->bus_prot_mask) {
		ret = mtk_infracfg_set_bus_protection(scp->infracfg,
				scpd->bus_prot_mask,
                   false);
		if (ret)
			goto out;
	}

	val = readl(ctl_addr);
	val |= scpd->sram_pdn_bits;
	writel(val, ctl_addr);

	/* wait until SRAM_PDN_ACK all 1 */
	timeout = jiffies + HZ;
	expired = false;
	while (pdn_ack && (readl(ctl_addr) & pdn_ack) != pdn_ack) {
		if (expired) {
			ret = -ETIMEDOUT;
			goto out;
		}

		cpu_relax();

		if (time_after(jiffies, timeout))
			expired = true;
	}

	val |= PWR_ISO_BIT;
	writel(val, ctl_addr);

	val &= ~PWR_RST_B_BIT;
	writel(val, ctl_addr);

	val |= PWR_CLK_DIS_BIT;
	writel(val, ctl_addr);

	val &= ~PWR_ON_BIT;
	writel(val, ctl_addr);

	val &= ~PWR_ON_2ND_BIT;
	writel(val, ctl_addr);

	/* wait until PWR_ACK = 0 */
	timeout = jiffies + HZ;
	expired = false;
	while (1) {
		ret = scpsys_domain_is_on(scpd);
		if (ret == 0)
			break;

		if (expired) {
			ret = -ETIMEDOUT;
			goto out;
		}

		cpu_relax();

		if (time_after(jiffies, timeout))
			expired = true;
	}

	for (i = 0; i < MAX_CLKS && scpd->clk[i]; i++)
		clk_disable_unprepare(scpd->clk[i]);

	return 0;

out:
	dev_err(scp->dev, "Failed to power off domain %s\n", genpd->name);

	return ret;
}

static bool scpsys_active_wakeup(struct device *dev)
{
	struct generic_pm_domain *genpd;
	struct scp_domain *scpd;

	genpd = pd_to_genpd(dev->pm_domain);
	scpd = container_of(genpd, struct scp_domain, genpd);

	return scpd->active_wakeup;
}

static int __init scpsys_probe(struct platform_device *pdev)
{
	struct genpd_onecell_data *pd_data;
	struct resource *res;
	int i, j, ret;
	struct scp *scp;
	struct clk *clk[MT8163_CLK_MAX];

	scp = devm_kzalloc(&pdev->dev, sizeof(*scp), GFP_KERNEL);
	if (!scp)
		return -ENOMEM;

	scp->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	scp->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(scp->base))
		return PTR_ERR(scp->base);

	pd_data = &scp->pd_data;

	pd_data->domains = devm_kzalloc(&pdev->dev,
			sizeof(*pd_data->domains) * NUM_DOMAINS, GFP_KERNEL);
	if (!pd_data->domains)
		return -ENOMEM;

	clk[MT8163_CLK_MM] = devm_clk_get(&pdev->dev, "mm");
	if (IS_ERR(clk[MT8163_CLK_MM]))
		return PTR_ERR(clk[MT8163_CLK_MM]);

	clk[MT8163_CLK_MFG] = devm_clk_get(&pdev->dev, "mfg");
	if (IS_ERR(clk[MT8163_CLK_MFG]))
		return PTR_ERR(clk[MT8163_CLK_MFG]);

	scp->infracfg = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
			"infracfg");
	if (IS_ERR(scp->infracfg)) {
		dev_err(&pdev->dev, "Cannot find infracfg controller: %ld\n",
				PTR_ERR(scp->infracfg));
		return PTR_ERR(scp->infracfg);
	}

	pd_data->num_domains = NUM_DOMAINS;

	for (i = 0; i < NUM_DOMAINS; i++) {
		struct scp_domain *scpd = &scp->domains[i];
		struct generic_pm_domain *genpd = &scpd->genpd;
		const struct scp_domain_data *data = &scp_domain_data[i];

		pd_data->domains[i] = genpd;
		scpd->scp = scp;

		scpd->sta_mask = data->sta_mask;
		scpd->ctl_addr = scp->base + data->ctl_offs;
		scpd->sram_pdn_bits = data->sram_pdn_bits;
		scpd->sram_pdn_ack_bits = data->sram_pdn_ack_bits;
		scpd->bus_prot_mask = data->bus_prot_mask;
		scpd->active_wakeup = data->active_wakeup;
		for (j = 0; j < MAX_CLKS && data->clk_id[j]; j++)
			scpd->clk[j] = clk[data->clk_id[j]];

		genpd->name = data->name;
		genpd->power_off = scpsys_power_off;
		genpd->power_on = scpsys_power_on;
		//genpd->dev_ops.active_wakeup = scpsys_active_wakeup;

		/*
		 * Initially turn on all domains to make the domains usable
		 * with !CONFIG_PM and to get the hardware in sync with the
		 * software.  The unused domains will be switched off during
		 * late_init time.
		 */
		genpd->power_on(genpd);

		pm_genpd_init(genpd, NULL, false);
	}

	/*
	 * We are not allowed to fail here since there is no way to unregister
	 * a power domain. Once registered above we have to keep the domains
	 * valid.
	 */

	ret = pm_genpd_add_subdomain(pd_data->domains[MT8163_POWER_DOMAIN_MFG_ASYNC],
		pd_data->domains[MT8163_POWER_DOMAIN_MFG]);

	if (ret && IS_ENABLED(CONFIG_PM))
		dev_err(&pdev->dev, "Failed to add subdomain: %d\n", ret);

	ret = of_genpd_add_provider_onecell(pdev->dev.of_node, pd_data);
	if (ret)
		dev_err(&pdev->dev, "Failed to add OF provider: %d\n", ret);

	return 0;
}

static const struct of_device_id of_scpsys_match_tbl[] = {
	{
		.compatible = "mediatek,mt8163-scpsys",
	}, {
		/* sentinel */
	}
};

static struct platform_driver scpsys_drv = {
	.driver = {
		.name = "mtk-scpsys-mt8163",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(of_scpsys_match_tbl),
	},
};

static int __init scpsys_drv_init(void)
{
	return platform_driver_probe(&scpsys_drv, scpsys_probe);
}

static void __exit scpsys_drv_exit(void)
{
	platform_driver_unregister(&scpsys_drv);
}

subsys_initcall(scpsys_drv_init);
module_exit(scpsys_drv_exit);
