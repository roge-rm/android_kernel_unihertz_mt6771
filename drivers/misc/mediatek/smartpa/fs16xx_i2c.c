// SPDX-License-Identifier: GPL-2.0
/*
 * Raw I2C access to the FS16xx smart amp for the vendor audio libraries
 * (libfs16xx_interface.so, libfs1860.so), as stock's foursemi driver:
 * misc device smartpa_i2c on the mediatek,speaker_amp client (I2C 3, 0x34).
 *
 * The chip isn't touched at probe. Userspace does init, presets, power on
 * and off. I2C_SLAVE_FORCE sets the shared client's address for 0x34 to 0x37
 * and every other ioctl returns 0 doing nothing. Read is clamped to 8192
 * bytes, write isn't (stock). No lock: the adapter lock in i2c_transfer
 * serialises transfers, and the address is global across open files.
 * Mode is left to init (init.mt6771.rc chmods it 0666 on boot).
 */

#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define FS16XX_READ_MAX 8192

static struct i2c_client *fs16xx_client;

static int fs16xx_open(struct inode *inode, struct file *file)
{
	return nonseekable_open(inode, file);
}

static long fs16xx_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct i2c_client *client = READ_ONCE(fs16xx_client);

	if (cmd == I2C_SLAVE_FORCE && (arg & ~3UL) == 0x34 && client)
		client->addr = arg;
	return 0;
}

static ssize_t fs16xx_read(struct file *file, char __user *buf, size_t count,
			   loff_t *offset)
{
	struct i2c_client *client = READ_ONCE(fs16xx_client);
	char *tmp;
	int ret;

	if (!client)
		return -ENODEV;
	if (count > FS16XX_READ_MAX)
		count = FS16XX_READ_MAX;
	tmp = kmalloc(count, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	ret = i2c_master_recv(client, tmp, count);
	if (ret >= 0 && copy_to_user(buf, tmp, ret))
		ret = -EFAULT;
	kfree(tmp);
	return ret;
}

static ssize_t fs16xx_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *offset)
{
	struct i2c_client *client = READ_ONCE(fs16xx_client);
	char *tmp;
	int ret;

	if (!client)
		return -ENODEV;
	tmp = memdup_user(buf, count);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);
	ret = i2c_master_send(client, tmp, count);
	kfree(tmp);
	return ret;
}

static const struct file_operations fs16xx_fops = {
	.owner = THIS_MODULE,
	.open = fs16xx_open,
	.read = fs16xx_read,
	.write = fs16xx_write,
	.unlocked_ioctl = fs16xx_ioctl,
	/* 4.14 passed I2C_SLAVE_FORCE through its compat table; 5.10 has none */
	.compat_ioctl = fs16xx_ioctl,
};

static struct miscdevice fs16xx_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "smartpa_i2c",
	.fops = &fs16xx_fops,
};

static int fs16xx_i2c_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	int ret;

	pr_info("%s client->addr:0x%04x\n", __func__, client->addr);
	/* One amp only: a second binding gets -EBUSY and leaves the first alone. */
	ret = misc_register(&fs16xx_misc);
	if (ret)
		return ret;
	WRITE_ONCE(fs16xx_client, client);
	return 0;
}

static int fs16xx_i2c_remove(struct i2c_client *client)
{
	misc_deregister(&fs16xx_misc);
	WRITE_ONCE(fs16xx_client, NULL);
	return 0;
}

static const struct i2c_device_id fs16xx_i2c_id[] = {
	{ "foursemi", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fs16xx_i2c_id);

static const struct of_device_id fs16xx_of_match[] = {
	{ .compatible = "mediatek,speaker_amp" },
	{ }
};
MODULE_DEVICE_TABLE(of, fs16xx_of_match);

static struct i2c_driver fs16xx_i2c_driver = {
	.driver = {
		.name = "foursemi",
		.of_match_table = fs16xx_of_match,
	},
	.probe = fs16xx_i2c_probe,
	.remove = fs16xx_i2c_remove,
	.id_table = fs16xx_i2c_id,
};

static int __init fs16xx_mod_init(void)
{
	pr_info("fs16xx_init\n");
	return i2c_add_driver(&fs16xx_i2c_driver);
}
module_init(fs16xx_mod_init);

static void __exit fs16xx_mod_exit(void)
{
	i2c_del_driver(&fs16xx_i2c_driver);
}
module_exit(fs16xx_mod_exit);

MODULE_DESCRIPTION("FS16xx smart amp raw I2C misc device");
MODULE_LICENSE("GPL");
