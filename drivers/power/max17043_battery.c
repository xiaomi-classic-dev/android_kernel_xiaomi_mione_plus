/*
 *  max17043_battery.c
 *  fuel-gauge systems for lithium-ion (Li+) batteries
 *
 *  Copyright (C) 2009 Samsung Electronics
 *  Minkyu Kang <mk7.kang@samsung.com>
 *  Copyright (C) 2009 Xiaomi Corporation
 *  Lin Liu <liulin@xiaomi.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/max17043_battery.h>
#include <linux/msm-charger.h>

#define MAX17043_VCELL_MSB	0x02
#define MAX17043_VCELL_LSB	0x03
#define MAX17043_SOC_MSB	0x04
#define MAX17043_SOC_LSB	0x05
#define MAX17043_MODE_MSB	0x06
#define MAX17043_MODE_LSB	0x07
#define MAX17043_VER_MSB	0x08
#define MAX17043_VER_LSB	0x09
#define MAX17043_RCOMP_CFG	0x0C
#define MAX17043_ATHD		0x0D
#define MAX17043_OCV_MSB		0x0E
#define MAX17043_MODEL_ACCESS	0x3E
#define MAX17043_CMD_MSB	0xFE
#define MAX17043_CMD_LSB	0xFF
#define MAX17043_RETRY_SECONDS	10

#define BATT_TEMP_UNKNOW	-300
#define BATT_ALERT_SOC		10
#define BATT_OFF_SOC		4
/* ATHD: 0x1F means 1%, 0x00 means 32% */
#define BATT_ALERT_ATHD		(32 - BATT_ALERT_SOC)

struct max17043_chip {
	struct i2c_client *client;
	struct max17043_platform_data *pdata;
	struct timespec next_update_time;
	struct wake_lock wlock;
	/* battery voltage */
	int vcell;
	/* battery capacity */
	int soc;
	/* the lastest soc */
	int last_soc;
	/* battery alert threshold */
	int athd;
	/* battery temperature last */
	int last_temp;
	int model_loaded;
	bool irq_requested;
	bool irq_wake_enabled;
	struct mutex max17043_lock;
};

static struct delayed_work bootup_work;
static bool system_is_bootup = true;
static u8 vcell_msb, vcell_lsb;	/* Used as the base adjustment */
/* Save soc each time it's read out */
static int ocv_saved;

struct max17043_chip *batt_chip;
static int max17043_load_model(struct max17043_chip *chip);
static irqreturn_t max17043_battery_short(int irq, void *dev);
extern int msm_charger_update_heartbeat(void);

static int max17043_read_reg(struct i2c_client *client, int reg)
{
	int ret;

	ret = i2c_smbus_read_byte_data(client, reg);

	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);

	return ret;
}

/* Keep SMBus errors signed until both register bytes have been checked. */
static int max17043_read_word(struct i2c_client *client, int reg, u16 *value)
{
	int msb, lsb;

	msb = max17043_read_reg(client, reg);
	if (msb < 0)
		return msb;
	lsb = max17043_read_reg(client, reg + 1);
	if (lsb < 0)
		return lsb;
	*value = msb << 8 | lsb;
	return 0;
}

static int max17043_write_word(struct i2c_client *client, int reg, u16 value)
{
	int ret = i2c_smbus_write_word_data(client, reg, swab16(value));

	if (ret < 0)
		dev_err(&client->dev, "write reg 0x%02x: err %d\n", reg, ret);
	return ret;
}

static void max17043_next_update(struct max17043_chip *chip, int seconds)
{
	ktime_get_ts(&chip->next_update_time);
	monotonic_to_bootbased(&chip->next_update_time);
	chip->next_update_time.tv_sec += seconds;
}

static void max17043_set_athd(struct max17043_chip *chip, int psoc)
{
	int soc, ret, rcomp;
	u16 config, raw_soc;

	if (psoc == 0) {
		ret = max17043_read_word(chip->client, MAX17043_SOC_MSB,
					&raw_soc);
		if (ret < 0)
			return;
		soc = raw_soc / 512;
	} else {
		soc = psoc;
	}

	/* The alert window is between 0% and 16% */
	if (soc <= 0)
		soc = 1;
	if (soc > 16)
		soc = 16;

	rcomp = max17043_read_reg(chip->client, MAX17043_RCOMP_CFG);
	if (rcomp < 0)
		return;

	/* set battery short alert to 30%/2 or current soc - 2 */
	config = rcomp << 8 | (32 - soc * 2);

	/* clear ATHD and set new ATHD value */
	ret = max17043_write_word(chip->client, MAX17043_RCOMP_CFG, config);
	if (ret < 0)
		return;
	chip->athd = soc;
	pr_info("max17043 set athd:0x%04x soc:%d\n", swab16(config), soc);
}

