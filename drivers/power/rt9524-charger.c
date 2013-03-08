/* 
 *  Richtek RT9524 Charger Driver
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */
#define DEBUG
#include <linux/platform_device.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/rt9524-charger.h>
#include <linux/power_supply.h>
#include <linux/device.h>
#ifdef CONFIG_LGE_WIRELESS_CHARGER_RT9524
#include <mach/gpio.h>
#include <linux/msm-charger.h>
#include <linux/max17040_battery.h>
#endif
#ifdef CONFIG_HAS_WAKELOCK
#include <linux/wakelock.h>
#endif

#define RT9524_DRIVER_NAME "rt9524"

#define RT9524_CABLE_PWRON_INT
#define RT9524_WORKAROUND_CHG_DONE
#define RT9524_CHG_WORK_PERIOD	((HZ) * 3)
#define RT9524_CHG_DONE_WORK_PERIOD	((HZ) * 20)

#define WIRELESS_CHG_I_CTL	103
#define WIRELESS_CHG_EN	104
#define WIRELESS_VIN_INT_N 123
#define WIRELESS_CHARGE_IRQ_N 124
#define WIRELESS_FULL_CHG 125

#define AAA	105
#define BBB 106
#define CCC 141

struct rt9524_chip {
	struct delayed_work charge_work;
	struct delayed_work charge_done_work;
	struct work_struct charge_irq_work; 
	struct power_supply charger;
	int irq;
	int chg_online;
	int present;
#ifdef CONFIG_LGE_WIRELESS_CHARGER_RT9524
	struct msm_hardware_charger	adapter_hw_chg;
#endif
#ifdef CONFIG_HAS_WAKELOCK
	struct wake_lock wl;
#endif
};

static struct workqueue_struct *local_charge_wq;
static struct workqueue_struct *local_charge_done_wq;
static struct workqueue_struct *local_irq_wq;

static struct rt9524_chip* chip; 

static irqreturn_t rt9524_valid_handler(int irq, void *data);
static irqreturn_t rt9524_valid_handler_2(int irq, void *data);
static int wireless_chg_status_change(struct rt9524_chip *chip, int pgb, int chgsb);
static int wireless_chg_status_change_testmode(struct rt9524_chip *chip, int pgb);

DEFINE_MUTEX(rt9524_mut);
DEFINE_MUTEX(rt9524_mut_testmode);

static void rt9524_set_mode(int pulse_cnt)	
{
	int ret = 0;
	int i = 0;

	for (i=0; i<pulse_cnt; i++)
	{
		ret = gpio_direction_output(WIRELESS_CHG_EN, 1);
		if (ret) {
			pr_err("%s: gpio_direction_output failed for %d\n",
					__func__, WIRELESS_CHG_EN);
		}

		usleep(200);

		ret = gpio_direction_output(WIRELESS_CHG_EN, 0);
		if (ret) {
			pr_err("%s: gpio_direction_output failed for %d\n",
					__func__, WIRELESS_CHG_EN);
		}

		usleep(200);

	}
#ifdef DEBUG
	printk("%s\n", __func__);
#endif
}


