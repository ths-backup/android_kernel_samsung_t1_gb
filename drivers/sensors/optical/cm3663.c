/* linux/driver/input/misc/cm3663.c
 * Copyright (C) 2010 Samsung Electronics. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/i2c.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <linux/gpio.h>
#include <linux/wakelock.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/uaccess.h>
#include <linux/cm3663.h>
#include <linux/regulator/consumer.h>
#include <plat/mux.h>

static struct i2c_client *this_client;

#define ALS_BUFFER_NUM	10
#define LIGHT_BUFFER_NUM	5
#define PROX_READ_NUM	40

/* ADDSEL is LOW */
#define REGS_ARA		0x18
#define REGS_ALS_CMD		0x20
#define REGS_ALS_MSB		0x21
#define REGS_INIT		0x22
#define REGS_ALS_LSB		0x23
#define REGS_PS_CMD		0xB0
#define REGS_PS_DATA		0xB1
#define REGS_PS_THD		0xB2

#define PROXIMITY_THRESHOLD	0x0A
#define DEFAULT_POLL_DELAY	200000000

static u8 reg_defaults[8] = {
	0x00, /* ARA: read only register */
	0x00, /* ALS_CMD: als cmd */
	0x00, /* ALS_MSB: read only register */
	0x20, /* INIT: interrupt disable */
	0x00, /* ALS_LSB: read only register */
	0x30, /* PS_CMD: interrupt disable */
	0x00, /* PS_DATA: read only register */
	PROXIMITY_THRESHOLD, /* PS_THD: 10 */
};

static const int adc_table[4] = {
	15,
	150,
	1500,
	15000,
};

enum {
	LIGHT_ENABLED = BIT(0),
	PROXIMITY_ENABLED = BIT(1),
};

/* driver data */
struct cm3663_data {
	struct input_dev *proximity_input_dev;
	struct input_dev *light_input_dev;
	struct i2c_client *i2c_client;
	struct work_struct work_light;
	struct work_struct work_prox;
	struct hrtimer light_timer;
	struct hrtimer prox_timer;
	struct mutex power_lock;
	struct wake_lock prx_wake_lock;
	struct workqueue_struct *light_wq;
	struct workqueue_struct *prox_wq;
	struct class *lightsensor_class;
	struct class *proximity_class;
	struct device *switch_cmd_dev;
	struct device *proximity_dev;
	struct cm3663_platform_data *pdata;
	bool als_buf_initialized;
	bool on;
	int als_value_buf[ALS_BUFFER_NUM];
	int als_index_count;
	int irq;
	int avg[3];
	int light_count;
	int light_buffer;
	ktime_t light_poll_delay;
	ktime_t prox_poll_delay;
	u8 power_state;
	int last_brightness_step;
	u8 prox_val;
};

/* 
 * lux table for auto brightness
 * This driver should send input event when the boundary is changed
 */
static int brightness_step_table[] = { 0, 15, 150, 1500, 15000, 10000000 };
static int adc_step_table[5] = { 0, 12, 40, 490, 7900};

static int n_drv_is = 0;

/* 
 * Reset the sensor by H/W.
 */
static void cm3663_reset(struct cm3663_data *cm3663);

/*
 * Read sensor data via I2C
 * If got an error, this will reset the sensor via reset function.
 */
static int cm3663_i2c_read(struct cm3663_data *cm3663, u8 addr, u8 *val)
{
	int err = 0;
	int retry = 10;
	struct i2c_msg msg[1];
	struct i2c_client *client = cm3663->i2c_client;

	if ((client == NULL) || (!client->adapter))
		return -ENODEV;

	msg->addr = addr >> 1;
	msg->flags = 1;
	msg->len = 1;
	msg->buf = val;

	while (retry--) {
		err = i2c_transfer(client->adapter, msg, 1);
		if (err >= 0)
			return err;
	}

	if (addr == REGS_ARA)
		return 0;

	pr_err("%s: i2c read failed at addr 0x%x: %d, ps_on: %d\n", __func__, addr, err, gpio_get_value(OMAP_GPIO_PS_ON)); 

	//cm3663_reset(cm3663);

	return err;
}

/*
 * Write sensor data via I2C
 * If got an error, this will reset the sensor via reset function.
 */
static int cm3663_i2c_write(struct cm3663_data *cm3663, u8 addr, u8 val)
{
	u8 data = val;
	int err = 0;
	int retry = 10;
	struct i2c_msg msg[1];
	struct i2c_client *client = cm3663->i2c_client;

	if ((client == NULL) || (!client->adapter))
		return -ENODEV;

	msg->addr = addr >> 1;
	msg->flags = 0;
	msg->len = 1;
	msg->buf = &data;

	while (retry--) {
		err = i2c_transfer(client->adapter, msg, 1);
		if (err >= 0)
			return err;
	}
	
	pr_err("%s: i2c write failed at addr 0x%x: %d, ps_on: %d\n", __func__, addr, err, gpio_get_value(OMAP_GPIO_PS_ON));  

	//cm3663_reset(cm3663);

	return err;
}

