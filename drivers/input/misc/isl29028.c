/* drivers/input/misc/isl29028.c
 *
 * Copyright (C) 2011 XiaoMi, Inc.
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

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/input/isl29028.h>
#include <linux/wakelock.h>
#include <linux/workqueue.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

/* isl29028 register list */
#define ISL29028_ID					0x00
#define ISL29028_CONFIGURE				0x01
#define ISL29028_INTERRUPT				0x02
#define ISL29028_PROX_LT				0x03
#define ISL29028_PROX_HT				0x04
#define ISL29028_ALSIR_TH1				0x05
#define ISL29028_ALSIR_TH2				0x06
#define ISL29028_ALSIR_TH3				0x07
#define ISL29028_PROX_DATA				0x08
#define ISL29028_ALSIR_DT1				0x09
#define ISL29028_ALSIR_DT2				0x0a
#define ISL29028_TEST1					0x0e
#define ISL29028_TEST2					0x0f

/* configure register bits */
#define ISL29028_PROX_EN				0x80
#define ISL29028_PROX_SLP_NONE				0x70
#define ISL29028_PROX_SLP_12DOT5MS			0x60
#define ISL29028_PROX_SLP_50MS				0x50
#define ISL29028_PROX_SLP_75MS				0x40
#define ISL29028_PROX_SLP_100MS				0x30
#define ISL29028_PROX_SLP_200MS				0x20
#define ISL29028_PROX_SLP_400MS				0x10
#define ISL29028_PROX_SLP_800MS				0x00
#define ISL29028_PROX_DR_220MA				0x08
#define ISL29028_ALS_EN					0x04
#define ISL29028_ALS_RANGE_HIGH				0x02
#define ISL29028_ALSIR_MODE_IR				0x01

#define ISL29028_DEF_CONFIG		ISL29028_PROX_DR_220MA

/* interrupt register bits */
#define ISL29028_PROX_FLAG				0x80
#define ISL29028_PROX_PRST_16				0x60
#define ISL29028_PROX_PRST_8				0x40
#define ISL29028_PROX_PRST_4				0x20
#define ISL29028_PROX_PRST_1				0x00
#define ISL29028_ALS_FLAG				0x08
#define ISL29028_ALS_PRST_16				0x06
#define ISL29028_ALS_PRST_8				0x04
#define ISL29028_ALS_PRST_4				0x02
#define ISL29028_ALS_PRST_1				0x00
#define ISL29028_INTCTRL_AND				0x01

#define ISL29028_ACTIVE_INTR	\
	(ISL29028_PROX_FLAG | ISL29028_PROX_PRST_1 | ISL29028_ALS_PRST_8)

#define ISL29028_INACTIVE_INTR	\
	(ISL29028_PROX_PRST_1 | ISL29028_ALS_PRST_8)

/* functionality need to support by i2c adapter */
#define ISL29028_FUNC	\
	(I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_READ_I2C_BLOCK)

/* ambient light range and resolution(unit is mLux) */
#define ISL29028_ALS_MIN				0
#define ISL29028_ALS_MAX				2137590
#define ISL29028_ALS_RES				33

/* proximity range and resolution */
#define ISL29028_PROX_MIN				0 /* near */
#define ISL29028_PROX_MAX				1 /* far */
#define ISL29028_PROX_MISC_MIN				0
#define ISL29028_PROX_MISC_MAX				255
#define ISL29028_PROX_RES				1



/* isl29028 device context */
struct isl29028_data {
	/* common state */
	struct mutex		mutex;
	struct wake_lock	wake_lock;
	struct delayed_work	delayed_work;
	struct i2c_client	*client;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend	early_suspend;
#endif
	bool			suspended;
	bool			early_suspended;
	unsigned long		dump_period;
	bool			dump_output;
	bool			dump_register;
	/* for light sensor */
	struct input_dev	*als_input;
	bool			als_opened;
	bool			als_enabled;
	bool			als_highrange;
	bool			als_autorange;
	unsigned long		als_sensitive;
	u16			als_light;
	/* for proximity sensor */
	struct input_dev	*prox_input;
	bool			prox_opened;
	bool			prox_enabled;
	unsigned long		prox_period;	/* unit: ns */
	unsigned long		prox_null_value;
	union {
		/* when an object is near:
		   1.irq need be high active to get leaving interrupt
		   2.light sensor has to temporarily disable due to item 1
		   here, union is more suitable than structure since all
		   following variables always have the same value */
		bool		irq_high_active;
		bool		als_temp_disable;
		bool		prox_object_near;
	};
	/* internal state work with irq reference count model */
	bool			irq_wakeup;
	bool			irq_disable;
	bool			irq_process;
};

/* function work with irq reference count model */
static int update_irq_wake(struct isl29028_data *data, bool on)
{
	int			error	= 0;
	struct i2c_client	*client	= data->client;

	if (!!data->irq_wakeup != !!on) {
		data->irq_wakeup = on;
		error = irq_set_irq_wake(client->irq, on);
		if (error < 0) {
			data->irq_wakeup = !on;
			dev_err(&client->dev, "fail to set irq wake.");
		}
	}

	return error;
}

