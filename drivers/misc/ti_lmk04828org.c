/*
 * https://blog.csdn.net/u010243305/article/details/78426058
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/acpi.h>
#include <linux/miscdevice.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <asm/current.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/unistd.h>

#define MASK_WRITE  0x80
#define MASK_READ   0x80
#define MASK_SEVE   0x60
#define MASK_ADDR_H     0x1F
#define SPI_SPEED_HZ    200000
#define LMK04828_MAGIC  'K'
#define GET_REG _IOR(LMK04828_MAGIC, 0, int)
#define SET_REG _IOW(LMK04828_MAGIC, 0, int)

struct lmk04828_t {
    dev_t devt;
    struct miscdevice misc_dev;
    spinlock_t spi_lock;
    struct spi_device *spi;
    struct list_head device_entry;

    /* TX/RX buffers are NULL unless opened (users > 0) */
    struct mutex buf_lock;
    unsigned int users;
    u8 *tx_buffer;
    u8 *rx_buffer;
    u32 speed_hz;
    u32 cur_index;  /* record the register offset */
    void __iomem *pl_cs_addr;
    u32 pl_cs_val;
};
static struct lmk04828_t *drvdata;

void lmk04828spi_cs(struct lmk04828_t *drvdata)
{
#if 0
    iowrite32(drvdata->pl_cs_val, drvdata->pl_cs_addr);
#endif
}

static ssize_t lmk04828spi_sync(struct lmk04828_t *drvdata, struct spi_message *m)
{
    DECLARE_COMPLETION_ONSTACK(done);
    int ret;
	struct spi_device *spi = drvdata->spi;

    if (spi == NULL) return -ESHUTDOWN;

    lmk04828spi_cs(drvdata);

	ret = spi_sync(spi, m);
    if (ret != 0)
		return ret;
	return m->actual_length;
}

static ssize_t lmk04828spi_sync_write(struct lmk04828_t *drvdata, size_t len)
{
    struct spi_transfer t = {
		.tx_buf     = drvdata->tx_buffer,
		.len        = len,
		.speed_hz   = drvdata->speed_hz,
	};
    struct spi_message m;

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    return lmk04828spi_sync(drvdata, &m);
}

static ssize_t lmk04828spi_sync_read(struct lmk04828_t *drvdata, size_t len)
{
    struct spi_transfer t = {
		.rx_buf     = drvdata->rx_buffer,
		.len        = len,
		.speed_hz   = drvdata->speed_hz,
	};
    struct spi_message m;

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    return lmk04828spi_sync(drvdata, &m);
}

int lmk04828_write_reg(int reg, u8 value)
{
    u8 cmd[3]={0};
    u8 addr_h = reg >> 8;
    u8 addr_l = reg & 0xff;

    cmd[0] = addr_h & MASK_ADDR_H;
    cmd[0] &= ~ MASK_SEVE;
    cmd[0] &= ~ MASK_WRITE;
    cmd[1] = addr_l;
    cmd[2] = value;
    drvdata->tx_buffer = cmd;
    drvdata->speed_hz = SPI_SPEED_HZ;

    return lmk04828spi_sync_write(drvdata, 3);
}

int lmk04828_read_reg(int reg, u8 buff[1])
{
    u8 cmd[3]={0};
    u8 addr_h = reg >> 8;
    u8 addr_l = reg & 0xff;

    cmd[0] = addr_h & MASK_ADDR_H;
    cmd[0] &= ~ MASK_SEVE;
    cmd[0] |=  MASK_READ;
    cmd[1] = addr_l;
    cmd[2] = 0;
    drvdata->tx_buffer = cmd;
    drvdata->rx_buffer = buff;
    drvdata->speed_hz = SPI_SPEED_HZ;

    return spi_write_then_read(drvdata->spi, cmd,2, buff, 1);
}

int lmk04828_spi_read(unsigned reg)
{
	u8 buf[3];
	int ret;

	buf[0] = 0x80 | (reg >> 8);
	buf[1] = reg & 0xFF;
	ret = spi_write_then_read(drvdata->spi, &buf[0], 2, &buf[2], 1);
	dev_dbg(&drvdata->spi->dev, "%s: REG: 0x%X VAL: 0x%X (%d)\n",
		__func__, reg, buf[2], ret);
	if (ret < 0) {
		dev_err(&drvdata->spi->dev, "%s: failed (%d)\n", __func__, ret);
		return ret;
	}

	return buf[2];
}

