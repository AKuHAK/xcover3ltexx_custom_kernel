/*
 * Battery driver for Marvell 88PM886 fuel-gauge chip
 *
 * Copyright (c) 2014 Marvell International Ltd.
 * Author:	Yi Zhang <yizhang@marvell.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/power_supply.h>
#include <linux/mfd/88pm886.h>
#include <linux/delay.h>
#include <linux/math64.h>
#include <linux/of_device.h>

#define MONITOR_INTERVAL		(HZ * 60)
#define LOW_BAT_INTERVAL		(HZ * 10)
#define LOW_BAT_CAP			(15)
enum {
	ALL_SAVED_DATA,
	SLEEP_COUNT,
};

struct pm886_battery_params {
	int status;
	int present;
	int volt;	/* µV */
	int ibat;	/* mA */
	int soc;	/* percents: 0~100% */
	int health;
	int tech;
	int temp;
};

struct pm886_battery_info {
	struct pm886_chip	*chip;
	struct device	*dev;
	struct pm886_battery_params	bat_params;

	struct power_supply	battery;
	struct delayed_work	monitor_work;
	struct delayed_work	charged_work;
	struct delayed_work	cc_work; /* sigma-delta offset compensation */
	struct workqueue_struct *bat_wqueue;
	atomic_t		cc_done;

	int			total_capacity;

	int			r_int;
	unsigned int		bat_ntc;

	int			ocv_is_realiable;
	int			range_low_th;
	int			range_high_th;
	int			sleep_counter_th;

	int			power_off_threshold;
	int			safe_power_off_threshold;

	int			irq_nums;
	int			irq[7];
};

static int ocv_table[100];

static char *supply_interface[] = {
	"ac",
	"usb",
};

struct ccnt {
	int soc;	/* mC, 1mAH = 1mA * 3600 = 3600 mC */
	int max_cc;
	int last_cc;
	int cc_offs;
	int cc_int;
};
static struct ccnt ccnt_data;

struct save_buffer {
	int soc;
	int temp;
	int ocv_is_realiable;
};
static struct save_buffer extern_data;

/*
 * - saved SoC
 * - ocv_is_realiable flag
 */
static int get_extern_data(struct pm886_battery_info *info, int flag)
{
	return 0;
}
static void set_extern_data(struct pm886_battery_info *info,
			    int flag, int data)
{
	return;
}

static int pm886_battery_write_buffer(struct pm886_battery_info *info,
				      struct save_buffer *value)
{
	int data;
	/* save values in RTC registers */
	data = (value->soc & 0x7f) | (value->ocv_is_realiable << 7);

	/* bits 0,1 are used for other purpose, so give up them * */
	data |= (((value->temp / 10) + 50) & 0xfc) << 8;
	set_extern_data(info, ALL_SAVED_DATA, data);

	return 0;
}

static int pm886_battery_read_buffer(struct pm886_battery_info *info,
				     struct save_buffer *value)
{
	int data;

	/* read values from RTC registers */
	data = get_extern_data(info, ALL_SAVED_DATA);
	value->soc = data & 0x7f;
	value->ocv_is_realiable = (data & 0x80) >> 7;
	value->temp = (((data >> 8) & 0xfc) - 50) * 10;
	if (value->temp < 0)
		value->temp = 0;

	/*
	 * if register is 0, then stored values are invalid,
	 * it may be casused by backup battery totally discharged
	 */
	if (data == 0) {
		pr_err("attention: saved value is not realiable!\n");
		return -EINVAL;
	}

	return 0;
}

static int is_charger_online(struct pm886_battery_info *info,
			     int *who_is_charging)
{
	struct power_supply *psy;
	union power_supply_propval val;
	int i, ret = 0;

	for (i = 0; i < info->battery.num_supplicants; i++) {
		psy = power_supply_get_by_name(info->battery.supplied_to[i]);
		if (!psy || !psy->get_property)
			continue;
		ret = psy->get_property(psy, POWER_SUPPLY_PROP_ONLINE, &val);
		if (ret == 0) {
			if (val.intval) {
				*who_is_charging = i;
				return val.intval;
			}
		}
	}
	*who_is_charging = 0;
	return 0;
}