static void update_irq_state(struct isl29028_data *data, bool on)
{
	struct i2c_client *client = data->client;

	if (!!data->irq_disable == !!on) {
		data->irq_disable = !on;
		if (on)
			enable_irq(client->irq);
		else
			disable_irq_nosync(client->irq);
	}
}

/* isl29028 utility function */
static unsigned long adjust_prox_period(unsigned long period)
{
	/* limit the shortest intval to 50ms to avoid the infrared led
	being visible red and increase the sensor life*/
	period = max(period, 50000000UL);

	if (period >= 800000000)	/* 800ms */
		return 800000000;
	else if (period >= 400000000)	/* 400ms */
		return 400000000;
	else if (period >= 200000000)	/* 200ms */
		return 200000000;
	else if (period >= 100000000)	/* 100ms */
		return 100000000;
	else if (period >= 75000000)	/* 75ms */
		return 75000000;
	else if (period >= 50000000)	/* 50ms */
		return 50000000;
	else if (period >= 12500000)	/* 12.5ms */
		return 12500000;
	else if (period >= 540000)	/* 0.54ms */
		return 540000;

	return 540000; /* out of range return as fast as we can */
}

static unsigned long adjust_prox_null_value(unsigned long null_value,
			struct isl29028_platform_data *pdata)
{
	unsigned long x, y;
	x = pdata->prox_lowthres_offset + pdata->prox_threswindow;
	if ((null_value + x) < 250) {
		return null_value;
	} else {
		y = 250 - x;
		if (null_value < (y + pdata->prox_lowthres_offset)) {
			return y;
		} else {
			return 255; /* error */
		}
	}
}

static bool adjust_als_range(u16 light, bool autorange,
		bool highrange, u16 lowthres, u16 highthres)
{
	if (autorange) {
		if (light <= lowthres)
			highrange = false;
		else if (light >= highthres)
			highrange = true;
	}

	return highrange;
}

static int scale_als_output(u16 light, bool highrange, unsigned long factor)
{
	int mlux;

	if (highrange)
		mlux = 522 * light;
	else
		mlux = (326 * light + 5) / 10;

	return factor * mlux;
}

static u8 get_reg_config(unsigned long period,
		bool als_active, bool prox_active, bool highrange)
{
	u8 config = ISL29028_DEF_CONFIG;

	switch (period) {
	case 800000000:
		config |= ISL29028_PROX_SLP_800MS;
		break;
	case 400000000:
		config |= ISL29028_PROX_SLP_400MS;
		break;
	case 200000000:
		config |= ISL29028_PROX_SLP_200MS;
		break;
	case 100000000:
		config |= ISL29028_PROX_SLP_100MS;
		break;
	case 75000000:
		config |= ISL29028_PROX_SLP_75MS;
		break;
	case 50000000:
		config |= ISL29028_PROX_SLP_50MS;
		break;
	case 12500000:
		config |= ISL29028_PROX_SLP_12DOT5MS;
		break;
	case 540000:
		config |= ISL29028_PROX_SLP_NONE;
		break;
	default:
		BUG(); /* period should already adjust by adjust_prox_period */
	}

	if (als_active)
		config |= ISL29028_ALS_EN;
	if (prox_active)
		config |= ISL29028_PROX_EN;
	if (highrange)
		config |= ISL29028_ALS_RANGE_HIGH;

	return config;
}

static u16 read_als_output(struct isl29028_data *data)
{
	u8			buf[2];
	u16			light	= 0;
	int			error	= 0;
	struct i2c_client	*client	= data->client;

	error = i2c_smbus_read_i2c_block_data(client,
			ISL29028_ALSIR_DT1, sizeof(buf), buf);
	if (error >= 0)
		light = (buf[1] << 8) | buf[0];
	else
		dev_err(&client->dev, "fail to read als output register.");

	return light;
}

static u8 read_prox_output(struct isl29028_data *data)
{
	s32			prox	= 0;
	struct i2c_client	*client	= data->client;

	prox = i2c_smbus_read_byte_data(client, ISL29028_PROX_DATA);
	if (prox < 0) {
		dev_err(&client->dev, "fail to read prox output register.");
		prox = 0;
	}

	return prox;
}

static int set_als_threshold(struct isl29028_data *data,
		u16 light, u16 sensitive)
{
	u8			buf[3];
	u16			lowthres;
	u16			highthres;
	int			error	= 0;
	struct i2c_client	*client	= data->client;

	lowthres  = max((light*(100-sensitive))/100, 0x000);
	highthres = min((light*(100+sensitive))/100, 0xfff);

	buf[0] = ((lowthres  >> 0) & 0xff);
	buf[1] = ((lowthres  >> 8) & 0x0f) | ((highthres << 4) & 0xf0);
	buf[2] = ((highthres >> 4) & 0xff);

	error = i2c_smbus_write_i2c_block_data(client,
			ISL29028_ALSIR_TH1, sizeof(buf), buf);
	if (error < 0)
		dev_err(&client->dev, "fail to set als threshold range.");

	return error;
}

