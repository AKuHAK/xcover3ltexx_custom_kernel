/*
 * DVC (dynamic voltage change) driver for Marvell 88PM88x
 * - Derived from dvc driver for 88pm800/88pm822/88pm860
 *
 * Copyright (C) 2014 Marvell International Ltd.
 * Joseph(Yossi) Hanin <yhanin@marvell.com>
 * Yi Zhang <yizhang@marvell.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/mfd/88pm88x.h>
#include <linux/mfd/88pm886.h>
#include <linux/mfd/88pm880.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>

/* to enable dvc*/
#define PM88X_PWR_HOLD			(1 << 7)

#define BUCK_MIN_VOLT			(600000)
#define BUCK_MAX_VOLT			(1587500)
#define BUCK_STEP			(12500)

struct pm88x_dvc_ops {
	void (*level_to_reg)(u8 level);
};

struct pm88x_buck1_dvc_desc {
	u8 current_reg;
	int max_level;
	int uV_step1;
	int uV_step2;
	int min_uV;
	int mid_uV;
	int max_uV;
	int mid_reg_val;
};

struct pm88x_dvc {
	struct device *dev;
	struct pm88x_chip *chip;
	struct pm88x_dvc_ops ops;
	struct pm88x_buck1_dvc_desc desc;
};
static struct pm88x_dvc *g_dvc;

static inline int map_volt_to_reg(int uv)
{
	if (uv >= g_dvc->desc.mid_uV)
		return (uv - g_dvc->desc.mid_uV) / (g_dvc->desc.uV_step2);
	else
		return (uv - g_dvc->desc.min_uV) / (g_dvc->desc.uV_step1);
}

static inline int list_volt_from_reg(int reg_val)
{
	if (reg_val >= g_dvc->desc.mid_reg_val)
		return reg_val * (g_dvc->desc.uV_step2) + g_dvc->desc.mid_uV;
	else
		return reg_val * (g_dvc->desc.uV_step1) + g_dvc->desc.min_uV;
}

static void pm886_level_to_reg(u8 level)
{
	if (level < 0 || level >= g_dvc->desc.max_level) {
		dev_err(g_dvc->dev, "DVC level is out of range!\n");
		return;
	}

	if (level < 4)
		g_dvc->desc.current_reg = PM886_BUCK1_VOUT + level;

	else
		g_dvc->desc.current_reg = PM886_BUCK1_4_VOUT + level - 4;
}

static void pm880_level_to_reg(u8 level)
{
	if (level < 0 || level >= g_dvc->desc.max_level) {
		dev_err(g_dvc->dev, "DVC level is out of range!\n");
		return;
	}

	g_dvc->desc.current_reg = PM880_BUCK1_VOUT + level;
}

/*
 * Example for usage: set buck1 level1 as 1200mV
 * pm88x_dvc_set_volt(1, 1200 * 1000);
 * level begins with 0
 */
int pm88x_dvc_set_volt(u8 level, int uv)
{
	int ret = 0;
	struct regmap *regmap;

	if (!g_dvc || !g_dvc->chip || !g_dvc->chip->buck_regmap) {
		pr_err("%s: NULL pointer!\n", __func__);
		return -EINVAL;
	}
	regmap = g_dvc->chip->buck_regmap;

	if (uv < g_dvc->desc.min_uV || uv > g_dvc->desc.max_uV) {
		dev_err(g_dvc->dev, "the expected voltage is out of range!\n");
		return -EINVAL;
	}

	g_dvc->ops.level_to_reg(level);

	ret = regmap_update_bits(regmap, g_dvc->desc.current_reg, 0x7f,
				 map_volt_to_reg(uv));
	return ret;
};

int pm88x_dvc_get_volt(u8 level)
{
	struct regmap *regmap;
	int ret = 0, regval = 0;
	int volt;

	if (!g_dvc || !g_dvc->chip || !g_dvc->chip->buck_regmap) {
		pr_err("%s: NULL pointer!\n", __func__);
		return -EINVAL;
	}

	g_dvc->ops.level_to_reg(level);

	regmap = g_dvc->chip->buck_regmap;
	ret = regmap_read(regmap, g_dvc->desc.current_reg, &regval);
	if (ret < 0) {
		dev_err(g_dvc->dev, "fail to read reg: 0x%x\n",
			g_dvc->desc.current_reg);
		return ret;
	}
	regval &= 0x7f;

	volt = list_volt_from_reg(regval);
	if (volt < g_dvc->desc.min_uV || volt > g_dvc->desc.max_uV) {
		dev_err(g_dvc->dev,
			"voltage out of range: %duV, level = %d\n", volt, level);
		return g_dvc->desc.min_uV;
	}

	return volt;
}