static int pm886_battery_get_charger_status(struct pm886_battery_info *info)
{
	int who, ret;
	struct power_supply *psy;
	union power_supply_propval val;
	int status = POWER_SUPPLY_STATUS_UNKNOWN;

	/* report charger online timely */
	if (!is_charger_online(info, &who))
		status = POWER_SUPPLY_STATUS_DISCHARGING;
	else {
		psy = power_supply_get_by_name(info->battery.supplied_to[who]);
		if (!psy || !psy->get_property) {
			pr_info("%s: get power supply failed.\n", __func__);
			return POWER_SUPPLY_STATUS_UNKNOWN;
		}
		ret = psy->get_property(psy, POWER_SUPPLY_PROP_STATUS, &val);
		if (ret) {
			dev_err(info->dev, "get charger property failed.\n");
			status = POWER_SUPPLY_STATUS_DISCHARGING;
		} else {
			/* update battery status */
			status = val.intval;
		}
	}
	return status;
}

static bool pm886_check_battery_present(struct pm886_battery_info *info)
{
	static bool present;
	if (!info) {
		pr_err("%s: empty battery info.\n", __func__);
		return true;
	}

	if (info->bat_ntc)
		present = true;
	else
		present = true;
	/*
	 * if the battery is not present and the "battery" voltage is good
	 * suppose the external power supply is used, then
	 * - disable battery detection feature
	 */
	return present;

}

static bool system_is_reboot(struct pm886_battery_info *info)
{
	if (!info || !info->chip)
		return true;
	return !((info->chip->powerdown1) || (info->chip->powerdown2));
}

static bool check_battery_change(struct pm886_battery_info *info,
				 int new_soc, int saved_soc)
{
	int status, remove_th;
	if (!info) {
		pr_err("%s: empty battery info!\n", __func__);
		return true;
	}

	info->bat_params.status = pm886_battery_get_charger_status(info);
	status = info->bat_params.status;
	switch (status) {
	case POWER_SUPPLY_STATUS_DISCHARGING:
	case POWER_SUPPLY_STATUS_UNKNOWN:
		if (saved_soc < 5)
			remove_th = 5;
		else if (saved_soc < 15)
			remove_th = 10;
		else if (saved_soc < 80)
			remove_th = 15;
		else
			remove_th = 5;
		break;
	default:
		remove_th = 60;
		break;
	}
	return !(abs(new_soc - saved_soc) > remove_th);
}

static bool check_soc_range(struct pm886_battery_info *info, int soc)
{
	if (!info) {
		pr_err("%s: empty battery info!\n", __func__);
		return true;
	}

	if ((soc > info->range_low_th) && (soc < info->range_high_th))
		return false;
	else
		return true;
}

/*
 * register 1 bit[7:0] -- bit[11:4] of measured value of voltage
 * register 0 bit[3:0] -- bit[3:0] of measured value of voltage
 */
static int pm886_get_batt_vol(struct pm886_battery_info *info, int active)
{
	int data;

	if (active)
		data = 4000;
	else
		data = 4000;

	dev_dbg(info->dev, "%s--> %s: vbat: %dmV\n", __func__,
		active ? "active" : "sleep", data);

	return data;
}

/* get soc from ocv: lookup table */
static int pm886_get_soc_from_ocv(int ocv)
{
	int i, count = 100;
	int soc = 0;

	if (ocv < ocv_table[0]) {
		soc = 0;
		goto out;
	}

	count = 100;
	for (i = count - 1; i >= 0; i--) {
		if (ocv >= ocv_table[i]) {
			soc = i + 1;
			break;
		}
	}
out:
	return soc;
}

static int pm886_get_ibat_cc(struct pm886_battery_info *info)
{
	int data = 100;
	return data;
}

/* read gpadc0 to get temperature */
static int pm886_get_batt_temp(struct pm886_battery_info *info)
{
	int temp = 20;
	return temp;
}