static int set_prox_threshold(struct isl29028_data *data,
		u8 lowthres, u8 highthres)
{
	u8			buf[2];
	int			error	= 0;
	struct i2c_client	*client	= data->client;

	buf[0] = lowthres;
	buf[1] = highthres;

	error = i2c_smbus_write_i2c_block_data(client,
			ISL29028_PROX_LT, sizeof(buf), buf);
	if (error < 0)
		dev_err(&client->dev, "fail to set prox threshold range.");

	return error;
}

static bool prox_active(struct isl29028_data *data)
{
	return data->prox_opened && data->prox_enabled;
}

static bool als_active(struct isl29028_data *data)
{
	return data->als_opened && data->als_enabled &&
		!data->suspended && !data->early_suspended &&
		(!prox_active(data) || !data->als_temp_disable);
}

static bool irq_active(struct isl29028_data *data)
{
	return !data->dump_period && !data->suspended &&
		(als_active(data) || prox_active(data));
}

static int irq_trigger_type(struct isl29028_data *data)
{
	return prox_active(data) && data->irq_high_active ?
			IRQF_TRIGGER_HIGH : IRQF_TRIGGER_LOW;
}

#define DUMP_REGISTER(data, reg) \
		dev_info(&data->client->dev, "%s\t: %02x", #reg, \
			i2c_smbus_read_byte_data(data->client, reg))

static void dump_register(struct isl29028_data *data)
{
	DUMP_REGISTER(data, ISL29028_CONFIGURE);
	DUMP_REGISTER(data, ISL29028_INTERRUPT);
	DUMP_REGISTER(data, ISL29028_PROX_LT);
	DUMP_REGISTER(data, ISL29028_PROX_HT);
	DUMP_REGISTER(data, ISL29028_ALSIR_TH1);
	DUMP_REGISTER(data, ISL29028_ALSIR_TH1);
	DUMP_REGISTER(data, ISL29028_ALSIR_TH3);
	DUMP_REGISTER(data, ISL29028_PROX_DATA);
	DUMP_REGISTER(data, ISL29028_ALSIR_DT1);
	DUMP_REGISTER(data, ISL29028_ALSIR_DT2);
}

static int update_device(struct isl29028_data *data)
{
	int				error	= 0;
	struct i2c_client		*client	= data->client;
	struct isl29028_platform_data	*pdata	= client->dev.platform_data;

	/* set sensor threshold */
	if (als_active(data)) {
		error = set_als_threshold(data,
				data->als_light, data->als_sensitive);
		if (error < 0)
			goto exit;
	}

	if (prox_active(data)) {
		error = set_prox_threshold(data,
			data->prox_null_value + pdata->prox_lowthres_offset,
			(data->prox_null_value + pdata->prox_lowthres_offset +
			pdata->prox_threswindow));
		if (error < 0)
			goto exit;
	}

	/* set configure register */
	error = i2c_smbus_write_byte_data(client, ISL29028_CONFIGURE,
			get_reg_config(data->prox_period, als_active(data),
				prox_active(data), data->als_highrange));
	if (error < 0) {
		dev_err(&client->dev, "fail to set configure register.");
		goto exit;
	}

	/* set interrupt register, also acknowledge pending request
	   note: prox flag just clear when psensor is disabled since
	   hardware can auto clear this flag when the object is far
	   from it in active state. */
	if (prox_active(data))
		error = i2c_smbus_write_byte_data(client,
				ISL29028_INTERRUPT, ISL29028_ACTIVE_INTR);
	else
		error = i2c_smbus_write_byte_data(client,
				ISL29028_INTERRUPT, ISL29028_INACTIVE_INTR);
	if (error < 0) {
		dev_err(&client->dev, "fail to set interrupt register.");
		goto exit;
	}

	/* change irq configuration */
	error = irq_set_irq_type(client->irq, irq_trigger_type(data));
	if (error < 0) {
		dev_err(&client->dev, "fail to set irq type.");
		goto exit;
	}

	error = update_irq_wake(data, prox_active(data));
	if (error < 0)
		goto exit;

	update_irq_state(data, irq_active(data));

	/* wait the initial sampling finish except interrupt handle */
	if ((als_active(data) || prox_active(data)) && !data->irq_process)
		msleep(100);

	/* schedule period work if need */
	if (data->dump_period) {
		schedule_delayed_work(&data->delayed_work,
			data->dump_period * HZ / 1000);
	} else
		cancel_delayed_work(&data->delayed_work);

	if (data->dump_register)
		dump_register(data);

exit:
	return error;
}