/* configure according to chip type */
static int pm88x_dvc_chip_init(struct pm88x_dvc *dvc)
{
	int ret = -EINVAL;

	if (!dvc || !dvc->chip)
		return -EINVAL;

	switch (dvc->chip->type) {
	case PM886:
		/* config gpio1 as DVC3 pin for 88pm886 */
		ret = regmap_update_bits(dvc->chip->base_regmap,
					 PM88X_GPIO_CTRL1,
					 PM88X_GPIO1_MODE_MSK,
					 PM88X_GPIO1_SET_DVC);
		if (ret < 0) {
			dev_err(dvc->dev, "Failed to set gpio1 as dvc pin!\n");
			return ret;
		}
		dvc->desc.current_reg = PM886_BUCK1_VOUT;
		dvc->desc.mid_reg_val = 0x50;
		dvc->desc.max_level = 8;
		dvc->desc.uV_step1 = 12500;
		dvc->desc.uV_step2 = 50000;
		dvc->desc.min_uV = 600000;
		dvc->desc.mid_uV = 1600000;
		dvc->desc.max_uV = 1800000;

		dvc->ops.level_to_reg = pm886_level_to_reg;
		break;
	case PM880:
		/* TODO: configure gpio to dvc function for 88pm880 */
		dvc->desc.current_reg = PM886_BUCK1_VOUT;
		dvc->desc.mid_reg_val = 0x50;
		dvc->desc.max_level = 16;
		dvc->desc.uV_step1 = 12500;
		dvc->desc.uV_step2 = 50000;
		dvc->desc.min_uV = 600000;
		dvc->desc.mid_uV = 1600000;
		dvc->desc.max_uV = 1800000;

		dvc->ops.level_to_reg = pm880_level_to_reg;
		break;
	default:
		return ret;
	}

	return 0;
}

static int pm88x_dvc_probe(struct platform_device *pdev)
{
	struct pm88x_chip *chip = dev_get_drvdata(pdev->dev.parent);
	static struct pm88x_dvc *dvcdata;
	int ret;

	dvcdata = devm_kzalloc(&pdev->dev, sizeof(*g_dvc), GFP_KERNEL);
	if (!dvcdata) {
		dev_err(&pdev->dev, "Failed to allocate g_dvc");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, dvcdata);

	/* get global handler */
	g_dvc = dvcdata;
	g_dvc->chip = chip;
	g_dvc->dev = &pdev->dev;

	ret = pm88x_dvc_chip_init(g_dvc);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to do chip specific dvc setting!\n");
		return ret;
	}

	/* enable dvc feature */
	ret = regmap_update_bits(chip->base_regmap, PM88X_MISC_CONFIG1,
				 PM88X_PWR_HOLD, PM88X_PWR_HOLD);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to enable pmic dvc feature!\n");
		return ret;
	}

	return 0;
}

static int pm88x_dvc_remove(struct platform_device *pdev)
{
	struct pm88x_dvc *dvcdata = platform_get_drvdata(pdev);
	devm_kfree(&pdev->dev, dvcdata);
	return 0;
}

static struct of_device_id pm88x_dvc_of_match[] = {
	{.compatible = "marvell,88pm88x-dvc"},
	{},
};

MODULE_DEVICE_TABLE(of, pm88x_dvc_of_match);

static struct platform_driver pm88x_dvc_driver = {
	.driver = {
		   .name = "88pm88x-dvc",
		   .owner = THIS_MODULE,
		   .of_match_table = of_match_ptr(pm88x_dvc_of_match),
		   },
	.probe = pm88x_dvc_probe,
	.remove = pm88x_dvc_remove,
};

static int pm88x_dvc_init(void)
{
	return platform_driver_register(&pm88x_dvc_driver);
}
subsys_initcall(pm88x_dvc_init);

static void pm88x_dvc_exit(void)
{
	platform_driver_unregister(&pm88x_dvc_driver);
}
module_exit(pm88x_dvc_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DVC Driver for Marvell 88PM886 PMIC");
MODULE_ALIAS("platform:88pm88x-dvc");