static void rt9524_charge(struct work_struct *rt9524_work)
{
#ifdef RT9524_WORKAROUND_CHG_DONE /* work around for charge done*/
	int pgb_val = 0;
	int chgsb_val = 0;
#endif
	struct rt9524_chip *rt9524_chg;

	rt9524_chg = container_of(rt9524_work, struct rt9524_chip,
			charge_work.work);
	/* Watchdog timer reset */
//	bq24160_set_bits(bq24160_chg->client, BQ24160_REG_STAT_CTRL, BQ24160_WDOG_TMR_MASK, (1<<BQ24160_WDOG_TMR_SHFT));
#ifdef DEBUG
	printk("%s: rt9524_chg->chg_online = %d\n", __func__, rt9524_chg->chg_online);
#endif

	if (rt9524_chg->chg_online) 
	{
		pgb_val = gpio_get_value_cansleep(WIRELESS_VIN_INT_N);
		chgsb_val = gpio_get_value_cansleep(WIRELESS_CHARGE_IRQ_N);
		if ( chgsb_val == 0 && pgb_val == 1 )
		{
#ifdef DEBUG
			printk("%s:charging done by chgsb is off, pgb is on\n", __func__);
#endif

			msm_charger_notify_event(&rt9524_chg->adapter_hw_chg, CHG_DONE_EVENT);
#ifdef CONFIG_HAS_WAKELOCK
			wake_lock(&rt9524_chg->wl);
#endif
			queue_delayed_work(local_charge_done_wq, &rt9524_chg->charge_done_work, RT9524_CHG_DONE_WORK_PERIOD);
		}
	}
	else 
	{
		// Charger remove
		cancel_delayed_work(&rt9524_chg->charge_work);
	}
#ifdef DEBUG
	printk("%s: pgb_val = %d, chgsb_val = %d\n", __func__, pgb_val, chgsb_val);
#endif
}

static void rt9524_charge_done(struct work_struct *rt9524_work)
{
	int rc;
	struct rt9524_chip *rt9524_chg;
	rt9524_chg = container_of(rt9524_work, struct rt9524_chip,
			charge_done_work.work);
#ifdef DEBUG
	printk("%s\n", __func__);
#endif

	rc = gpio_direction_output(WIRELESS_CHG_EN, 0);
	if (rc) {
		printk("%s: gpio_direction_output failed for WIRELESS_CHG_EN", __func__);
	}
#ifdef CONFIG_HAS_WAKELOCK
	wake_unlock(&rt9524_chg->wl);
#endif
}

/* full_charge_test */
/*
static int atoi(char *name)
{
    int val = 0; 
    for (;; name++) {
        switch (*name) {
	        case '0' ... '9': 
	            val = 10*val+(*name-'0');
	            break;
	        default:
	            return val; 
    	}    
	}    
}
static bool testmode_enable(void)
{
	int enable = atoi(testmode_fs);
	printk("%s: enable = %d\n", __func__, enable);
	if(enable) return true;
	else return false;
}
*/


static int testmode_enable_status = 0;

static ssize_t show_testmode_enable(struct device *dev,
                                    struct device_attribute *attr, char *buf)
{
	int ret;
	ret = sprintf(buf, "%d\n", testmode_enable_status);
	return ret;
}

static ssize_t store_testmode_enable(struct device *dev, struct device_attribute *attr, 
									const char *buf, size_t count)
{
	int ret = 0;
	int pgb_val = 0;
	unsigned char string[2];
	if(!count) return -EINVAL;
	sscanf(buf, "%s", string);
	if(!strncmp(string, "1", 1))
	{
		testmode_enable_status = 1;
		pgb_val = gpio_get_value_cansleep(WIRELESS_VIN_INT_N); 
		wireless_chg_status_change_testmode(chip, pgb_val);
		ret = gpio_direction_output(WIRELESS_FULL_CHG, 1);

#ifdef DEBUG
		printk("%s: testmode enable = %d\n", __func__, testmode_enable_status);
#endif

	}

	else if(!strncmp(string, "0", 1))
	{
		ret = gpio_direction_output(WIRELESS_FULL_CHG, 0);
		testmode_enable_status = 0;
#ifdef DEBUG
		printk("%s: testmode enable = %d\n", __func__, testmode_enable_status);
#endif
	}
	else return ret;
	return count;
}

static DEVICE_ATTR(testmode_enable, 0664, show_testmode_enable, store_testmode_enable);