static int reset_device(struct isl29028_data *data)
{
	int				error	= 0;
	struct i2c_client		*client	= data->client;

	/* issue reset command sequence */
	error = i2c_smbus_write_byte_data(client, ISL29028_CONFIGURE, 0x00);
	if (error < 0)
		goto err;

	error = i2c_smbus_write_byte_data(client, ISL29028_TEST2, 0x29);
	if (error < 0)
		goto err;

	error = i2c_smbus_write_byte_data(client, ISL29028_TEST1, 0x00);
	if (error < 0)
		goto err;

	error = i2c_smbus_write_byte_data(client, ISL29028_TEST2, 0x00);
	if (error < 0)
		goto err;

	msleep(1); /* wait reset finish */
	return error;

err:
	dev_err(&client->dev, "fail to reset device.");
	return error;
}

/* sysfs interface */
static ssize_t isl29028_show_dump_period(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	unsigned long		dump_period;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	mutex_lock(&data->mutex);
	dump_period = data->dump_period;
	mutex_unlock(&data->mutex);

	return scnprintf(buf, PAGE_SIZE, "%lu\n", dump_period);
}

static ssize_t isl29028_store_dump_period(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t cnt)
{
	int			error;
	unsigned long		dump_period;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	error = strict_strtoul(buf, 0, &dump_period);
	if (error >= 0) {
		mutex_lock(&data->mutex);
		data->dump_period = dump_period;
		error = update_device(data);
		mutex_unlock(&data->mutex);
	}

	return error < 0 ? error : cnt;
}

static ssize_t isl29028_show_dump_output(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	unsigned long		dump_output;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	mutex_lock(&data->mutex);
	dump_output = data->dump_output;
	mutex_unlock(&data->mutex);

	return scnprintf(buf, PAGE_SIZE, "%lu\n", dump_output);
}

static ssize_t isl29028_store_dump_output(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t cnt)
{
	int			error;
	unsigned long		dump_output;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	error = strict_strtoul(buf, 0, &dump_output);
	if (error >= 0) {
		mutex_lock(&data->mutex);
		data->dump_output = dump_output;
		error = update_device(data);
		mutex_unlock(&data->mutex);
	}

	return error < 0 ? error : cnt;
}

static ssize_t isl29028_show_dump_register(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	unsigned long		dump_register;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	mutex_lock(&data->mutex);
	dump_register = data->dump_register;
	mutex_unlock(&data->mutex);

	return scnprintf(buf, PAGE_SIZE, "%lu\n", dump_register);
}

static ssize_t isl29028_store_dump_register(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t cnt)
{
	int			error;
	unsigned long		dump_register;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	error = strict_strtoul(buf, 0, &dump_register);
	if (error >= 0) {
		mutex_lock(&data->mutex);
		data->dump_register = dump_register;
		error = update_device(data);
		mutex_unlock(&data->mutex);
	}

	return error < 0 ? error : cnt;
}

static ssize_t isl29028_show_als_enable(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	unsigned long		enabled;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	mutex_lock(&data->mutex);
	enabled = data->als_enabled;
	mutex_unlock(&data->mutex);

	return scnprintf(buf, PAGE_SIZE, "%lu\n", enabled);
}

static ssize_t isl29028_store_als_enable(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t cnt)
{
	int			error;
	unsigned long		enabled;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	error = strict_strtoul(buf, 0, &enabled);
	if (error >= 0) {
		mutex_lock(&data->mutex);
		data->als_enabled = enabled;
		error = update_device(data);
		mutex_unlock(&data->mutex);
	}

	return error < 0 ? error : cnt;
}

static ssize_t isl29028_show_als_period(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	/* isl29028 just support 800ms fix period */
	return scnprintf(buf, PAGE_SIZE, "%lu\n", 800000000UL);
}

static ssize_t isl29028_store_als_period(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t cnt)
{
	int		error;
	unsigned long	period;

	/* period is fix in isl29028, just check format */
	error = strict_strtoul(buf, 0, &period);
	return error < 0 ? error : cnt;
}

static ssize_t isl29028_show_als_range(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	unsigned long		range;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	mutex_lock(&data->mutex);
	range = data->als_highrange;
	mutex_unlock(&data->mutex);

	return scnprintf(buf, PAGE_SIZE, "%lu\n", range);
}

static ssize_t isl29028_store_als_range(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t cnt)
{
	int			error;
	unsigned long		range;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	error = strict_strtoul(buf, 0, &range);
	if (error >= 0) {
		mutex_lock(&data->mutex);
		/* 0==>low range 1==>high range 2==>auto range */
		data->als_highrange = (range == 1);
		data->als_autorange = (range == 2);
		error = update_device(data);
		mutex_unlock(&data->mutex);
	}

	return error < 0 ? error : cnt;
}

static ssize_t isl29028_show_als_sensitive(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	unsigned long		sensitive;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	mutex_lock(&data->mutex);
	sensitive = data->als_sensitive;
	mutex_unlock(&data->mutex);

	return scnprintf(buf, PAGE_SIZE, "%lu\n", sensitive);
}