/*
 * Enable light sensor
 */
static void cm3663_light_enable(struct cm3663_data *cm3663)
{
	u8 tmp;
	cm3663->light_count = 0;
	cm3663->light_buffer = 0;
	cm3663_i2c_read(cm3663, REGS_ARA, &tmp);
	cm3663_i2c_read(cm3663, REGS_ARA, &tmp);
	cm3663_i2c_read(cm3663, REGS_ARA, &tmp);
	cm3663_i2c_write(cm3663, REGS_INIT, reg_defaults[3]);
	cm3663_i2c_write(cm3663, REGS_ALS_CMD, reg_defaults[1]);
	hrtimer_start(&cm3663->light_timer, cm3663->light_poll_delay,
						HRTIMER_MODE_REL);
}

/*
 * Disable light sensor
 */
static void cm3663_light_disable(struct cm3663_data *cm3663)
{
	cm3663_i2c_write(cm3663, REGS_ALS_CMD, 0x01);
	hrtimer_cancel(&cm3663->light_timer);
	cancel_work_sync(&cm3663->work_light);
}

/*
 * Get the adc value from the sensor and 
 * calculate the average except min/max.
 */
static int lightsensor_get_alsvalue(struct cm3663_data *cm3663)
{
	int i = 0;
	int j = 0;
	int value = 0;
	int als_avr_value;
	unsigned int als_total = 0;
	unsigned int als_index = 0;
	unsigned int als_max = 0;
	unsigned int als_min = 0;
	u8 als_high, als_low;

	/* get ALS */
	cm3663_i2c_read(cm3663, REGS_ALS_LSB, &als_low);
	cm3663_i2c_read(cm3663, REGS_ALS_MSB, &als_high);
	als_avr_value = ((als_high << 8) | als_low) * 5;
#if 0
	value = ((als_high << 8) | als_low) * 5;
	als_index = (cm3663->als_index_count++) % ALS_BUFFER_NUM;

	/*ALS buffer initialize (light sensor off ---> light sensor on) */
	if (!cm3663->als_buf_initialized) {
		cm3663->als_buf_initialized = true;
		for (j = 0; j < ALS_BUFFER_NUM; j++)
			cm3663->als_value_buf[j] = value;
	} else
		cm3663->als_value_buf[als_index] = value;

	als_max = cm3663->als_value_buf[0];
	als_min = cm3663->als_value_buf[0];

	for (i = 0; i < ALS_BUFFER_NUM; i++) {
		als_total += cm3663->als_value_buf[i];

		if (als_max < cm3663->als_value_buf[i])
			als_max = cm3663->als_value_buf[i];

		if (als_min > cm3663->als_value_buf[i])
			als_min = cm3663->als_value_buf[i];
	}
	als_avr_value = (als_total-(als_max+als_min))/(ALS_BUFFER_NUM-2);
	
	if (cm3663->als_index_count == ALS_BUFFER_NUM-1)
		cm3663->als_index_count = 0;
	if(als_avr_value<9)
		als_avr_value = 0;
#endif	

	if(als_avr_value<8)
		als_avr_value = 0;

	//printk("lightsensor_get_alsvalue : %d\n", als_avr_value);

	return als_avr_value;
}

static void proxsensor_get_avgvalue(struct cm3663_data *cm3663)
{
	int min = 0, max = 0, avg = 0;
	int i;
	u8 proximity_value = 0;

	for (i = 0; i < PROX_READ_NUM; i++) {
		msleep(40);
		cm3663_i2c_read(cm3663, REGS_PS_DATA, &proximity_value);
		avg += proximity_value;

		if (!i)
			min = proximity_value;
		else if (proximity_value < min)
			min = proximity_value;

		if (proximity_value > max)
			max = proximity_value;
	}
	avg /= PROX_READ_NUM;

	cm3663->avg[0] = min;
	cm3663->avg[1] = avg;
	cm3663->avg[2] = max;
}

/*
 * sysfs interface
 * return the proximity state.
 */
static ssize_t cm3663_proximity_state_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct cm3663_data *cm3663 = dev_get_drvdata(dev);
	return sprintf(buf, "%u\n", cm3663->prox_val <= PROXIMITY_THRESHOLD ? 0 : 1);
}


/*
 * sysfs interface
 * return the adc value of proximity
 */
static ssize_t cm3663_proximity_adc_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct cm3663_data *cm3663 = dev_get_drvdata(dev);
	u8 proximity_value = 0;

	if (!(cm3663->power_state & PROXIMITY_ENABLED)) {
		mutex_lock(&cm3663->power_lock);
		cm3663_i2c_write(cm3663, REGS_PS_CMD, reg_defaults[5]);
		mutex_unlock(&cm3663->power_lock);
	}

	msleep(20);
	cm3663_i2c_read(cm3663, REGS_PS_DATA, &proximity_value);

	if (!(cm3663->power_state & PROXIMITY_ENABLED)) {
		mutex_lock(&cm3663->power_lock);
		cm3663_i2c_write(cm3663, REGS_PS_CMD, 0x01);
		mutex_unlock(&cm3663->power_lock);
	}

	return sprintf(buf, "%d", proximity_value);
}

