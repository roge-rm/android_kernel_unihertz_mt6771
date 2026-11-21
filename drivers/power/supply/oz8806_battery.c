// SPDX-License-Identifier: GPL-2.0
/*
 * The "battery" power supply at /devices/platform/battery (the stock
 * "mediatek,bat_gm30" node), reporting the OZ8806 gauge until mtk_battery is
 * ported. Stock mtk_battery takes level, voltage, current and temperature
 * from the OZ8806 too; this keeps its supply name, parent and the
 * bmu_init_done and save_capacity attributes the OZ8806 driver adds there,
 * plus gauge_log for the OCV run logger.
 *
 * Every 10 s, as stock's battery_thread, it calls oz8806_update() and
 * reports the change.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power/oz8806.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#define OZ8806_BATTERY_INTERVAL_MS	10000

struct oz8806_battery {
	struct power_supply *psy;
	struct delayed_work work;
};

static enum power_supply_property oz8806_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_TEMP,
};

static int oz8806_battery_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct oz8806_report r;
	int curr;

	if (!oz8806_gauge_int_done() || oz8806_get_report(&r))
		return -ENODATA;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = r.status;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (r.soc < 0)
			return -ENODATA;
		val->intval = clamp(r.soc, 0, 100);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		/* live read, as stock battery_get_property; else the last report */
		curr = oz8806_get_battry_current();
		val->intval = (curr == -1 ? r.curr : curr) * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = r.volt * 1000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = r.fcc * 1000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = r.rc * 1000;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		if (!r.temp_ok)
			return -ENODATA;
		val->intval = r.temp * 10;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct power_supply_desc oz8806_battery_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = oz8806_battery_props,
	.num_properties = ARRAY_SIZE(oz8806_battery_props),
	.get_property = oz8806_battery_get_property,
};

static ssize_t bmu_init_done_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", oz8806_get_bmu_init_done());
}
static DEVICE_ATTR_RO(bmu_init_done);

static ssize_t save_capacity_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", oz8806_get_save_capacity());
}

/* sets the driver's saved capacity, not the chip */
static ssize_t save_capacity_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	int v;

	if (kstrtoint(buf, 10, &v))
		return -EINVAL;
	oz8806_set_save_capacity(v);
	return count;
}
static DEVICE_ATTR_RW(save_capacity);

/* the OCV run logger reads this */
static ssize_t gauge_log_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return oz8806_log_line(buf, PAGE_SIZE);
}
static DEVICE_ATTR_RO(gauge_log);

static struct attribute *oz8806_battery_attrs[] = {
	&dev_attr_bmu_init_done.attr,
	&dev_attr_gauge_log.attr,
	&dev_attr_save_capacity.attr,
	NULL,
};
ATTRIBUTE_GROUPS(oz8806_battery);

static void oz8806_battery_work(struct work_struct *work)
{
	struct oz8806_battery *bat = container_of(to_delayed_work(work),
						  struct oz8806_battery, work);

	oz8806_update();
	power_supply_changed(bat->psy);
	queue_delayed_work(system_freezable_wq, &bat->work,
			   msecs_to_jiffies(OZ8806_BATTERY_INTERVAL_MS));
}

static void oz8806_battery_changed(void *arg)
{
	struct oz8806_battery *bat = arg;

	power_supply_changed(bat->psy);
}

static int oz8806_battery_probe(struct platform_device *pdev)
{
	struct power_supply_config cfg = {};
	struct oz8806_battery *bat;

	bat = devm_kzalloc(&pdev->dev, sizeof(*bat), GFP_KERNEL);
	if (!bat)
		return -ENOMEM;
	cfg.of_node = pdev->dev.of_node;
	cfg.drv_data = bat;
	cfg.attr_grp = oz8806_battery_groups;
	bat->psy = devm_power_supply_register(&pdev->dev, &oz8806_battery_desc, &cfg);
	if (IS_ERR(bat->psy))
		return PTR_ERR(bat->psy);
	platform_set_drvdata(pdev, bat);
	INIT_DELAYED_WORK(&bat->work, oz8806_battery_work);
	queue_delayed_work(system_freezable_wq, &bat->work, 0);
	oz8806_set_changed_cb(oz8806_battery_changed, bat);
	return 0;
}

static int oz8806_battery_remove(struct platform_device *pdev)
{
	struct oz8806_battery *bat = platform_get_drvdata(pdev);

	oz8806_set_changed_cb(NULL, NULL);
	cancel_delayed_work_sync(&bat->work);
	return 0;
}

static const struct of_device_id oz8806_battery_of_match[] = {
	{ .compatible = "mediatek,bat_gm30" },
	{ }
};
MODULE_DEVICE_TABLE(of, oz8806_battery_of_match);

static struct platform_driver oz8806_battery_driver = {
	.probe = oz8806_battery_probe,
	.remove = oz8806_battery_remove,
	.driver = {
		.name = "oz8806-battery",
		.of_match_table = oz8806_battery_of_match,
	},
};
module_platform_driver(oz8806_battery_driver);

MODULE_DESCRIPTION("Titan Pocket battery supply from the OZ8806 gauge");
MODULE_LICENSE("GPL");