static ssize_t isl29028_store_als_sensitive(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t cnt)
{
	int			error;
	unsigned long		sensitive;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	error = strict_strtoul(buf, 0, &sensitive);
	if (error >= 0) {
		mutex_lock(&data->mutex);
		data->als_sensitive = sensitive;
		error = update_device(data);
		mutex_unlock(&data->mutex);
	}

	return error < 0 ? error : cnt;
}

static ssize_t isl29028_show_prox_enable(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	unsigned long		enabled;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	mutex_lock(&data->mutex);
	enabled = data->prox_enabled;
	mutex_unlock(&data->mutex);

	return scnprintf(buf, PAGE_SIZE, "%lu\n", enabled);
}

static ssize_t isl29028_store_prox_enable(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t cnt)
{
	int			error;
	unsigned long		enabled;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	error = strict_strtoul(buf, 0, &enabled);
	if (error >= 0) {
		mutex_lock(&data->mutex);
		data->prox_enabled = enabled;
		error = update_device(data);
		mutex_unlock(&data->mutex);
	}

	return error < 0 ? error : cnt;
}

static ssize_t isl29028_show_prox_period(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	unsigned long		period;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	mutex_lock(&data->mutex);
	period = data->prox_period;
	mutex_unlock(&data->mutex);

	return scnprintf(buf, PAGE_SIZE, "%lu\n", period);
}

static ssize_t isl29028_store_prox_period(struct device *dev,
		struct device_attribute *attr,	const char *buf, size_t cnt)
{
	int			error;
	unsigned long		period;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	error = strict_strtoul(buf, 0, &period);
	if (error >= 0) {
		mutex_lock(&data->mutex);
		data->prox_period = adjust_prox_period(period);
		error = update_device(data);
		mutex_unlock(&data->mutex);
	}

	return error < 0 ? error : cnt;
}

static ssize_t isl29028_show_prox_null_value(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	unsigned long		null_value;
	struct isl29028_data	*data;

	data = dev_get_drvdata(dev);

	mutex_lock(&data->mutex);
	null_value = data->prox_null_value;
	mutex_unlock(&data->mutex);

	return scnprintf(buf, PAGE_SIZE, "%lu\n", null_value);
}


static ssize_t isl29028_store_prox_null_value(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t cnt)
{
	int			error;
	unsigned long		null_value;
	struct isl29028_data	*data;
	struct isl29028_platform_data *pdata;

	data = dev_get_drvdata(dev);
	pdata = data->client->dev.platform_data;

	error = strict_strtoul(buf, 0, &null_value);
	if (error >= 0) {
		mutex_lock(&data->mutex);
		null_value = adjust_prox_null_value(null_value, pdata);
		if (null_value != 255) {
			data->prox_null_value = null_value;
			error = update_device(data);
		} else {
			error = -1;
		}
		mutex_unlock(&data->mutex);
	}

	return error < 0 ? error : cnt;
}


static struct device_attribute dev_attr_dump_period =
	__ATTR(dump_period , S_IWUSR | S_IRUGO,
		isl29028_show_dump_period, isl29028_store_dump_period);
static struct device_attribute dev_attr_dump_output =
	__ATTR(dump_output, S_IWUSR | S_IRUGO,
		isl29028_show_dump_output, isl29028_store_dump_output);
static struct device_attribute dev_attr_dump_register =
	__ATTR(dump_register, S_IWUSR | S_IRUGO,
		isl29028_show_dump_register, isl29028_store_dump_register);

static struct device_attribute dev_attr_als_enable =
	__ATTR(enable, S_IWUSR | S_IRUGO,
		isl29028_show_als_enable, isl29028_store_als_enable);
static struct device_attribute dev_attr_als_poll_delay =
	__ATTR(poll_delay, S_IWUSR | S_IRUGO,
		isl29028_show_als_period, isl29028_store_als_period);
static struct device_attribute dev_attr_als_range =
	__ATTR(range, S_IWUSR | S_IRUGO,
		isl29028_show_als_range , isl29028_store_als_range);
static struct device_attribute dev_attr_als_sensitive =
	__ATTR(sensitive, S_IWUSR | S_IRUGO,
		isl29028_show_als_sensitive, isl29028_store_als_sensitive);

static struct attribute *isl29028_als_attrs[] = {
	&dev_attr_dump_period.attr   ,
	&dev_attr_dump_output.attr   ,
	&dev_attr_dump_register.attr ,
	&dev_attr_als_enable.attr    ,
	&dev_attr_als_poll_delay.attr,
	&dev_attr_als_range.attr     ,
	&dev_attr_als_sensitive.attr ,
	NULL
};

static struct attribute_group isl29028_als_attr_grp = {
	.attrs = isl29028_als_attrs,
};

static const struct attribute_group *isl29028_als_attr_grps[] = {
	&isl29028_als_attr_grp,
	NULL
};

static struct device_attribute dev_attr_prox_enable =
	__ATTR(enable, S_IWUSR | S_IRUGO,
		isl29028_show_prox_enable, isl29028_store_prox_enable);