/*
 * dummy sysfs interface
 */
static ssize_t cm3663_proximity_adc_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t size)
{
	return size;
}

/*
 * sysfs interface
 * return the adc value from light sensor
 */
static ssize_t cm3663_light_sensor_adc_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct cm3663_data *cm3663 = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n", lightsensor_get_alsvalue(cm3663));
}

/*
 * sysfs interface
 * return the lux value calculated using adc/lux table.
 */
static ssize_t cm3663_light_sensor_lux_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct cm3663_data *cm3663 = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n", lightsensor_get_alsvalue(cm3663));	// temporary fix
}

/*
 * sysfs interface
 * show polling interval
 */
static ssize_t cm3663_light_poll_delay_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct cm3663_data *cm3663 = dev_get_drvdata(dev);
	return sprintf(buf, "%lld\n", ktime_to_ns(cm3663->light_poll_delay));
}

/*
 * sysfs interface
 * change polling interval
 */
static ssize_t cm3663_light_poll_delay_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	struct cm3663_data *cm3663 = dev_get_drvdata(dev);
	int64_t new_delay;
	int err;

	err = strict_strtoll(buf, 10, &new_delay);
	if (err < 0)
		return err;

	mutex_lock(&cm3663->power_lock);
	
	if (new_delay != ktime_to_ns(cm3663->light_poll_delay)) {
		cm3663->light_poll_delay = ns_to_ktime(new_delay);
		if (cm3663->power_state & LIGHT_ENABLED) {
			cm3663_light_disable(cm3663);
			cm3663_light_enable(cm3663);
		}
	}
	mutex_unlock(&cm3663->power_lock);

	return size;
}

/*
 * dummy sysfs interface
 * show polling interval
 */
static ssize_t cm3663_proximity_poll_delay_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0\n");
}

/*
 * dummy sysfs interface
 * change polling interval
 */
static ssize_t cm3663_proximity_poll_delay_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t size)
{
	return size;
}

/*
 * sysfs interface
 * return the current power state of light sensor
 */
static ssize_t cm3663_light_enable_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct cm3663_data *cm3663 = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n",
		       (cm3663->power_state & LIGHT_ENABLED) ? 1 : 0);
}

/*
 * sysfs interface
 * return the current power state of proximity
 */
static ssize_t cm3663_proximity_enable_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct cm3663_data *cm3663 = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n",
		       (cm3663->power_state & PROXIMITY_ENABLED) ? 1 : 0);
}

/*
 * sysfs interface
 * Turn on or off the light sensor
 */
static ssize_t cm3663_light_enable_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t size)
{
	struct cm3663_data *cm3663 = dev_get_drvdata(dev);
	int enable = simple_strtoul(buf, NULL,10);
	int enabled = cm3663->power_state & LIGHT_ENABLED;
	
	//printk("[LIGHT SENSOR] %s : new state(%d), old state(%d)\n", __func__, enable, enabled);
	
	if(enable == enabled)
		return size;
		
	mutex_lock(&cm3663->power_lock);
	
	if (enable) {
		if(!(cm3663->power_state)) {
			printk("\n light_power true \n");
			cm3663->pdata->proximity_power(true);
		}	
		cm3663->power_state |= LIGHT_ENABLED;
		cm3663_light_enable(cm3663);
	} else {
		cm3663_light_disable(cm3663);
		cm3663->power_state &= ~LIGHT_ENABLED;
		if(!((cm3663->power_state)&PROXIMITY_ENABLED)) {
			printk("\n light_power false \n");
			//cm3663->pdata->proximity_power(false);
		}
	}
	
	mutex_unlock(&cm3663->power_lock);
	return size;
}

/*
 * sysfs interface
 * Turn on or off the proximity
 */