int lmk04828_reg_pll2_n(char enable, unsigned int val)
{
    int ret1,ret2,ret3;

    if (!enable) {
        ret1 = lmk04828_write_reg(0x168, val & 0xff);
        ret2 = lmk04828_write_reg(0x167, (val >> 8) & 0xff);
        ret3 = lmk04828_write_reg(0x166, (val >> 16) & 0x03 );
    } else {
        ret1 = lmk04828_write_reg(0x168, val & 0xff);
        ret2 = lmk04828_write_reg(0x167, (val >> 8) & 0xff);
        ret3 = lmk04828_write_reg(0x166, ((val >> 16) & 0x03) | 0x04 );
    }
    if (ret1 >=0 && ret2 >=0 && ret3 >=0) {
        return 0;
    } else {
        return -1;
    }
}

int lmk04828_reg_init(void)
{
    if (0 > lmk04828_write_reg(0, 0x80)) {
        return -1;
    }
    msleep(100);

    /* PIN MUX SET */
    if (0 > lmk04828_write_reg(0x14A, 0X33)) {
        return -1;
    }

    if (0 > lmk04828_write_reg(0, 0x10)) {
        return -1;
    }
    msleep(100);

    if (!((0xD0 == lmk04828_spi_read(0x004)) && 
          (0x5B == lmk04828_spi_read(0x005)) && 
          (0x20 == lmk04828_spi_read(0x006)))) {
        dev_err(&drvdata->spi->dev, "Initialization falied!\n");
        return -1;
    }
    if (0 > lmk04828_write_reg(0x116, 0x79)) {
        return -1;
    }
    if (0 > lmk04828_write_reg(0x11e, 0x79)) {
        return -1;
    }
    if (0 > lmk04828_write_reg(0x126, 0x79)) {
        return -1;
    }
    if (0 > lmk04828_write_reg(0x100, 0)) {
        return -1;
    }
    if (0 > lmk04828_write_reg(0x104, 0x20)) {
        return -1;
    }
    if (0 > lmk04828_write_reg(0x106, 0x70)) {
        return -1;
    }
    if (0 > lmk04828_write_reg(0x107, 0x33)) {
        return -1;
    }
    if (0 > lmk04828_write_reg(0x128, 0)) {
        return -1;
    }
    if (0 > lmk04828_write_reg(0x143, 0x10)) {
        return -1;
    }
    if (0 > lmk04828_write_reg(0x144, 0xff)) {
        return -1;
    }
    if (0 > lmk04828_write_reg(0x147, 0x38)) {
        return -1;
    }
    if (0 > lmk04828_write_reg(0x162, 0x45)) {
        return -1;
    }
    if (0 > lmk04828_write_reg(0x168, 0x0a)) {
        return -1;
    }

    return 0;
}

/*------------------------------------------------------------------------------
 * File ops
 */

static int lmk04828_open(struct inode *inode, struct file *fp)
{
    return 0;
}

static int lmk04828_release(struct inode *inode, struct file *fp)
{
    return 0;
}

static loff_t lmk04828_llseek(struct file *fp, loff_t off, int len)
{
    drvdata->cur_index = off;
    return 0;
}

static ssize_t lmk04828_read(struct file *fp, char __user *buf,
		size_t size, loff_t *off)
{
    u8 kbuf[1]={0};

    mutex_lock(&drvdata->buf_lock);
    lmk04828_read_reg(drvdata->cur_index, kbuf);
    mutex_unlock(&drvdata->buf_lock);

    return copy_to_user(buf, kbuf, 1);
}

static ssize_t lmk04828_write(struct file *fp, const char __user *buf,
        size_t size, loff_t  *off)
{
    u8 kbuf[1] = { 0 };
    int ret = 0;

	if (0 > copy_from_user(kbuf, buf, 1)) {
        dev_err(&drvdata->spi->dev, "copy_from_user error %s %d\n", __func__, __LINE__);
		return -1;
    }

    mutex_lock(&drvdata->buf_lock);
    ret = lmk04828_write_reg(drvdata->cur_index, kbuf[0]);
    mutex_unlock(&drvdata->buf_lock);
    return ret;
}