static int wireless_chg_status_change(struct rt9524_chip *chip, int pgb, int chgsb)
{
	int ret = 0;
	int batt_soc = 0;

	mutex_lock(&rt9524_mut);

	batt_soc = max17040_get_battery_capacity_percent(); //max17040_get_capacity_percent();
	if (pgb == 0) chip->present = 1;
	else chip->present = 0;
#ifdef DEBUG
	printk("%s: batt_soc = %d\n", __func__, batt_soc);
#endif
	if (chip->present == 0) // remove state
	{
		msm_charger_notify_event(&chip->adapter_hw_chg, CHG_REMOVED_EVENT);
#ifdef DEBUG
		printk("%s: CHG_REMOVED_EVENT, chip->present=%d, pgb=%d, chgsb=%d\n", __func__, chip->present, pgb, chgsb/*, chg_done_cnt*/);
#endif
		ret = gpio_direction_output(WIRELESS_FULL_CHG, 0);
		if (ret) {
			printk("%s: gpio_direction_output failed for WIRELESS_FULL_CHG\n", __func__);
		}
	}
	else if (chip->present == 1)
	{
		if (pgb == 0 && chgsb == 0 && batt_soc <= 95) // charge & recharge state
		{
			msm_charger_notify_event(&chip->adapter_hw_chg, CHG_INSERTED_EVENT);
			rt9524_set_mode(1);
			ret = gpio_direction_output(WIRELESS_FULL_CHG, 0);
			if (ret) {
				printk("%s: gpio_direction_output failed for WIRELESS_FULL_CHG\n", __func__);
			}
#ifdef DEBUG
			printk("%s: CHG_INSERTED_EVENT, chip->present=%d, pgb=%d, chgsb=%d\n", __func__, chip->present, pgb, chgsb);
#endif
		}
		else if ((pgb == 0 && chgsb == 1) || (pgb == 0 && chgsb == 0 && batt_soc > 95)) //charge done state
		{
			msm_charger_notify_event(&chip->adapter_hw_chg, CHG_DONE_EVENT);
#ifdef DEBUG
			printk("%s: CHG_DONE_EVENT, chip->present=%d, pgb=%d, chgsb=%d\n", __func__, chip->present, pgb, chgsb);
#endif
			msleep(2000);
			ret = gpio_direction_output(WIRELESS_FULL_CHG, 1);
			msleep(5000);
			ret = gpio_direction_output(WIRELESS_FULL_CHG, 0);
			if (ret) {
				printk("%s: gpio_direction_output failed for WIRELESS_FULL_CHG\n", __func__);
			}
		}
		else if (pgb == 1 && chgsb == 1) //can't happen, exist for safty.
		{
			msleep(50);
#ifdef DEBUG
			printk("%s: Wireless charge off!!!!\n", __func__);
#endif
			ret = gpio_direction_output(WIRELESS_FULL_CHG, 0);
			if (ret) {
				printk("%s: gpio_direction_output failed for WIRELESS_FULL_CHG\n", __func__);
			}
		}
		else //can't happen, exist for safty.
		{
#ifdef DEBUG
			printk("%s: lie!! OVP or FULL_CHG low even though full charged.\n", __func__);
#endif
			ret = gpio_direction_output(WIRELESS_FULL_CHG, 0);
			if (ret) {
				printk("%s: gpio_direction_output failed for WIRELESS_FULL_CHG\n", __func__);
			}
		}
	}

	mutex_unlock(&rt9524_mut);
	
	return ret;
}

static int wireless_chg_status_change_testmode(struct rt9524_chip *chip, int pgb)
{
	int ret = 0;

	mutex_lock(&rt9524_mut_testmode);

	if (pgb == 0) chip->present = 1;
	else chip->present = 0;
	
	if (pgb == 0)
	{
		printk("testmode success CHG_INSERTED_EVENT\n");
		ret = msm_charger_notify_event(&chip->adapter_hw_chg, CHG_INSERTED_EVENT);
		if (ret) printk("%s: testmode fail CHG_INSERTED_EVENT\n", __func__);
		rt9524_set_mode(1);
	}
	else if (pgb ==1)
	{
		printk("testmode success CHG_REMOVED_EVENT\n");
		ret = msm_charger_notify_event(&chip->adapter_hw_chg, CHG_REMOVED_EVENT);
		if (ret) printk("%s: testmode fail CHG_REMOVED_EVENT\n", __func__);
	}
	else printk("%s: testmode pgb fail. It can't be shown. exist for safty\n",__func__);
	ret = gpio_direction_output(WIRELESS_FULL_CHG, 1);
	if (ret) printk("%s: testmode fail gpio_direction_output\n", __func__);

	mutex_unlock(&rt9524_mut_testmode);

	return ret;
}