static int pm886_battery_get_slp_cnt(struct pm886_battery_info *info)
{
	return 0;
}

/*
 * check whether it's the right point to set ocv_is_realiable flag
 * if yes, set it;
 *   else, leave it as 0;
 */
static void check_set_ocv_flag(struct pm886_battery_info *info,
			       struct ccnt *ccnt_val)
{
	int old_soc, vol, slp_cnt, new_soc, low_th, high_th;
	bool soc_in_good_range;

	/* save old SOC in case to recover */
	old_soc = ccnt_val->soc;
	low_th = info->range_low_th;
	high_th = info->range_high_th;

	if (info->ocv_is_realiable)
		return;

	/* check if battery is relaxed enough */
	slp_cnt = pm886_battery_get_slp_cnt(info);
	dev_dbg(info->dev, "pmic slp_cnt = %d seconds\n", slp_cnt);

	if (slp_cnt < info->sleep_counter_th) {
		dev_dbg(info->dev, "battery is not relaxed.\n");
		return;
	}

	dev_info(info->dev, "battery has slept %d second.\n", slp_cnt);

	/* read last sleep voltage and calc new SOC */
	vol = pm886_get_batt_vol(info, 0);
	new_soc = pm886_get_soc_from_ocv(vol);

	/* check if the new SoC is in good range or not */
	soc_in_good_range = check_soc_range(info, new_soc);
	if (soc_in_good_range) {
		info->ocv_is_realiable = 1;
		ccnt_val->soc = new_soc;
		dev_info(info->dev, "good range: new SoC = %d\n", new_soc);
	} else {
		info->ocv_is_realiable = 0;
		ccnt_val->soc = old_soc;
		dev_info(info->dev, "in bad range (%d), no update\n", old_soc);
	}

	ccnt_val->last_cc =
		(ccnt_val->max_cc / 1000) * (ccnt_val->soc * 10 + 5);
}

static int pm886_battery_calc_ccnt(struct pm886_battery_info *info,
				   struct ccnt *ccnt_val)
{
	/* 1. read columb counter to get the original SoC value */
	/* 2. clap battery SoC for sanity check */
	if (ccnt_val->last_cc > ccnt_val->max_cc) {
		ccnt_val->soc = 100;
		ccnt_val->last_cc = ccnt_val->max_cc;
	}
	if (ccnt_val->last_cc < 0) {
		ccnt_val->soc = 0;
		ccnt_val->last_cc = 0;
	}

	ccnt_val->soc = ccnt_val->last_cc * 100 / ccnt_val->max_cc;
	return 0;
}

/*
 * correct SoC according to user scenario:
 * for example, boost the SoC if the charger status has changed to FULL
 */
static void pm886_battery_correct_soc(struct pm886_battery_info *info,
				      struct ccnt *ccnt_val)
{
	return;
}

static void pm886_bat_update_status(struct pm886_battery_info *info)
{
	int ibat;

	/* hardcode type[Lion] */
	info->bat_params.tech = POWER_SUPPLY_TECHNOLOGY_LION;

	if (info->bat_ntc)
		info->bat_params.temp = pm886_get_batt_temp(info) * 10;
	else
		info->bat_params.temp = 260;

	if (!atomic_read(&info->cc_done))
		return;

	info->bat_params.volt = pm886_get_batt_vol(info, 1);

	ibat = pm886_get_ibat_cc(info);
	info->bat_params.ibat = ibat / 1000; /* change to mA */

	/* charging status */
	if (info->bat_params.present == 0) {
		info->bat_params.status = POWER_SUPPLY_STATUS_UNKNOWN;
		return;
	}

	pm886_battery_calc_ccnt(info, &ccnt_data);
	pm886_battery_correct_soc(info, &ccnt_data);
	info->bat_params.soc = ccnt_data.soc;

	check_set_ocv_flag(info, &ccnt_data);
}