static void max17043_get_soc_local(struct i2c_client *client)
{
	struct max17043_chip *chip = i2c_get_clientdata(client);
	struct timespec now;
	static int vcell_min;
	int soc, ret;
	int charging = battery_charging();
	u16 raw_soc, raw_vcell;

	ktime_get_ts(&now);
	monotonic_to_bootbased(&now);
	if (timespec_compare(&now, &chip->next_update_time) < 0)
		return;

	max17043_next_update(chip, 1);

	if (!chip->model_loaded) {
		ret = max17043_load_model(chip);
		if (ret < 0)
			goto retry;
		chip->model_loaded = 1;
		dev_info(&client->dev, "model loaded successfully\n");
	}

	ret = max17043_read_word(client, MAX17043_SOC_MSB, &raw_soc);
	if (ret < 0)
		goto retry;
	ret = max17043_read_word(client, MAX17043_VCELL_MSB, &raw_vcell);
	if (ret < 0)
		goto retry;
	soc = raw_soc / 512;
	chip->soc = soc > 100 ? 100 : soc;
	chip->vcell = (raw_vcell * 5) / 4 / 16;

	/* Avoid reporting soc=0 during bootup */
	if (system_is_bootup && soc == 0 && chip->vcell > 3000 && !charging)
		chip->soc = 1;

	/* Shut down if voltage is kept below for a while */
	if (!system_is_bootup && !charging && chip->vcell < 3400)
		vcell_min++;
	else
		vcell_min = 0;

	/* soc is zero if voltage is below 3400mV for a while */
	if (!system_is_bootup
	    && !charging && chip->vcell < 3400 && vcell_min >= 5) {
		pr_info("max17043 voltage is low\n");
		chip->soc = 0;
	}

	/* Report zero if soc is below 3 and voltage is below 3500 */
	if (!system_is_bootup
	    && !charging && chip->vcell < 3500 && chip->soc < 4) {
		pr_info("max17043 soc is low:%d\n", chip->soc);
		chip->soc = 0;
	}

	/* Avoid soc is up when discharging */
	if (!charging
	    && !system_is_bootup
	    && chip->last_soc >= 0 && chip->last_soc < chip->soc)
		chip->soc = chip->last_soc;
	else
		chip->last_soc = chip->soc;

	/* wake lock */
	if (chip->soc == 0)
		wake_lock_timeout(&chip->wlock, 20 * HZ);

	/* Update battery short alarm */
	if ((chip->soc <= BATT_ALERT_SOC) && (chip->soc > 0)
	    && (abs(chip->soc - chip->athd) >= 1)) {
		pr_info("max17043 update new athd\n");
		max17043_set_athd(chip, chip->soc);
	}

	if (!chip->irq_requested) {
		ret = request_threaded_irq(client->irq, NULL,
					max17043_battery_short,
					IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					"max17043", chip);
		if (ret < 0)
			goto retry;
		chip->irq_requested = true;
	}
	if (!chip->irq_wake_enabled) {
		ret = irq_set_irq_wake(client->irq, 1);
		if (ret < 0)
			goto retry;
		chip->irq_wake_enabled = true;
	}

	pr_info("max17043 soc:%d %d v:%d\n", soc, chip->soc, chip->vcell);
	return;
retry:
	/* Retain the last valid sample and retry without hammering the bus. */
	dev_err(&client->dev, "update failed: %d, model_loaded=%d; retry in %ds\n",
		ret, chip->model_loaded, MAX17043_RETRY_SECONDS);
	max17043_next_update(chip, MAX17043_RETRY_SECONDS);
}