static void rt9524_irq_charge(struct work_struct *rt9524_work)
{
	int pgb_val = 0;
	int chgsb_val = 0;
	int ret = 0;

#ifdef CONFIG_HAS_WAKELOCK
	wake_lock(&chip->wl);
#endif
	msleep(1000);
	chgsb_val = gpio_get_value_cansleep(WIRELESS_CHARGE_IRQ_N);
	pgb_val = gpio_get_value_cansleep(WIRELESS_VIN_INT_N);
#ifdef DEBUG
	printk("WIRELESS irq changed!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
#endif

	if (testmode_enable_status) ret = wireless_chg_status_change_testmode(chip, pgb_val);
	else ret = wireless_chg_status_change(chip, pgb_val, chgsb_val);
#ifdef CONFIG_HAS_WAKELOCK
	wake_unlock(&chip->wl);
#endif
}

static irqreturn_t rt9524_valid_handler(int irq, void *data)
{
	struct rt9524_chip *chip = (struct rt9524_chip *)data;

	queue_work(local_irq_wq,&chip->charge_irq_work);

	return IRQ_HANDLED;
}

static irqreturn_t rt9524_valid_handler_2(int irq, void *data)
{
	struct rt9524_chip *chip = (struct rt9524_chip *)data;

	queue_work(local_irq_wq,&chip->charge_irq_work);
	
	return IRQ_HANDLED;
}

static int rt9524_charger_get_property(struct power_supply *psy,
                                        enum power_supply_property psp,
                                        union power_supply_propval *val)
{
	struct rt9524_chip *chip = container_of(psy, struct rt9524_chip, charger);
	int ret = 0;
	int pgb_val = 0;
	int chgsb_val = 0;
	pgb_val = gpio_get_value_cansleep(WIRELESS_VIN_INT_N);
	chgsb_val = gpio_get_value_cansleep(WIRELESS_CHARGE_IRQ_N);

	if (psp == POWER_SUPPLY_PROP_ONLINE) {
		if (testmode_enable_status && pgb_val == 0)
		{
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
#ifdef DEBUG
		printk("%s: POWER_SUPPLY_STATUS_CHARGING for testmode\n",__func__);
#endif

		}
		else
		{
			val->intval = chip->chg_online;
#ifdef DEBUG
		printk("%s: POWER_SUPPLY_PROP_ONLINE\n",__func__);
#endif

		}
	}
	else if (psp == POWER_SUPPLY_PROP_STATUS) {
		if (chip->chg_online) {
			if (chgsb_val == 1 && pgb_val == 0) {
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
#ifdef DEBUG
			printk("%s: val->intval = POWER_SUPPLY_STATUS_DISCHARGING\n",__func__);
#endif
			}
			else if ( chgsb_val == 0 && pgb_val == 0 ) {
				val->intval = POWER_SUPPLY_STATUS_CHARGING;
#ifdef DEBUG
				printk("%s: val->intval = POWER_SUPPLY_STATUS_CHARGING\n", __func__);
#endif
			}
			else {
				val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
#ifdef DEBUG
				printk("%s: val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING\n",__func__);
#endif
			}
		}
		else {
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
#ifdef DEBUG
			printk("%s: val->intval = POWER_SUPPLY_STATUS_DISCHARGING\n",__func__);
#endif
		}
		ret = 0;
	} else {
		ret = -ENODEV;
	}
#ifdef DEBUG
	printk("%s: pgb_val = %d, chgsb_val = %d\n", __func__, pgb_val, chgsb_val);
#endif
	return ret;
}

static enum power_supply_property rt9524_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
};