static void pm886_battery_monitor_work(struct work_struct *work)
{
	struct pm886_battery_info *info;
	static int prev_cap, prev_volt, prev_status;

	info = container_of(work, struct pm886_battery_info, monitor_work.work);

	pm886_bat_update_status(info);

	/* notify when parameters are changed */
	if ((prev_cap != info->bat_params.soc)
	    || (abs(prev_volt - info->bat_params.volt) > 100)
	    || (prev_status != info->bat_params.status)) {
		power_supply_changed(&info->battery);

		prev_cap = info->bat_params.soc;
		prev_volt = info->bat_params.volt;
		prev_status = info->bat_params.status;
		dev_info(info->dev,
			 "cap=%d, temp=%d, volt=%d, ocv_is_realiable=%d\n",
			 info->bat_params.soc, info->bat_params.temp / 10,
			 info->bat_params.volt, info->ocv_is_realiable);
	}

	/* save the recent value in non-volatile memory */
	extern_data.soc = ccnt_data.soc;
	extern_data.temp = info->bat_params.temp;
	extern_data.ocv_is_realiable = info->ocv_is_realiable;
	pm886_battery_write_buffer(info, &extern_data);

	if (info->bat_params.soc <= LOW_BAT_CAP) {
		queue_delayed_work(info->bat_wqueue, &info->monitor_work,
				   LOW_BAT_INTERVAL);
		ccnt_data.cc_int = 10;
	} else {
		queue_delayed_work(info->bat_wqueue, &info->monitor_work,
				   MONITOR_INTERVAL);
		ccnt_data.cc_int = 60;
	}
}

static void pm886_charged_work(struct work_struct *work)
{
	struct pm886_battery_info *info =
		container_of(work, struct pm886_battery_info,
			     charged_work.work);

	pm886_bat_update_status(info);
	power_supply_changed(&info->battery);
	return;
}

/* need about 5s to finish offset compensation */
static void pm886_battery_cc_work(struct work_struct *work)
{
	struct pm886_battery_info *info;
	info = container_of(work, struct pm886_battery_info, cc_work.work);
	if (!info) {
		pr_err("%s: empty battery info!\n", __func__);
		return;
	}
	atomic_set(&info->cc_done, 1);
}

static int pm886_setup_fuelgauge(struct pm886_battery_info *info)
{
	int ret = 0;
	if (!info) {
		pr_err("%s: empty battery info.\n", __func__);
		return true;
	}

	/* 0. set FGC_CLK_EN in 0x10, to enable system clock */
	/*    set SD_PWRUP in 0x24, to enable 32kHZ and sigma-delta converter */
	/*    set IBAT_EOC_EN in 0x32, to decide EOC */
	/* 1. set OFFCOMP_EN in 0x25 to compensate Ibat */
	/* 2. set VBAT_SMPL_NUM in 0x32 to configure sample for VBAT */
	return ret;
}