static struct device_attribute dev_attr_prox_poll_delay =
	__ATTR(poll_delay, S_IWUSR | S_IRUGO,
		isl29028_show_prox_period, isl29028_store_prox_period);
static struct device_attribute dev_attr_prox_null_value =
	__ATTR(null_value, S_IWUSR | S_IRUGO,
		isl29028_show_prox_null_value, isl29028_store_prox_null_value);


static struct attribute *isl29028_prox_attrs[] = {
	&dev_attr_dump_period.attr    ,
	&dev_attr_dump_output.attr    ,
	&dev_attr_dump_register.attr  ,
	&dev_attr_prox_enable.attr    ,
	&dev_attr_prox_poll_delay.attr,
	&dev_attr_prox_null_value.attr,
	NULL
};

static struct attribute_group isl29028_prox_attr_grp = {
	.attrs = isl29028_prox_attrs,
};

static const struct attribute_group *isl29028_prox_attr_grps[] = {
	&isl29028_prox_attr_grp,
	NULL
};

/* input device driver interface */
static int isl29028_als_open(struct input_dev *dev)
{
	int			error = 0;
	struct isl29028_data	*data = input_get_drvdata(dev);

	mutex_lock(&data->mutex);
	data->als_opened = true;
	error = update_device(data);
	mutex_unlock(&data->mutex);

	return error;
}

static void isl29028_als_close(struct input_dev *dev)
{
	struct isl29028_data *data = input_get_drvdata(dev);

	mutex_lock(&data->mutex);
	data->als_opened = false;
	update_device(data);
	mutex_unlock(&data->mutex);
}

static int isl29028_prox_open(struct input_dev *dev)
{
	int			error = 0;
	struct isl29028_data	*data = input_get_drvdata(dev);

	mutex_lock(&data->mutex);
	data->prox_opened = true;
	error = update_device(data);
	mutex_unlock(&data->mutex);

	return error;
}

static void isl29028_prox_close(struct input_dev *dev)
{
	struct isl29028_data *data = input_get_drvdata(dev);

	mutex_lock(&data->mutex);
	data->prox_opened = false;
	update_device(data);
	mutex_unlock(&data->mutex);
}

/* interrupt handler */
static irqreturn_t isl29028_interrupt(int irq, void *dev)
{
	u8				prox;
	u16				light;
	int				value;
	int				count;
	struct isl29028_data		*data;
	struct isl29028_platform_data	*pdata;

	data = dev;
	pdata = data->client->dev.platform_data;
	mutex_lock(&data->mutex);
	data->irq_process = true;

	/* collect ambient light */
	if (als_active(data)) {
		light = read_als_output(data);
		value = scale_als_output(light,
				data->als_highrange, pdata->als_factor);

		data->als_light = light;
		data->als_highrange = adjust_als_range(light,
				data->als_autorange, data->als_highrange,
				pdata->als_lowthres, pdata->als_highthres);

		input_report_abs(data->als_input, ABS_MISC, value);
		input_sync(data->als_input);

		if (data->dump_output) {
			dev_info(&data->client->dev,
				"light=%04hu(%07dmLux).", light, value);
		}
	}

	/* collect proximity output */
	if (prox_active(data)) {
		prox = read_prox_output(data);
		data->prox_object_near = (prox >= (data->prox_null_value +
			pdata->prox_lowthres_offset +
			pdata->prox_threswindow));

		input_report_abs(data->prox_input,
			ABS_DISTANCE, data->prox_object_near ? 0 : 1);

		input_report_abs(data->prox_input,
			ABS_MISC, prox);

		input_sync(data->prox_input);
		wake_lock_timeout(&data->wake_lock, HZ / 2);

		if (data->dump_output) {
			dev_info(&data->client->dev, "prox=%03hhu(%s).",
				prox, data->prox_object_near ? "near" : "far");
		}
	}

	/* try acknowledge interrupt three time before fail */
	for (count = 0; count < 3; count++) {
		if (update_device(data) >= 0)
			break;

		dev_err(&data->client->dev,
			"fail to acknowledge interrupt(%d).", count);
		msleep(1);
	}

	if (count == 3) /* disable irq to avoid irq storm */
		disable_irq_nosync(data->client->irq);

	data->irq_process = false;
	mutex_unlock(&data->mutex);
	return IRQ_HANDLED;
}

/* work for period dump sensor output */
static void isl29028_work(struct work_struct *work)
{
	struct isl29028_data *data = container_of(
		to_delayed_work(work), struct isl29028_data, delayed_work);

	/* reuse the interrupt handler logic */
	isl29028_interrupt(data->client->irq, data);
}

/* i2c client driver interface */
static void teardown_als_input(struct isl29028_data *data, bool unreg)
{
	if (data->als_input) {
		kfree(data->als_input->phys);
		if (unreg)
			input_unregister_device(data->als_input);
		else
			input_free_device(data->als_input);
		data->als_input = NULL;
	}
}