static ssize_t cm3663_proximity_enable_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	struct cm3663_data *cm3663 = dev_get_drvdata(dev);
	int enable = simple_strtoul(buf, NULL,10);
	int enabled = (cm3663->power_state & PROXIMITY_ENABLED) >> 1;
	u8 tmp = 0;

	printk("[PROXIMITY] %s : new state(%d), old state(%d)\n", __func__, enable, enabled);

	if(enable == enabled)
		return size;
		
	mutex_lock(&cm3663->power_lock);
	
	if (enable) {
		if (!(cm3663->power_state)) {
			printk("\n proximity_power true \n");
			cm3663->pdata->proximity_power(true);
		}

		cm3663->power_state |= PROXIMITY_ENABLED;
		cm3663_i2c_read(cm3663, REGS_ARA, &tmp);
		cm3663_i2c_read(cm3663, REGS_ARA, &tmp);
		cm3663_i2c_write(cm3663, REGS_INIT, reg_defaults[3]);
		cm3663_i2c_write(cm3663, REGS_PS_THD, reg_defaults[7]);
		cm3663_i2c_write(cm3663, REGS_PS_CMD, reg_defaults[5]);
		enable_irq(cm3663->irq);
		//enable_irq_wake(cm3663->irq);
	} else {
		//disable_irq_wake(cm3663->irq);
		disable_irq(cm3663->irq);
		cm3663_i2c_write(cm3663, REGS_PS_CMD, 0x01);
		cm3663->power_state &= ~PROXIMITY_ENABLED;

		if (!(cm3663->power_state)) {
			printk("\n proximity_power false \n");
			//cm3663->pdata->proximity_power(false);
		}
	}
	
	mutex_unlock(&cm3663->power_lock);
	return size;
}

static ssize_t proximity_avg_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct cm3663_data *cm3663 = dev_get_drvdata(dev);

	return sprintf(buf, "%d,%d,%d\n",
		cm3663->avg[0], cm3663->avg[1], cm3663->avg[2]);
}

static ssize_t proximity_avg_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	struct cm3663_data *cm3663 = dev_get_drvdata(dev);
	bool new_value;

	if (sysfs_streq(buf, "1"))
		new_value = true;
	else if (sysfs_streq(buf, "0"))
		new_value = false;
	else {
		pr_err("%s: invalid value %d\n", __func__, *buf);
		return -EINVAL;
	}

	mutex_lock(&cm3663->power_lock);
	if (new_value) {
		if (!(cm3663->power_state & PROXIMITY_ENABLED)) {
			cm3663_i2c_write(cm3663, REGS_PS_CMD, reg_defaults[5]);
		}
		hrtimer_start(&cm3663->prox_timer, cm3663->prox_poll_delay,
							HRTIMER_MODE_REL);
	} else if (!new_value) {
		hrtimer_cancel(&cm3663->prox_timer);
		cancel_work_sync(&cm3663->work_prox);
		if (!(cm3663->power_state & PROXIMITY_ENABLED)) {
			cm3663_i2c_write(cm3663, REGS_PS_CMD, 0x01);
		}
	}
	mutex_unlock(&cm3663->power_lock);

	return size;
}

static struct device_attribute dev_attr_light_poll_delay =
	__ATTR(poll_delay, S_IRUSR | S_IRGRP | S_IWUSR | S_IWGRP, cm3663_light_poll_delay_show, cm3663_light_poll_delay_store);

static struct device_attribute dev_attr_proximity_poll_delay =
	__ATTR(poll_delay, S_IRUSR | S_IRGRP | S_IWUSR | S_IWGRP, cm3663_proximity_poll_delay_show, cm3663_proximity_poll_delay_store);
	
static struct device_attribute dev_attr_light_sensor_lux = 
	__ATTR(lux, S_IRUSR | S_IRGRP, cm3663_light_sensor_lux_show, NULL);

static struct device_attribute dev_attr_proximity_sensor_state = 
	__ATTR(state, S_IRUSR | S_IRGRP, cm3663_proximity_state_show, NULL);

static struct device_attribute dev_attr_light_sensor_adc =
	__ATTR(adc, S_IRUSR | S_IRGRP, cm3663_light_sensor_adc_show, NULL);
	
static struct device_attribute dev_attr_proximity_sensor_adc = 
	__ATTR(adc, S_IRUSR | S_IRGRP | S_IWUSR | S_IWGRP, cm3663_proximity_adc_show, cm3663_proximity_adc_store);

static struct device_attribute dev_attr_light_enable =
	__ATTR(enable, S_IRUSR | S_IRGRP | S_IWUSR | S_IWGRP, cm3663_light_enable_show, cm3663_light_enable_store);

static struct device_attribute dev_attr_proximity_enable =
	__ATTR(enable, S_IRUSR | S_IRGRP | S_IWUSR | S_IWGRP, cm3663_proximity_enable_show, cm3663_proximity_enable_store);

static struct device_attribute dev_attr_proximity_avg =
	__ATTR(prox_avg, S_IRUSR | S_IRGRP | S_IWUSR | S_IWGRP, proximity_avg_show, proximity_avg_store);

/* sysfs attributes for input device */
static struct attribute *light_sysfs_attrs[] = {
	&dev_attr_light_enable.attr,
	&dev_attr_light_poll_delay.attr,
	NULL
};

static struct attribute_group light_attribute_group = {
	.attrs = light_sysfs_attrs,
};

static struct attribute *proximity_sysfs_attrs[] = {
	&dev_attr_proximity_enable.attr,
	&dev_attr_proximity_poll_delay.attr,
	NULL
};