static int pm886_calc_init_soc(struct pm886_battery_info *info)
{
	int initial_soc = 80;
	int ret, slp_volt, soc_from_vbat_slp, soc_from_saved, slp_cnt;
	bool battery_is_changed, soc_in_good_range;

	struct save_buffer value;
	/*---------------- the following gets the initial_soc --------------*/
	/*
	 * 1. read vbat_sleep:
	 * - then use the vbat_sleep to calculate SoC: soc_from_vbat_slp
	 */
	slp_volt = pm886_get_batt_vol(info, 0);
	soc_from_vbat_slp = pm886_get_soc_from_ocv(slp_volt);

	/* 2. read saved SoC: soc_from_saved */
	/*
	 * - if the system comes here becasue of software reboot, or
	 *   soc_from_saved is not realiable, use soc_from_vbat_slp
	 *   IOW, initial_soc = soc_from_vbat_slp; return;
	 */
	if (system_is_reboot(info)) {
		initial_soc = soc_from_vbat_slp;
		goto out;
	}

	ret = pm886_battery_read_buffer(info, &value);
	if (ret < 0) {
		initial_soc = soc_from_vbat_slp;
		goto out;
	}
	soc_from_saved = value.soc;

	/*
	 * 3. compare the soc_from_vbat_slp and the soc_from_saved
	 *    to decide whether the battery is changed:
	 *    if changed, initial_soc = soc_from_vbat_slp;
	 *    else, --> battery is not changed
	 *        if sleep_counter < threshold --> battery is not relaxed
	 *                initial_soc = soc_from_saved;
	 *        else, ---> battery is relaxed
	 *            if soc_from_vbat_slp in good range
	 *                initial_soc = soc_from_vbat_slp;
	 *            else
	 *                initial_soc = soc_from_saved;
	 */
	battery_is_changed = check_battery_change(info, soc_from_vbat_slp,
						  soc_from_saved);
	if (battery_is_changed) {
		initial_soc = soc_from_vbat_slp;
		goto out;
	}

	/* battery unchanged */
	slp_cnt = pm886_battery_get_slp_cnt(info);
	if (slp_cnt < info->sleep_counter_th) {
		initial_soc = soc_from_saved;
		goto out;
	}

	soc_in_good_range = check_soc_range(info, soc_from_vbat_slp);
	if (soc_in_good_range) {
		initial_soc = soc_from_vbat_slp;
		goto out;
	}
	initial_soc = soc_from_saved;
out:
	/* update ccnt_data timely */
	ccnt_data.soc = initial_soc;
	ccnt_data.last_cc =
		(ccnt_data.max_cc / 1000) * (ccnt_data.soc * 10 + 5);

	return initial_soc;
}

static int pm886_init_fuelgauge(struct pm886_battery_info *info)
{
	int ret;
	if (!info) {
		pr_err("%s: empty battery info.\n", __func__);
		return -EINVAL;
	}

	/* configure HW registers to enable the fuelgauge */
	ret = pm886_setup_fuelgauge(info);
	if (ret < 0) {
		dev_err(info->dev, "setup fuelgauge registers fail.\n");
		return ret;
	}

	/*------------------- the following is SW related policy -------------*/
	/* 1. check whether the battery is present */
	info->bat_params.present = pm886_check_battery_present(info);
	/* 2. get initial_soc */
	info->bat_params.soc = pm886_calc_init_soc(info);
	/* 3. the following initial the software status */
	info->bat_params.status = pm886_battery_get_charger_status(info);

	return 0;
}

static void pm886_external_power_changed(struct power_supply *psy)
{
	struct pm886_battery_info *info;

	info = container_of(psy, struct pm886_battery_info, battery);
	queue_delayed_work(info->bat_wqueue, &info->charged_work, 0);
	return;
}

static enum power_supply_property pm886_batt_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_HEALTH,
};

static int pm886_batt_get_prop(struct power_supply *psy,
			       enum power_supply_property psp,
			       union power_supply_propval *val)
{
	struct pm886_battery_info *info = dev_get_drvdata(psy->dev->parent);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = info->bat_params.status;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = info->bat_params.present;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		/* report fake capacity if battery is absent */
		if (!info->bat_params.present)
			info->bat_params.soc = 80;
		val->intval = info->bat_params.soc;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = info->bat_params.tech;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = info->bat_params.volt;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		info->bat_params.ibat = pm886_get_ibat_cc(info);
		info->bat_params.ibat /= 1000;
		val->intval = info->bat_params.ibat;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		if (!info->bat_params.present)
			info->bat_params.temp = 240;
		val->intval = info->bat_params.temp;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = info->bat_params.health;
		break;
	default:
		return -ENODEV;
	}
	return 0;
}

/* this interrupt may not need to be handled */
static irqreturn_t pm886_battery_cc_handler(int irq, void *data)
{
	struct pm886_battery_info *info = data;
	if (!info) {
		pr_err("%s: empty battery info.\n", __func__);
		return IRQ_NONE;
	}
	dev_info(info->dev, "battery columb counter interrupt is served\n");

	power_supply_changed(&info->battery);

	return IRQ_HANDLED;
}