static long lmk04828_ioctl(struct file *fp, unsigned int cmd,
		unsigned long arg)
{
    switch(cmd) {
    case GET_REG:
		break;
    case SET_REG:
		break;
    default:
		dev_err(&drvdata->spi->dev, "invalid argument\n");
		return -EINVAL;
    }

    return 0;
}

static struct file_operations fops = {
    .owner			= THIS_MODULE,
    .open			= lmk04828_open,
    .release		= lmk04828_release,
    .read			= lmk04828_read,
    .write			= lmk04828_write,
    .llseek			= lmk04828_llseek,
    .unlocked_ioctl	= lmk04828_ioctl,
};

/*------------------------------------------------------------------------------
 * init and deinit
 */

static int lmk04828_probe(struct spi_device *spi)
{
    //struct lmk04828_t *drvdata;
    struct device_inode *np;
	int ret;

    dev_info(&spi->dev, "%s: enter\n", __func__);

    /* Allocate driver data */
    drvdata = kzalloc(sizeof(*drvdata), GFP_KERNEL);
    if (!drvdata)
        return -ENOMEM;

    /* Initialize the driver data */
    drvdata->spi = spi;
    drvdata->speed_hz = SPI_SPEED_HZ;
    spin_lock_init(&drvdata->spi_lock);
    mutex_init(&drvdata->buf_lock);
    INIT_LIST_HEAD(&drvdata->device_entry);
    drvdata->misc_dev.fops = &fops;
    drvdata->misc_dev.name = "lmk04828";
    drvdata->misc_dev.minor = MISC_DYNAMIC_MINOR;

    misc_register(&drvdata->misc_dev);

#ifdef CONFIG_OF
    np = of_find_inode_by_name(NULL, "lmk04828-spi");
    if (NULL == np) {
        dev_err(&spi->dev, "device-tree: inode 'lmk04828-spi' not find\n");
        return -EINVAL;
    }
#if 0
	u32 tmp;
	ret = of_property_read_u32_index(np, "pl-cs-addr", 0, &tmp);
    if (ret < 0) {
		dev_err(&spi->dev, "device-tree: property 'pl-cs-addr' not find\n");
		return -EINVAL;
	}
	drvdata->pl_cs_addr = ioremap(tmp, 4);
	ret = of_property_read_u32_index(np, "pl-cs-val", 0, &drvdata->pl_cs_val);
	if (ret < 0) {
        dev_err(&spi->dev, "device-tree: property 'pl-cs-val' not find\n");
		return -EINVAL;
    }
    dev_info(&spi->dev, "device-tree: pl-cs-addr=%x(ioremap %p), pl-cs-val=%x\n",
		tmp, drvdata->pl_cs_addr, drvdata->pl_cs_val);
	iowrite32(drvdata->pl_cs_val, drvdata->pl_cs_addr);
#endif
#endif

	spi_set_drvdata(spi, drvdata);

    ret = lmk04828_reg_init();
	if (ret < 0) {
		dev_err(&spi->dev, "%s: init failed due to spi access!\n", __func__);
		return -ENODEV;
	}

    dev_info(&spi->dev, "%s: succeed\n", __func__);
    return 0;
}

static int lmk04828_remove(struct spi_device *spi)
{
    struct lmk04828_t *drvdata = spi_get_drvdata(spi);
    misc_deregister(&drvdata->misc_dev);
	kfree(drvdata);
    return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id lmk04828_of_match[] = {
    { .compatible = "ti,lmk04828" },
    { }
};
#endif

static const struct spi_device_id lmk04828_id_table[] = {
    { "lmk04828", 0 },
    { }
};
MODULE_DEVICE_TABLE(spi, lmk04828_id_table);

static struct spi_driver lmk04828_driver = {
    .driver = {
        .name   = "lmk04828",
        .owner = THIS_MODULE,
#ifdef CONFIG_OF
        .of_match_table = of_match_ptr(lmk04828_of_match),
#endif
    },
    .probe      = lmk04828_probe,
    .remove     = lmk04828_remove,
    .id_table   = lmk04828_id_table,
};

module_spi_driver(lmk04828_driver);
MODULE_ALIAS("lmk04828-spi");
MODULE_AUTHOR("Thomas Chou <thomas@wytron.com.tw>");
MODULE_LICENSE("GPL");