static struct attribute_group proximity_attribute_group = {
	.attrs = proximity_sysfs_attrs,
};

/* 
 * sysfs attributes for additional functions.
 * this interfaces will be located under /sys/class/sensors/xxx
 */
static struct device_attribute *additional_light_attrs[] = {
	&dev_attr_light_sensor_lux,
	&dev_attr_light_sensor_adc,
	NULL,
};

static struct device_attribute *additional_proximity_attrs[] = {
	&dev_attr_proximity_sensor_state,
	&dev_attr_proximity_sensor_adc,
	&dev_attr_proximity_avg,
	NULL,
};

/*
 * work function for light sensor
 * it get the current adc value from the sensor and
 * get lux value using adc/lux table.
 * if there was a change of boundary, it will notify the event.
 */
static void cm3663_work_func_light(struct work_struct *work)
{
	int als;
	struct cm3663_data *cm3663 = container_of(work, struct cm3663_data,
									work_light);
	int i = 0;
	int j = 0;
	int step = 0;
	int final = sizeof(brightness_step_table)/sizeof(int);
	int adc = sizeof(adc_step_table)/sizeof(int);

	als = lightsensor_get_alsvalue(cm3663);   // temporary fix for light sensor
#if 0
	for(; j < adc; j++)
	{
		if((als >= adc_step_table[j]) && (als < adc_step_table[j+1]))
		{
			if(j<(adc-1)) {
				als = adc_step_table[j] + 
					((als - adc_step_table[j])*(adc_step_table[j+1])/(adc_step_table[j+1]-adc_step_table[j]));
			} else {
				als = adc_step_table[j] + (als - adc_step_table[j]);
			}
			break;
		}
	}	

	for(; i < final - 1; i++)
	{
		if((als >= brightness_step_table[i]) && (als < brightness_step_table[i+1]))
		{
			step = i;
			break;
		}
	}

	if(cm3663->last_brightness_step != step)
	{
		if (cm3663->light_count == LIGHT_BUFFER_NUM) {
			input_report_abs(cm3663->light_input_dev, ABS_MISC, als);
			input_sync(cm3663->light_input_dev);
			cm3663->last_brightness_step = step;
			printk(KERN_INFO "[LIGHT SENSOR] Report ALS(%d)\n", als);
		}
		cm3663->light_count++;
		if (cm3663->light_count>LIGHT_BUFFER_NUM)
			cm3663->light_count = 0;

	} else {
	
		cm3663->light_count = 0;
	}
#endif

	for (i = 0; ARRAY_SIZE(adc_table); i++)
		if (als <= adc_table[i])
			break;

	if (cm3663->light_buffer == i) {
		if (cm3663->light_count++ == LIGHT_BUFFER_NUM) {
			input_report_abs(cm3663->light_input_dev,
							ABS_MISC, als+1);
			input_sync(cm3663->light_input_dev);
			cm3663->light_count = 0;
			//printk(KERN_INFO "[LIGHT SENSOR] Report ALS(%d)\n", als);
		}
	} else {
		cm3663->light_buffer = i;
		cm3663->light_count = 0;
	}

}
static void cm3663_work_func_prox(struct work_struct *work)
{
	struct cm3663_data *cm3663 = container_of(work, struct cm3663_data, work_prox);
	
	proxsensor_get_avgvalue(cm3663);
}

/* This function is for light sensor.  It operates every a few seconds.
 * It asks for work to be done on a thread because i2c needs a thread
 * context (slow and blocking) and then reschedules the timer to run again.
 */
static enum hrtimer_restart cm3663_light_timer_func(struct hrtimer *timer)
{
	struct cm3663_data *cm3663
			= container_of(timer, struct cm3663_data, light_timer);
	queue_work(cm3663->light_wq, &cm3663->work_light);
	hrtimer_forward_now(&cm3663->light_timer, cm3663->light_poll_delay);
	return HRTIMER_RESTART;
}

static enum hrtimer_restart cm3663_prox_timer_func(struct hrtimer *timer)
{
	struct cm3663_data *cm3663
			= container_of(timer, struct cm3663_data, prox_timer);
	queue_work(cm3663->prox_wq, &cm3663->work_prox);
	hrtimer_forward_now(&cm3663->prox_timer, cm3663->prox_poll_delay);
	return HRTIMER_RESTART;
}

/* interrupt happened due to transition/change of near/far proximity state */
irqreturn_t cm3663_irq_thread_fn(int irq, void *data)
{
	struct cm3663_data *ip = data;
	u8 val = 1;

	val = gpio_get_value(ip->pdata->irq);
	if (val < 0) {
		pr_err("%s: gpio_get_value error %d\n", __func__, val);
		return IRQ_HANDLED;
	}

	/* for debugging : going to be removed */
	cm3663_i2c_read(ip, REGS_PS_DATA, &ip->prox_val);
	printk("%s: proximity value = %d\n", __func__, ip->prox_val);

	/* 0 is close, 1 is far */
	input_report_abs(ip->proximity_input_dev, ABS_DISTANCE, val);
	input_sync(ip->proximity_input_dev);
	wake_lock_timeout(&ip->prx_wake_lock, 3*HZ);

	return IRQ_HANDLED;
}