static irqreturn_t pm886_battery_vbat_handler(int irq, void *data)
{
	struct pm886_battery_info *info = data;
	if (!info) {
		pr_err("%s: empty battery info.\n", __func__);
		return IRQ_NONE;
	}
	dev_info(info->dev, "battery voltage interrupt is served\n");

	power_supply_changed(&info->battery);

	return IRQ_HANDLED;
}

static irqreturn_t pm886_battery_detect_handler(int irq, void *data)
{
	struct pm886_battery_info *info = data;
	if (!info) {
		pr_err("%s: empty battery info.\n", __func__);
		return IRQ_NONE;
	}
	dev_info(info->dev, "battery detection interrupt is served\n");

	/* check whether the battery is present */
	info->bat_params.present = pm886_check_battery_present(info);
	power_supply_changed(&info->battery);

	return IRQ_HANDLED;
}

static struct pm886_irq_desc {
	const char *name;
	irqreturn_t (*handler)(int irq, void *data);
} pm886_irq_descs[] = {
	{"columb counter", pm886_battery_cc_handler},
	{"battery voltage", pm886_battery_vbat_handler},
	{"battery detection", pm886_battery_detect_handler},
};

static int pm886_battery_dt_init(struct device_node *np,
			    struct pm886_battery_info *info)
{
	int ret;

	ret = of_property_read_u32(np, "bat-ntc-support", &info->bat_ntc);
	if (ret)
		return ret;
	ret = of_property_read_u32(np, "bat-capacity", &info->total_capacity);
	if (ret)
		return ret;
	ret = of_property_read_u32(np, "external-resistor", &info->r_int);
	if (ret)
		return ret;
	ret = of_property_read_u32(np, "sleep-period", &info->sleep_counter_th);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "low-threshold", &info->range_low_th);
	if (ret)
		return ret;
	ret = of_property_read_u32(np, "high-threshold", &info->range_high_th);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "power-off-threshold",
				   &info->power_off_threshold);
	if (ret)
		return ret;
	ret = of_property_read_u32(np, "safe-power-off-threshold",
			&info->safe_power_off_threshold);
	if (ret)
		return ret;

	/* initialize the ocv table */
	ret = of_property_read_u32_array(np, "ocv-table", ocv_table, 100);
	if (ret)
		return ret;

	return 0;
}