void max17043_temperature_compensation(int temp)
{
	struct max17043_chip *chip;
	struct max17043_platform_data *pdata;
	u16 config;
	int athd, ncomp, ret;

	if (batt_chip == NULL) {
		pr_warn("Fuel Gauge Not Ready!\n");
		return;
	}
	chip = batt_chip;
	pdata = chip->pdata;
	mutex_lock(&chip->max17043_lock);
	if (!chip->pdata->ready)
		goto out;

	if (chip->last_temp != BATT_TEMP_UNKNOW &&
	    abs(temp - chip->last_temp) < 2)
		goto out;

	/* Update RCOMP */

	if (temp > 20)
		ncomp = pdata->rcomp_value +
		    (((temp - 20) * pdata->temp_cold_up) / 1000);
	else if (temp < 20)
		ncomp = pdata->rcomp_value +
		    (((temp - 20) * pdata->temp_cold_down) / 1000);
	else
		ncomp = pdata->rcomp_value;

	if (ncomp > 255)
		ncomp = 255;
	else if (ncomp < 0)
		ncomp = 0;

	athd = max17043_read_reg(chip->client, MAX17043_ATHD);
	if (athd < 0)
		goto out;
	/* set next alert threshold and clear alert and sleep bit */
	config = ncomp << 8 | athd;
	ret = max17043_write_word(chip->client, MAX17043_RCOMP_CFG, config);
	if (ret < 0)
		goto out;
	chip->last_temp = temp;

	pr_info("max17043 compensation temp:%d config:0x%04x\n",
		temp, swab16(config));
out:
	mutex_unlock(&chip->max17043_lock);
}

EXPORT_SYMBOL(max17043_temperature_compensation);

int max17043_get_batt_soc(void)
{
	struct max17043_chip *chip;

	chip = batt_chip;

	if (batt_chip == NULL) {
		pr_warn("Gauge IC Not Ready!\n");
		return 1;
	}

	/* 17043 is suspended */
	if (!chip->pdata->ready)
		return chip->soc;

	mutex_lock(&chip->max17043_lock);
	max17043_get_soc_local(chip->client);
	mutex_unlock(&chip->max17043_lock);

	return chip->soc;
}

EXPORT_SYMBOL(max17043_get_batt_soc);

int max17043_get_batt_mvolts(void)
{
	struct max17043_chip *chip;

	chip = batt_chip;

	if (batt_chip == NULL) {
		pr_warn("Fuel Gauge Not Ready!\n");
		return 0;
	}

	/* 17043 is suspended */
	if (!chip->pdata->ready)
		return chip->vcell;

	mutex_lock(&chip->max17043_lock);
	max17043_get_soc_local(chip->client);
	mutex_unlock(&chip->max17043_lock);

	return chip->vcell;
}

EXPORT_SYMBOL(max17043_get_batt_mvolts);

static int max17043_get_version(struct i2c_client *client)
{
	int ret;
	u16 version;

	ret = max17043_read_word(client, MAX17043_VER_MSB, &version);
	if (!ret)
		dev_info(&client->dev, "max17043 Fuel-Gauge Ver %d%d\n",
			 version >> 8, version & 0xff);
	return ret;
}

/* thread interrupt handler */
static irqreturn_t max17043_battery_short(int irq, void *dev)
{
	struct max17043_chip *chip;
	int i;

	chip = (struct max17043_chip *)dev;

	/* make resume update battery capacity */
	wake_lock_timeout(&chip->wlock, HZ);
	pr_info("max17043 irq \n");

	for (i = 0; i < 5; i++) {
		if (chip->pdata->ready)
			break;
		else
			msleep(100);
	}
	if (!chip->pdata->ready) {
		pr_err("max17043 battery short irq not handled\n");
	} else {
		msm_charger_update_heartbeat();
		mutex_lock(&chip->max17043_lock);
		max17043_set_athd(chip, 0);
		mutex_unlock(&chip->max17043_lock);
	}

	return IRQ_HANDLED;
}