static int cm3663_setup_irq(struct cm3663_data *cm3663)
{
	int rc = -EIO;
	int irq;

	rc = gpio_request(cm3663->pdata->irq, "cm3663_irq");
	if (rc < 0) {
		pr_err("%s: gpio %d request failed (%d)\n",
			__func__, cm3663->pdata->irq, rc);
		return rc;
	}

	rc = gpio_direction_input(cm3663->pdata->irq);
	if (rc < 0) {
		pr_err("%s: failed to set gpio %d as input (%d)\n",
			__func__, cm3663->pdata->irq, rc);
		goto err_gpio_direction_input;
	}

	irq = gpio_to_irq(cm3663->pdata->irq);
	rc = request_threaded_irq(irq, NULL, cm3663_irq_thread_fn,
			 IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
//			IRQF_TRIGGER_LOW |IRQF_TRIGGER_HIGH |IRQF_ONESHOT,
			 "proximity_int", cm3663);
	if (rc < 0) {
		pr_err("%s: request_irq(%d) failed for gpio %d (%d)\n",
			__func__, irq,
			cm3663->pdata->irq, rc);
		goto err_request_irq;
	}

	/* start with interrupts disabled */
	disable_irq(irq);
	cm3663->irq = irq;

	return 0;

err_request_irq:
err_gpio_direction_input:
	gpio_free(cm3663->pdata->irq);

	return rc;
}

/*
 * reset this sensor 
 */
static void cm3663_reset(struct cm3663_data *cm3663)
{
	unsigned char tmp;
	
	hrtimer_cancel(&cm3663->light_timer);
	//cancel_work_sync(&cm3663->work_light);

	cm3663->pdata->proximity_power(false);
	msleep(50);
	cm3663->pdata->proximity_power(true);
	msleep(50);
	
	if (cm3663->power_state & LIGHT_ENABLED) {
		cm3663_light_enable(cm3663);
	}

	if (cm3663->power_state & PROXIMITY_ENABLED) {
		cm3663_i2c_read(cm3663, REGS_ARA, &tmp);
		cm3663_i2c_read(cm3663, REGS_ARA, &tmp);
		cm3663_i2c_write(cm3663, REGS_INIT, reg_defaults[3]);
		cm3663_i2c_write(cm3663, REGS_PS_THD, reg_defaults[7]);
		cm3663_i2c_write(cm3663, REGS_PS_CMD, reg_defaults[5]);
	}
	
}

static const struct file_operations light_fops = {
	.owner  = THIS_MODULE,
};

static struct miscdevice light_device = {
    .minor  = MISC_DYNAMIC_MINOR,
    .name   = "light",
    .fops   = &light_fops,
};


static struct device *light_sensor_device;
static struct device *proximity_sensor_device;
extern struct class *sensors_class;
extern int sensors_register(struct device *dev, void * drvdata, struct device_attribute *attributes[], char *name);

/*
 * probe function
 */