static int pm886_battery_probe(struct platform_device *pdev)
{
	struct pm886_chip *chip = dev_get_drvdata(pdev->dev.parent);
	struct pm886_battery_info *info;
	struct pm886_bat_pdata *pdata;
	struct device_node *node = pdev->dev.of_node;
	int ret;
	int i, j;

	info = devm_kzalloc(&pdev->dev, sizeof(struct pm886_battery_info),
			    GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	pdata = pdev->dev.platform_data;
	ret = pm886_battery_dt_init(node, info);
	if (ret)
		return -EINVAL;

	/* give default total capacity */
	if (info->total_capacity)
		ccnt_data.max_cc = info->total_capacity * 3600;
	else
		ccnt_data.max_cc = 1500 * 3600;

	atomic_set(&info->cc_done, 0);
	info->chip = chip;
	info->dev = &pdev->dev;
	info->bat_params.status = POWER_SUPPLY_STATUS_UNKNOWN;

	platform_set_drvdata(pdev, info);

	for (i = 0, j = 0; i < pdev->num_resources; i++) {
		info->irq[j] = platform_get_irq(pdev, i);
		if (info->irq[j] < 0)
			continue;
		j++;
	}
	info->irq_nums = j;

	ret = pm886_init_fuelgauge(info);
	if (ret < 0)
		return ret;

	pm886_bat_update_status(info);

	info->battery.name = "battery";
	info->battery.type = POWER_SUPPLY_TYPE_BATTERY;
	info->battery.properties = pm886_batt_props;
	info->battery.num_properties = ARRAY_SIZE(pm886_batt_props);
	info->battery.get_property = pm886_batt_get_prop;
	info->battery.external_power_changed = pm886_external_power_changed;
	info->battery.supplied_to = supply_interface;
	info->battery.num_supplicants = ARRAY_SIZE(supply_interface);
	power_supply_register(&pdev->dev, &info->battery);
	info->battery.dev->parent = &pdev->dev;

	info->bat_wqueue = create_singlethread_workqueue("88pm886-battery-wq");
	if (!info->bat_wqueue) {
		dev_info(chip->dev, "%s: failed to create wq.\n", __func__);
		ret = -ESRCH;
		goto out;
	}

	/* interrupt should be request in the last stage */
	for (i = 0; i < info->irq_nums; i++) {
		ret = devm_request_threaded_irq(info->dev, info->irq[i], NULL,
						pm886_irq_descs[i].handler,
						IRQF_ONESHOT | IRQF_NO_SUSPEND,
						pm886_irq_descs[i].name, info);
		if (ret < 0) {
			dev_err(info->dev, "failed to request IRQ: #%d: %d\n",
				info->irq[i], ret);
			if (!pm886_irq_descs[i].handler)
				goto out_irq;
		}
	}

	INIT_DELAYED_WORK(&info->charged_work, pm886_charged_work);

	INIT_DEFERRABLE_WORK(&info->monitor_work,
			     pm886_battery_monitor_work);
	INIT_DELAYED_WORK(&info->cc_work, pm886_battery_cc_work);
	queue_delayed_work(info->bat_wqueue, &info->cc_work, 5 * HZ);
	queue_delayed_work(info->bat_wqueue, &info->monitor_work, HZ);

	device_init_wakeup(&pdev->dev, 1);

	return 0;
out_irq:
	while (--i >= 0)
		devm_free_irq(info->dev, info->irq[i], info);
out:
	power_supply_unregister(&info->battery);

	return ret;
}

static int pm886_battery_remove(struct platform_device *pdev)
{
	int i;
	struct pm886_battery_info *info = platform_get_drvdata(pdev);
	if (!info)
		return 0;

	for (i = 0; i < info->irq_nums; i++) {
		if (pm886_irq_descs[i].handler != NULL)
			devm_free_irq(info->dev, info->irq[i], info);
	}

	cancel_delayed_work_sync(&info->monitor_work);
	cancel_delayed_work_sync(&info->charged_work);
	flush_workqueue(info->bat_wqueue);

	power_supply_unregister(&info->battery);

	platform_set_drvdata(pdev, NULL);
	return 0;
}

#ifdef CONFIG_PM
static int pm886_battery_suspend(struct device *dev)
{
	struct pm886_battery_info *info = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&info->monitor_work);
	return 0;
}

static int pm886_battery_resume(struct device *dev)
{
	struct pm886_battery_info *info = dev_get_drvdata(dev);

	/*
	 * avoid to reading in short sleep case
	 * to update ocv_is_realiable flag effectively
	 */
	queue_delayed_work(info->bat_wqueue,
			   &info->monitor_work, 300 * HZ / 1000);
	return 0;
}

static const struct dev_pm_ops pm886_battery_pm_ops = {
	.suspend	= pm886_battery_suspend,
	.resume		= pm886_battery_resume,
};
#endif

static const struct of_device_id pm886_fg_dt_match[] = {
	{ .compatible = "marvell,88pm886-battery", },
	{ },
};
MODULE_DEVICE_TABLE(of, pm886_fg_dt_match);

static struct platform_driver pm886_battery_driver = {
	.driver		= {
		.name	= "88pm886-battery",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(pm886_fg_dt_match),
#ifdef CONFIG_PM
		.pm	= &pm886_battery_pm_ops,
#endif
	},
	.probe		= pm886_battery_probe,
	.remove		= pm886_battery_remove,
};

static int pm886_battery_init(void)
{
	return platform_driver_register(&pm886_battery_driver);
}
module_init(pm886_battery_init);

static void pm886_battery_exit(void)
{
	platform_driver_unregister(&pm886_battery_driver);
}
module_exit(pm886_battery_exit);

MODULE_DESCRIPTION("Marvell 88PM886 battery driver");
MODULE_AUTHOR("Yi Zhang <yizhang@marvell.com>");
MODULE_LICENSE("GPL v2");