static int max17043_load_model(struct max17043_chip *chip)
{
	int i, ret, err;
	struct max17043_platform_data *pdata;
	struct i2c_client *client;
	u16 config, original_ocv, restore_ocv, soc, ocv = 0, vcell, v_tmp;
	u16 raw_vcell, base_vcell;
	bool saved = false;
	int changed = 0;

	pdata = chip->pdata;
	client = chip->client;

	/* The vcell is read during loading model */
	ret = max17043_read_word(client, MAX17043_VCELL_MSB, &raw_vcell);
	if (ret < 0)
		return ret;
	v_tmp = (raw_vcell * 5) / 4 / 16;
	base_vcell = vcell_msb << 8 | vcell_lsb;

	/* Unlock Model Access */
	ret = max17043_write_word(client, MAX17043_MODEL_ACCESS, pdata->unlock);
	if (ret < 0)
		goto lock;
	/* Read original RCOMP and OCV */
	ret = max17043_read_word(client, MAX17043_RCOMP_CFG, &config);
	if (ret < 0)
		goto lock;
	ret = max17043_read_word(client, MAX17043_OCV_MSB, &original_ocv);
	if (ret < 0)
		goto lock;
	saved = true;
	restore_ocv = original_ocv;
	pr_info("max17043 load_model 0x%02x 0x%02x 0x%02x 0x%02x\n",
		config >> 8, config & 0xff, original_ocv >> 8,
		original_ocv & 0xff);
	/* Write OCV Test Value */
	ret = max17043_write_word(client, MAX17043_OCV_MSB, pdata->ocv_test);
	if (ret < 0)
		goto restore;
	/* Write RCOMP to a Maximum value */
	ret = max17043_write_word(client, MAX17043_RCOMP_CFG, 0xFF00);
	if (ret < 0)
		goto restore;
	/* Write the Model */
	for (i = 0; i < 64; i += 2) {
		soc = pdata->model[i] << 8 | pdata->model[i + 1];
		ret = max17043_write_word(client, 0x40 + i, soc);
		if (ret < 0)
			goto restore;
	}
	msleep(160);
	/* Write OCV Test Value */
	ret = max17043_write_word(client, MAX17043_OCV_MSB, pdata->ocv_test);
	if (ret < 0)
		goto restore;
	/* Wait for verification */
	msleep(160);
	/* Read SOC */
	ret = max17043_read_word(client, MAX17043_SOC_MSB, &soc);
	if (ret < 0)
		goto restore;
	if ((soc >> 8) >= pdata->soc_checkA &&
	    (soc >> 8) <= pdata->soc_checkB)
		ret = 0;
	else
		ret = -EINVAL;

	pr_info("max17043 soc:0x%2x 0x%2x in 0x%x,0x%x chk:%d\n",
		soc >> 8, soc & 0xff, pdata->soc_checkA, pdata->soc_checkB, ret);
	if (ret < 0)
		goto restore;

	/* The vcell during initialization */
	vcell = (base_vcell * 5) / 4 / 16;
	ocv = (original_ocv * 5) / 4 / 16;

	pr_info("max17043 vcell:%d v_tmp:%d ocv:%d\n", vcell, v_tmp, ocv);

	/* Average(v_tmp, vcell) */
	if (!battery_charging() && v_tmp > vcell) {
		vcell = v_tmp;
		base_vcell = raw_vcell;
	}
	/* The difference is based on the experiment */
	if ((ocv > (vcell + 200)) || (vcell > (ocv + 250))) {
		/* voltage compensation */
		restore_ocv = (((base_vcell >> 8) + 2) & 0xff) << 8 |
			      (base_vcell & 0xff);
		changed = 1;
	}

	/* Preserve the successful-load OCV policy; failures restore the original. */
	pr_info("max17043 model restore: adjusted=%d charging=%d "
		"vcell=%u ocv_before=%u ocv_after=%u wait_ms=%u verify=%d\n",
		changed, battery_charging(), vcell, ocv,
		(restore_ocv * 5) / 4 / 16,
		changed ? 500 : 200, ret);
restore:
	err = max17043_write_word(client, MAX17043_RCOMP_CFG, config);
	if (err < 0) {
		if (!ret)
			ret = err;
		max17043_write_word(client, MAX17043_RCOMP_CFG, config);
	}
	err = max17043_write_word(client, MAX17043_OCV_MSB,
				  ret ? original_ocv : restore_ocv);
	if (err < 0) {
		if (!ret)
			ret = err;
		/* A failed transfer may have changed the register partially. */
		max17043_write_word(client, MAX17043_OCV_MSB, original_ocv);
	}
	/* Lock Model Access */
lock:
	err = max17043_write_word(client, MAX17043_MODEL_ACCESS, 0);
	if (err < 0) {
		if (!ret)
			ret = err;
		max17043_write_word(client, MAX17043_MODEL_ACCESS, 0);
	}
	if (saved)
		msleep(!ret && changed ? 500 : 200);
	if (ret < 0)
		return ret;
	vcell_msb = base_vcell >> 8;
	vcell_lsb = base_vcell & 0xff;
	ocv_saved = changed ? vcell + 40 : ocv;
	return 0;
}