static struct power_supply rt9524_charger_ps = {
	.name = "wireless-charger",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = rt9524_charger_props,
	.num_properties = ARRAY_SIZE(rt9524_charger_props),
	.get_property = rt9524_charger_get_property,
};

#ifdef CONFIG_LGE_WIRELESS_CHARGER_RT9524
static int rt9524_start_charging(struct msm_hardware_charger *hw_chg,
		int chg_voltage, int chg_current)
{
	struct rt9524_chip *rt9524_chg;
	int ret = 0;

	rt9524_chg = container_of(hw_chg, struct rt9524_chip, adapter_hw_chg);
	if (rt9524_chg->chg_online)
	{
//		we are already charging 
#ifdef DEBUG
		printk("%s: we are already charging, return\n", __func__);
#endif
		return 0;
	}

	msleep(100);
	rt9524_chg->chg_online = 1;
	queue_delayed_work(local_charge_wq, &rt9524_chg->charge_work, RT9524_CHG_WORK_PERIOD);

	power_supply_changed(&rt9524_chg->charger);

#ifdef DEBUG
	printk("rt9524_start_charging\n");
#endif

	return ret;
}

static int rt9524_stop_charging(struct msm_hardware_charger *hw_chg)
{
	struct rt9524_chip *rt9524_chg;
	int ret = 0;

	rt9524_chg = container_of(hw_chg, struct rt9524_chip, adapter_hw_chg);
	if (!(rt9524_chg->chg_online))
		/* we arent charging */
		return 0;

	rt9524_chg->chg_online = 0;
	cancel_delayed_work(&rt9524_chg->charge_work);

	power_supply_changed(&rt9524_chg->charger);

#ifdef DEBUG
	printk("rt9524_stop_charging\n");
#endif

	return ret;
}


static int rt9524_charging_switched(struct msm_hardware_charger *hw_chg)
{
	struct rt9524_chip *rt9524_chg;

	rt9524_chg = container_of(hw_chg, struct rt9524_chip, adapter_hw_chg);
#ifdef DEBUG
	printk("rt9524_charging_switched\n");
#endif
	return 0;
}

#endif