static int setup_als_input(struct isl29028_data *data)
{
	int				error = 0;
	bool				unreg = false;
	struct isl29028_platform_data	*pdata;

	pdata = data->client->dev.platform_data;

	data->als_input = input_allocate_device();
	if (data->als_input == NULL) {
		dev_err(&data->client->dev, "fail to allocate als input.");
		error = -ENOMEM;
		goto exit_teardown_input;
	}
	input_set_drvdata(data->als_input, data);

	data->als_input->name		= "lightsensor";
	data->als_input->open		= isl29028_als_open;
	data->als_input->close		= isl29028_als_close;
	data->als_input->dev.groups	= isl29028_als_attr_grps;

	input_set_capability(data->als_input, EV_ABS, ABS_MISC);
	input_set_abs_params(data->als_input,
		ABS_MISC, pdata->als_factor*ISL29028_ALS_MIN,
		pdata->als_factor*ISL29028_ALS_MAX, 0, 0);

	error = input_register_device(data->als_input);
	unreg = (error >= 0);
	if (error < 0) {
		dev_err(&data->client->dev, "fail to register als input.");
		goto exit_teardown_input;
	}

	data->als_input->phys =
		kobject_get_path(&data->als_input->dev.kobj, GFP_KERNEL);
	if (data->als_input->phys == NULL) {
		dev_err(&data->client->dev, "fail to get als sysfs path.");
		error = -ENOMEM;
		goto exit_teardown_input;
	}

	goto exit; /* all is fine */

exit_teardown_input:
	teardown_als_input(data, unreg);
exit:
	return error;
}

static void teardown_prox_input(struct isl29028_data *data, bool unreg)
{
	if (data->prox_input) {
		kfree(data->prox_input->phys);
		if (unreg)
			input_unregister_device(data->prox_input);
		else
			input_free_device(data->prox_input);
		data->prox_input = NULL;
	}
}

static int setup_prox_input(struct isl29028_data *data)
{
	int  error = 0;
	bool unreg = false;

	data->prox_input = input_allocate_device();
	if (data->prox_input == NULL) {
		dev_err(&data->client->dev, "fail to allocate prox input.");
		error = -ENOMEM;
		goto exit_teardown_input;
	}
	input_set_drvdata(data->prox_input, data);

	data->prox_input->name		= "proximity";
	data->prox_input->open		= isl29028_prox_open;
	data->prox_input->close		= isl29028_prox_close;
	data->prox_input->dev.groups	= isl29028_prox_attr_grps;

	input_set_capability(data->prox_input, EV_ABS, ABS_DISTANCE);
	input_set_abs_params(data->prox_input, ABS_DISTANCE,
			ISL29028_PROX_MIN, ISL29028_PROX_MAX, 0, 0);
	input_report_abs(data->prox_input, ABS_DISTANCE, 1);

	input_set_capability(data->prox_input, EV_ABS, ABS_MISC);
	input_set_abs_params(data->prox_input, ABS_MISC,
			ISL29028_PROX_MISC_MIN, ISL29028_PROX_MISC_MAX, 0, 0);

	error = input_register_device(data->prox_input);
	unreg = (error >= 0);
	if (error < 0) {
		dev_err(&data->client->dev, "fail to register prox input.");
		goto exit_teardown_input;
	}

	data->prox_input->phys =
		kobject_get_path(&data->prox_input->dev.kobj, GFP_KERNEL);
	if (data->prox_input->phys == NULL) {
		dev_err(&data->client->dev, "fail to get prox sysfs path.");
		error = -ENOMEM;
		goto exit_teardown_input;
	}

	goto exit; /* all is fine */

exit_teardown_input:
	teardown_prox_input(data, unreg);
exit:
	return error;
}

static int isl29028_setup(struct isl29028_data *data)
{
	struct isl29028_platform_data *pdata = data->client->dev.platform_data;
	if (pdata->setup)
		return pdata->setup(data->client, pdata);
	else
		return 0;
}

static int isl29028_teardown(struct isl29028_data *data)
{
	struct isl29028_platform_data *pdata = data->client->dev.platform_data;
	if (pdata->teardown)
		return pdata->teardown(data->client, pdata);
	else
		return 0;
}

#if defined(CONFIG_PM)
static int isl29028_suspend(struct device *dev)
{
	int			error = 0;
	struct isl29028_data	*data = dev_get_drvdata(dev);

	mutex_lock(&data->mutex);
	data->suspended = true;
	error = update_device(data);
	mutex_unlock(&data->mutex);

	return error;
}

static int isl29028_resume(struct device *dev)
{
	int			error = 0;
	struct isl29028_data	*data = dev_get_drvdata(dev);

	mutex_lock(&data->mutex);
	data->suspended = false;
	error = update_device(data);
	mutex_unlock(&data->mutex);

	return error;
}