static void max17043_bootup(struct work_struct *work)
{
	pr_info("%s\n", __func__);
	system_is_bootup = false;
}

static int __devinit max17043_probe(struct i2c_client *client,
				    const struct i2c_device_id *id)
{
	int ret;
	u16 raw_vcell;
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct max17043_chip *chip;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA |
				   I2C_FUNC_SMBUS_WORD_DATA))
		return -EIO;

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->client = client;
	chip->pdata = client->dev.platform_data;
	mutex_init(&chip->max17043_lock);

	i2c_set_clientdata(client, chip);

	ret = max17043_get_version(client);
	if (ret < 0) {
		ret = -ENODEV;
		goto err;
	}

	ret = max17043_read_word(client, MAX17043_VCELL_MSB, &raw_vcell);
	if (ret < 0)
		goto err;
	vcell_msb = raw_vcell >> 8;
	vcell_lsb = raw_vcell & 0xff;
	chip->vcell = (raw_vcell * 5) / 4 / 16;
	chip->soc = 1;
	chip->last_soc = -1;

	chip->last_temp = BATT_TEMP_UNKNOW;
	chip->model_loaded = 0;

	/* next update must be at least 1 second later */
	ktime_get_ts(&chip->next_update_time);
	monotonic_to_bootbased(&chip->next_update_time);

	wake_lock_init(&chip->wlock, WAKE_LOCK_SUSPEND, "batt_short");
	INIT_DELAYED_WORK(&bootup_work, max17043_bootup);
	schedule_delayed_work(&bootup_work,
			      round_jiffies_relative(msecs_to_jiffies(65000)));

	chip->pdata->ready = true;
	batt_chip = chip;

	return 0;
err:
	batt_chip = NULL;
	kfree(chip);
	return ret;
}

static int __devexit max17043_remove(struct i2c_client *client)
{
	struct max17043_chip *chip = i2c_get_clientdata(client);

	batt_chip = NULL;
	cancel_delayed_work_sync(&bootup_work);
	if (chip->irq_wake_enabled)
		irq_set_irq_wake(client->irq, 0);
	if (chip->irq_requested)
		free_irq(client->irq, chip);
	wake_lock_destroy(&chip->wlock);
	kfree(chip);
	return 0;
}

#ifdef CONFIG_PM

static int max17043_suspend(struct i2c_client *client, pm_message_t state)
{
	struct max17043_chip *chip = i2c_get_clientdata(client);

	chip->pdata->ready = false;
	pr_debug("max17043 suspend\n");
	return 0;
}

static int max17043_resume(struct i2c_client *client)
{
	struct max17043_chip *chip = i2c_get_clientdata(client);

	chip->pdata->ready = true;
	pr_debug("max17043 resume\n");
	return 0;
}

#else

#define max17040_suspend NULL
#define max17040_resume NULL

#endif /* CONFIG_PM */

static const struct i2c_device_id max17043_id[] = {
	{"max17043", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, max17043_id);

static struct i2c_driver max17043_i2c_driver = {
	.driver = {
		   .name = "max17043",
		   },
	.probe = max17043_probe,
	.remove = __devexit_p(max17043_remove),
	.suspend = max17043_suspend,
	.resume = max17043_resume,
	.id_table = max17043_id,
};

static int __init max17043_init(void)
{
	return i2c_add_driver(&max17043_i2c_driver);
}

late_initcall(max17043_init);

static void __exit max17043_exit(void)
{
	i2c_del_driver(&max17043_i2c_driver);
}

module_exit(max17043_exit);

MODULE_AUTHOR("Liu Lin <liulin@xiaomi.com>");
MODULE_DESCRIPTION("MAX17043 Fuel Gauge");
MODULE_LICENSE("GPL");