static int cm3663_i2c_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	int ret = -ENODEV;
	struct input_dev *input_dev;
	struct cm3663_data *cm3663;
	u8 tmp;
	int rt = 0;
	int cnt = 5;
	int i = 0;
	int adc = sizeof(adc_step_table)/sizeof(int);
	
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("%s: i2c functionality check failed!\n", __func__);
		return ret;
	}

	cm3663 = kzalloc(sizeof(struct cm3663_data), GFP_KERNEL);
	if (!cm3663) {
		pr_err("%s: failed to alloc memory for module data\n", __func__);
		return -ENOMEM;
	}

	cm3663->pdata = client->dev.platform_data;
	cm3663->i2c_client = client;
	i2c_set_clientdata(client, cm3663);
	this_client=client;

	/* wake lock init */
	wake_lock_init(&cm3663->prx_wake_lock, WAKE_LOCK_SUSPEND, "prx_wake_lock");
	
	mutex_init(&cm3663->power_lock);

	ret = cm3663_setup_irq(cm3663);
	if (ret) {
		pr_err("%s: could not setup irq\n", __func__);
		goto err_setup_irq;
	}

	/* allocate proximity input_device */
	input_dev = input_allocate_device();
	if (!input_dev) {
		pr_err("%s: could not allocate input device\n", __func__);
		goto err_input_allocate_device_proximity;
	}
	cm3663->proximity_input_dev = input_dev;
	input_set_drvdata(input_dev, cm3663);
	input_dev->name = "proximity_sensor";
	input_set_capability(input_dev, EV_ABS, ABS_DISTANCE);
	input_set_abs_params(input_dev, ABS_DISTANCE, 0, 1, 0, 0);

	ret = input_register_device(input_dev);
	if (ret < 0) {
		pr_err("%s: could not register input device\n", __func__);
		input_free_device(input_dev);
		goto err_input_register_device_proximity;
	}
	ret = sysfs_create_group(&input_dev->dev.kobj,
				 &proximity_attribute_group);
	if (ret) {
		pr_err("%s: could not create sysfs group\n", __func__);
		goto err_sysfs_create_group_proximity;
	}

	/* hrtimer settings.  we poll for light values using a timer. */
	hrtimer_init(&cm3663->light_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	cm3663->light_poll_delay = ns_to_ktime(200 * NSEC_PER_MSEC);
	cm3663->light_timer.function = cm3663_light_timer_func;
	/* prox_timer settings. we poll for light values using a timer. */
	hrtimer_init(&cm3663->prox_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	cm3663->prox_poll_delay = ns_to_ktime(2000 * NSEC_PER_MSEC);
	cm3663->prox_timer.function = cm3663_prox_timer_func;

	/* the timer just fires off a work queue request.  we need a thread
	   to read the i2c (can be slow and blocking). */
	cm3663->light_wq = create_singlethread_workqueue("cm3663_light_wq");
	if (!cm3663->light_wq) {
		ret = -ENOMEM;
		pr_err("%s: could not create light workqueue\n", __func__);
		goto err_create_light_workqueue;
	}
	cm3663->prox_wq = create_singlethread_workqueue("cm3663_prox_wq");
	if (!cm3663->prox_wq) {
		ret = -ENOMEM;
		pr_err("%s: could not create prox workqueue\n", __func__);
		goto err_create_prox_workqueue;
	}

	/* this is the thread function we run on the work queue */
	INIT_WORK(&cm3663->work_light, cm3663_work_func_light);
	INIT_WORK(&cm3663->work_prox, cm3663_work_func_prox);

	/* allocate lightsensor-level input_device */
	input_dev = input_allocate_device();
	if (!input_dev) {
		pr_err("%s: could not allocate input device\n", __func__);
		ret = -ENOMEM;
		goto err_input_allocate_device_light;
	}
	input_set_drvdata(input_dev, cm3663);
	input_dev->name = "light_sensor";
	input_set_capability(input_dev, EV_ABS, ABS_MISC);
	input_set_abs_params(input_dev, ABS_MISC, 0, 1, 0, 0);

	ret = input_register_device(input_dev);
	if (ret < 0) {
		pr_err("%s: could not register input device\n", __func__);
		input_free_device(input_dev);
		goto err_input_register_device_light;
	}
	cm3663->light_input_dev = input_dev;
	ret = sysfs_create_group(&input_dev->dev.kobj,
				 &light_attribute_group);
	if (ret) {
		pr_err("%s: could not create sysfs group\n", __func__);
		goto err_sysfs_create_group_light;
	}
	
	ret = sensors_register(light_sensor_device, cm3663, additional_light_attrs, "light_sensor");
	if(ret) {
		pr_err("%s: cound not register sensor device\n", __func__);
		goto err_sysfs_create_group_light;
	}
	
	ret = sensors_register(proximity_sensor_device, cm3663, additional_proximity_attrs, "proximity_sensor");
	if(ret) {
		pr_err("%s: cound not register sensor device\n", __func__);
		goto err_sysfs_create_group_light;
	}

	/* print inital proximity value with no contact */
	mutex_lock(&cm3663->power_lock);

	cm3663_i2c_read(cm3663, REGS_ARA, &tmp);
	cm3663_i2c_read(cm3663, REGS_ARA, &tmp);
	
	do{
		rt=cm3663_i2c_write(cm3663, REGS_INIT, reg_defaults[3]);
		cnt--;
		printk("cm3663_i2c_write cnt =%d ret = %d \n", cnt, rt);
		if (!cnt && rt<0) {
		  n_drv_is = 1;
			goto err_i2c_failed;
		}
	}while(rt<0);
		
	cm3663_i2c_write(cm3663, REGS_PS_THD, reg_defaults[7]);
	cm3663_i2c_write(cm3663, REGS_PS_CMD, reg_defaults[5]);
	msleep(100);

	cm3663_i2c_read(cm3663, REGS_PS_DATA, &tmp);
	pr_err("%s: initial proximity value = %d\n", __func__, tmp);

	cm3663_i2c_write(cm3663, REGS_PS_CMD, 0x01);
	mutex_unlock(&cm3663->power_lock);

	/* set initial proximity value as 1 */
	input_report_abs(cm3663->proximity_input_dev, ABS_DISTANCE, 1);
	input_sync(cm3663->proximity_input_dev);

#if 0
	if(cm3663->pdata->adc_step_table[1]) {
		for(i=0; i<adc;i++) {
			adc_step_table[i] = cm3663->pdata->adc_step_table[i];
			printk("[cm3663] probe get adc_step_table step:%d, value : %d \n", i, adc_step_table[i]);
		}
	} else 
		printk("[cm3663] probe use defult adc_step_table step \n");
#endif
	printk("CM3663 probe ok!!!n");
	goto done;

/* error, unwind it all */
err_i2c_failed:
	sysfs_remove_group(&cm3663->light_input_dev->dev.kobj,
			   &light_attribute_group);
err_sysfs_create_group_light:	
	input_unregister_device(cm3663->light_input_dev);
err_input_register_device_light:
err_input_allocate_device_light:
	destroy_workqueue(cm3663->prox_wq);
err_create_prox_workqueue:	
	destroy_workqueue(cm3663->light_wq);
err_create_light_workqueue:
	sysfs_remove_group(&cm3663->proximity_input_dev->dev.kobj,
			   &proximity_attribute_group);
err_sysfs_create_group_proximity:
	input_unregister_device(cm3663->proximity_input_dev);
err_input_register_device_proximity:
err_input_allocate_device_proximity:
err_setup_irq:
	//free_irq(cm3663->pdata->irq, 0);
	mutex_destroy(&cm3663->power_lock);
	wake_lock_destroy(&cm3663->prx_wake_lock);
	kfree(cm3663);
	printk("CM3663 probe fail!!!\n");

done:
	return ret;
}

static int cm3663_suspend(struct device *dev)
{
	/* We disable power only if proximity is disabled.  If proximity
	   is enabled, we leave power on because proximity is allowed
	   to wake up device.  We remove power without changing
	   cm3663->power_state because we use that state in resume.
	*/
  if(!n_drv_is) {	
  	struct cm3663_data *cm3663 = i2c_get_clientdata(this_client);


  	printk("\n cm3663_suspend+ \n");
	
  	if (!(cm3663->power_state & PROXIMITY_ENABLED)) {
  		printk("\n cm3663_suspend proximity_power false\n ");
  		cm3663->pdata->proximity_power(false);
  	}	
  	printk("\n cm3663_suspend- \n");
	}
  return 0;
}

static int cm3663_resume(struct device *dev)
{
	/* Turn power back on if we were before suspend. */
	if(!n_drv_is) {	
  	struct cm3663_data *cm3663 = i2c_get_clientdata(this_client);

  	printk("\n cm3663_resume+ \n");
	
  	if (!(cm3663->power_state & PROXIMITY_ENABLED)) {
  		printk("\n cm3663_resume proximity_power ture \n");
  		cm3663->pdata->proximity_power(true);
  	} else {
  		wake_lock_timeout(&cm3663->prx_wake_lock, 3*HZ);
  	}
	
	 printk("\n cm3663_resume- \n");
	}
	return 0;
}


static int cm3663_i2c_remove(struct i2c_client *client)
{
	struct cm3663_data *cm3663 = i2c_get_clientdata(client);

	device_destroy(cm3663->lightsensor_class, 0);
	class_destroy(cm3663->lightsensor_class);
	misc_deregister(&light_device);
	sysfs_remove_group(&cm3663->light_input_dev->dev.kobj,
			   &light_attribute_group);
	input_unregister_device(cm3663->light_input_dev);
	sysfs_remove_group(&cm3663->proximity_input_dev->dev.kobj,
			   &proximity_attribute_group);
	input_unregister_device(cm3663->proximity_input_dev);
	free_irq(cm3663->pdata->irq, NULL);
	if (cm3663->power_state) {
		cm3663->power_state = 0;
		if (cm3663->power_state & LIGHT_ENABLED)
			cm3663_light_disable(cm3663);
		if (cm3663->power_state & PROXIMITY_ENABLED)
			cm3663_i2c_write(cm3663, REGS_PS_CMD, 0x01);
	}
	destroy_workqueue(cm3663->prox_wq);
	destroy_workqueue(cm3663->light_wq);
	mutex_destroy(&cm3663->power_lock);
	wake_lock_destroy(&cm3663->prx_wake_lock);
	kfree(cm3663);
	return 0;
}

static const struct i2c_device_id cm3663_device_id[] = {
	{"cm3663", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, cm3663_device_id);

static struct i2c_driver cm3663_i2c_driver = {
	.driver = {
		.name = "cm3663",
		.owner = THIS_MODULE,

	},
	.probe		= cm3663_i2c_probe,
	.remove		= cm3663_i2c_remove,
	.suspend = cm3663_suspend,
	.resume = cm3663_resume,
	.id_table	= cm3663_device_id,
};


static int __init cm3663_init(void)
{
	return i2c_add_driver(&cm3663_i2c_driver);
}

static void __exit cm3663_exit(void)
{
	i2c_del_driver(&cm3663_i2c_driver);
}

module_init(cm3663_init);
module_exit(cm3663_exit);

MODULE_AUTHOR("tim.sk.lee@samsung.com");
MODULE_DESCRIPTION("Optical Sensor driver for cm3663");
MODULE_LICENSE("GPL");