static const struct dev_pm_ops isl29028_pm_ops = {
	.suspend	= isl29028_suspend,
	.resume		= isl29028_resume,
};
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
static void isl29028_early_suspend(struct early_suspend *h)
{
	struct isl29028_data *data = container_of(h,
			struct isl29028_data, early_suspend);

	mutex_lock(&data->mutex);
	data->early_suspended = true;
	update_device(data);
	mutex_unlock(&data->mutex);
}

static void isl29028_early_resume(struct early_suspend *h)
{
	struct isl29028_data *data = container_of(h,
			struct isl29028_data, early_suspend);

	mutex_lock(&data->mutex);
	data->early_suspended = false;
	update_device(data);
	mutex_unlock(&data->mutex);
}
#endif

static int isl29028_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	int				error;
	struct isl29028_data		*data;
	struct isl29028_platform_data	*pdata;

	pdata = client->dev.platform_data;
	if (pdata == NULL) {
		dev_err(&client->dev, "invalid platform data.");
		error = -EINVAL;
		goto exit;
	}

	/* allocate device status */
	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (data == NULL) {
		dev_err(&client->dev, "fail to allocate isl29028_data.");
		error = -ENOMEM;
		goto exit;
	}
	i2c_set_clientdata(client, data);

	mutex_init(&data->mutex);
	INIT_DELAYED_WORK(&data->delayed_work, isl29028_work);
	wake_lock_init(&data->wake_lock, WAKE_LOCK_SUSPEND, "isl29028");

	data->client		= client;
	data->als_highrange	= (pdata->als_highrange == 1);
	data->als_autorange	= (pdata->als_highrange == 2);
	data->als_sensitive	= pdata->als_sensitive;
	data->prox_period	= adjust_prox_period(pdata->prox_period);
	data->prox_null_value	= pdata->prox_null_value;

	/* run platform setup */
	error = isl29028_setup(data);
	if (error < 0) {
		dev_err(&client->dev, "fail to perform platform setup.");
		goto exit_free_data;
	}

	/* verify device */
	if (i2c_check_functionality(client->adapter, ISL29028_FUNC) == 0) {
		dev_err(&client->dev, "incompatible adapter.");
		error = -ENODEV;
		goto exit_teardown;
	}

	if (i2c_smbus_read_byte_data(client, ISL29028_ID) < 0) {
		dev_err(&client->dev, "fail to detect ISL29028 chip.");
		error = -ENODEV;
		goto exit_teardown;
	}

	/* register to input system */
	error = setup_als_input(data);
	if (error < 0)
		goto exit_teardown;

	error = setup_prox_input(data);
	if (error < 0)
		goto exit_teardown_als_input;

	/* reset device to good state */
	error = reset_device(data);
	if (error < 0)
		goto exit_teardown_prox_input;

	/* setup interrupt */
	error = request_threaded_irq(client->irq, NULL, isl29028_interrupt,
			IRQF_TRIGGER_LOW | IRQF_ONESHOT, "isl29028", data);
	if (error < 0) {
		dev_err(&client->dev, "fail to request irq.");
		goto exit_teardown_prox_input;
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	data->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	data->early_suspend.suspend = isl29028_early_suspend;
	data->early_suspend.resume = isl29028_early_resume;
	register_early_suspend(&data->early_suspend);
#endif
	goto exit; /* all is fine */

exit_teardown_prox_input:
	teardown_prox_input(data, true);
exit_teardown_als_input:
	teardown_als_input(data, true);
exit_teardown:
	isl29028_teardown(data);
exit_free_data:
	wake_lock_destroy(&data->wake_lock);
	kfree(data);
exit:
	return error;
}

static int isl29028_remove(struct i2c_client *client)
{
	struct isl29028_data *data;
	data = i2c_get_clientdata(client);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&data->early_suspend);
#endif
	free_irq(client->irq, data);

	teardown_als_input(data, true);
	teardown_prox_input(data, true);

	isl29028_teardown(data);
	wake_lock_destroy(&data->wake_lock);
	cancel_delayed_work_sync(&data->delayed_work);
	kfree(data);

	return 0;
}

static const struct i2c_device_id isl29028_ids[] = {
	{"isl29028", 0 },
	{/* list end */},
};

MODULE_DEVICE_TABLE(i2c, isl29028_ids);

static struct i2c_driver isl29028_driver = {
	.probe		= isl29028_probe,
	.remove		= isl29028_remove,
	.driver = {
		.name	= "isl29028",
#ifdef CONFIG_PM
		.pm	= &isl29028_pm_ops,
#endif
	},
	.id_table	= isl29028_ids,
};

/* module initialization and termination */
static int __init isl29028_init(void)
{
	return i2c_add_driver(&isl29028_driver);
}

static void __exit isl29028_exit(void)
{
	i2c_del_driver(&isl29028_driver);
}

module_init(isl29028_init);
module_exit(isl29028_exit);

MODULE_AUTHOR("Xiang Xiao <xiaoxiang@xiaomi.com>");
MODULE_DESCRIPTION("ISL29028 light/proximity input driver");
MODULE_LICENSE("GPL");