static int rt9524_probe(struct platform_device *pdev)
{
    int ret = 0;
	int pgb_irq_num = 0;
	int chgsb_irq_num = 0;
	ret = device_create_file(&pdev->dev, &dev_attr_testmode_enable);
	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
#ifdef DEBUG
	printk("%s E\n", __func__);
#endif
	INIT_DELAYED_WORK(&chip->charge_work, rt9524_charge);
	INIT_DELAYED_WORK(&chip->charge_done_work, rt9524_charge_done);
	INIT_WORK(&chip->charge_irq_work, rt9524_irq_charge);
#ifdef DEBUG
	printk("%s: INIT_DELAYED_WORK start", __func__);
#endif

	chip->charger = rt9524_charger_ps;

	ret = power_supply_register(&pdev->dev, &chip->charger);
	if (ret) {
		dev_err(&pdev->dev, "failed: power supply register\n");
		goto out;
	}

#ifdef CONFIG_LGE_WIRELESS_CHARGER_RT9524
	/* fill hw_chg structure for registering msm_charger */
	chip->adapter_hw_chg.type = CHG_TYPE_AC;
	chip->adapter_hw_chg.rating = 1;
	chip->adapter_hw_chg.name = "rt9524-adapter";
	chip->adapter_hw_chg.start_charging = rt9524_start_charging;
	chip->adapter_hw_chg.stop_charging = rt9524_stop_charging;
	chip->adapter_hw_chg.charging_switched = rt9524_charging_switched;

	ret = gpio_request(WIRELESS_VIN_INT_N, "rt9524_irq_gpio_pgb");
	if (ret) {
		printk("%s: gpio_request failed for WIRELESS_VIN_INT_N\n", __func__);
		goto out;
	}

	pgb_irq_num = gpio_to_irq(WIRELESS_VIN_INT_N);
	ret = gpio_tlmm_config(GPIO_CFG(WIRELESS_VIN_INT_N, 0, GPIO_CFG_INPUT, GPIO_CFG_PULL_UP, GPIO_CFG_2MA), GPIO_CFG_ENABLE);

	ret = request_threaded_irq(pgb_irq_num, NULL, rt9524_valid_handler,
		IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING, "rt9524_irq_pgb", chip);

	if (unlikely(ret < 0))
	{
		printk("%s failed to request IRQ %d\n",__func__, ret);
		goto out;
	}
	//set_irq_wake(pgb_irq_num, 1);
	irq_set_irq_wake(pgb_irq_num, 1);

#ifdef DEBUG
	printk("%s: gpio_set_WIRELESS_VIN_INT_N\n", __func__);
#endif

	ret = gpio_request(WIRELESS_CHARGE_IRQ_N, "rt9524_irq_gpio_chgsb");
	if (ret) {
		printk("%s: gpio_request failed for WIRELESS_CHARGE_IRQ_N\n", __func__);
		goto out;
	}
	
	chgsb_irq_num = gpio_to_irq(WIRELESS_CHARGE_IRQ_N);
	ret = gpio_tlmm_config(GPIO_CFG(WIRELESS_CHARGE_IRQ_N, 0, GPIO_CFG_INPUT, GPIO_CFG_PULL_UP, GPIO_CFG_2MA), GPIO_CFG_ENABLE);

	ret = request_threaded_irq(chgsb_irq_num, NULL, rt9524_valid_handler_2,
		IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING, "rt9524_irq_chgsb", chip);

	if (unlikely(ret < 0))
	{
		printk("%s failed to request IRQ %d\n",__func__, ret);
		goto out;
	}
	//set_irq_wake(chgsb_irq_num, 1);
	irq_set_irq_wake(chgsb_irq_num, 1);

#ifdef DEBUG
	printk("%s: gpio_set_WIRELESS_CHARGE_IRQ_N\n", __func__);
#endif

	/* set gpios for current consumption optimization. add here for additional controls */
	ret = gpio_request(AAA, "AAA");
	if(ret) {
		dev_err(&pdev->dev,"%s: AAA request\n",
				__func__);
		goto out;
	}
	ret = gpio_tlmm_config(GPIO_CFG(AAA, 0, GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	if(ret) {
		dev_err(&pdev->dev,"%s: AAA tlmmd\n",
				__func__);
		goto out;
	}
	ret = gpio_request(BBB, "BBB");
	if(ret) {
		dev_err(&pdev->dev,"%s: BBB request \n",
				__func__);
		goto out;
	}
	ret = gpio_tlmm_config(GPIO_CFG(BBB, 0, GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	if(ret) {
		dev_err(&pdev->dev,"%s: BBB tlmm\n",
				__func__);
		goto out;
	}
	ret = gpio_request(CCC, "CCC");
	if(ret) {
		dev_err(&pdev->dev,"%s: CCC request \n",
				__func__);
		goto out;
	}
	ret = gpio_tlmm_config(GPIO_CFG(CCC, 0, GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	if(ret) {
		dev_err(&pdev->dev,"%s: CCC tlmm\n",
				__func__);
		goto out;
	}
	/* gpio setting end */

	ret = gpio_request(WIRELESS_CHG_EN, "WIRELESS_CHG_EN");
	if(ret) {
		dev_err(&pdev->dev,"%s: WIRELESS_CHG_EN %d request failed. ret = %d\n",
				__func__, WIRELESS_CHG_EN, ret);
		goto out;
	}
	ret = gpio_tlmm_config(GPIO_CFG(WIRELESS_CHG_EN, 0, GPIO_CFG_OUTPUT, GPIO_CFG_PULL_DOWN, GPIO_CFG_2MA), GPIO_CFG_ENABLE);

	ret = gpio_request(WIRELESS_CHG_I_CTL, "WIRELESS_CHG_I_CTL");
	if(ret) {
		dev_err(&pdev->dev,"%s: WIRELESS_CHG_I_CTL %d request failed. ret = %d\n",
				__func__, WIRELESS_CHG_I_CTL, ret);
		goto out;
	}
	ret = gpio_tlmm_config(GPIO_CFG(WIRELESS_CHG_I_CTL, 0, GPIO_CFG_OUTPUT, GPIO_CFG_PULL_UP, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	ret = gpio_direction_output(WIRELESS_CHG_I_CTL, 1);
	if (ret) {
		printk("%s: gpio_direction_output failed for WIRELESS_CHG_I_CTL", __func__);
		goto out;
	}

	ret = gpio_request(WIRELESS_FULL_CHG, "WIRELESS_FULL_CHG");
	if(ret) {
		dev_err(&pdev->dev,"%s: WIRELESS_FULL_CHG %d request failed. ret = %d\n",
				__func__, WIRELESS_FULL_CHG, ret);
		goto out;
	}
	ret = gpio_tlmm_config(GPIO_CFG(WIRELESS_FULL_CHG, 0, GPIO_CFG_OUTPUT,  GPIO_CFG_PULL_DOWN, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	ret = gpio_direction_output(WIRELESS_FULL_CHG, 0);
	if (ret) {
		printk("%s: gpio_direction_output failed for WIRELESS_FULL_CHG", __func__);
		goto out;
	}

	ret = msm_charger_register(&chip->adapter_hw_chg);
	if (ret) {
		dev_err(&pdev->dev,
			"%s msm_charger_register failed for ret =%d\n",
			__func__, ret);
		goto out;
	}
#endif


	ret = gpio_get_value_cansleep(WIRELESS_VIN_INT_N);

	msleep(100);

	if(!ret)
	{
		msm_charger_notify_event(&chip->adapter_hw_chg, CHG_INSERTED_EVENT);
		chip->present = 1;
		rt9524_set_mode(1);

#ifdef DEBUG
		printk("%s: msm_charger_notify_event <CHG_INSERTED_EVENT>", __func__);
#endif

	}

#ifdef CONFIG_HAS_WAKELOCK
	wake_lock_init(&chip->wl, WAKE_LOCK_SUSPEND, "rt9524");
#endif
#ifdef DEBUG
	printk("%s: probe finished", __func__);
#endif
	return 0;
out:
	kfree(chip);
	wake_lock_destroy(&chip->wl);
	return ret;
}

static int rt9524_remove(struct platform_device *pdev)
{
	power_supply_unregister(&rt9524_charger_ps);
	kfree(chip);
	return 0;
}

static struct platform_driver rt9524_driver = {
		.probe = rt9524_probe,
		.remove = rt9524_remove,
		.driver = {
				.name = RT9524_DRIVER_NAME,
				.owner = THIS_MODULE,
		},
};

static int __init rt9524_init(void)
{
	local_charge_wq = create_workqueue("rt9524_charge_work");
	if(!local_charge_wq)
		return -ENOMEM;
	
	local_charge_done_wq = create_workqueue("rt9524_charge_done_work");
	if(!local_charge_done_wq)
		return -ENOMEM;

	local_irq_wq = create_workqueue("rt9524_irq_work");
	if(!local_irq_wq)
		return -ENOMEM;
	
	return platform_driver_register(&rt9524_driver);
}
module_init(rt9524_init);

static void __exit rt9524_exit(void)
{
	if(local_charge_wq)
		destroy_workqueue(local_charge_wq);
	local_charge_wq = NULL;

	if(local_charge_done_wq)
		destroy_workqueue(local_charge_done_wq);
	local_charge_done_wq = NULL;

	if(local_irq_wq)
		destroy_workqueue(local_irq_wq);
	local_irq_wq = NULL;
	
	platform_driver_unregister(&rt9524_driver);
}
module_exit(rt9524_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jinhong Kim <miracle.kim@lge.com>");
MODULE_DESCRIPTION("Power supply driver for RT9524");
