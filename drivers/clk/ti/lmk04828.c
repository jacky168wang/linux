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
#define GET_REG     _IOR(LMK04828_MAGIC, 0,int)
#define SET_REG     _IOW(LMK04828_MAGIC, 0, int)

struct lmk04828_t {
    dev_t           devt;
    struct miscdevice   misc_dev;
    spinlock_t      spi_lock;
    struct spi_device   *spi;
    struct list_head    device_entry;

    /* TX/RX buffers are NULL unless this device is open (users > 0) */
    struct mutex        buf_lock;
    unsigned        users;
    u8          *tx_buffer;
    u8          *rx_buffer;
    u32         speed_hz;
    u32         cur_index;  //record the register offset
    void __iomem *      pl_cs_addr;
    u32             pl_cs_val;
};static struct lmk04828_t *lmk04828;


void lmk04828spi_cs(void)
{
    iowrite32( lmk04828->pl_cs_val, lmk04828->pl_cs_addr);

}
static ssize_t lmk04828spi_sync(struct lmk04828_t *spidev, struct spi_message *message)
{
    DECLARE_COMPLETION_ONSTACK(done);
    int status;
    struct spi_device *spi;

    spin_lock_irq(&spidev->spi_lock);
    spi = spidev->spi;
    spin_unlock_irq(&spidev->spi_lock);
    lmk04828spi_cs();

    if (spi == NULL)
        status = -ESHUTDOWN;
    else
        status = spi_sync(spi, message);

    if (status == 0)
        status = message->actual_length;
    return status;
}

static ssize_t lmk04828spi_sync_write(struct lmk04828_t *spidev, size_t len)
{
    struct spi_transfer t = {
            .tx_buf     = spidev->tx_buffer,
            .len        = len,
            .speed_hz   = spidev->speed_hz,
        };
    struct spi_message  m;

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    return lmk04828spi_sync(spidev, &m);
}

static ssize_t lmk04828spi_sync_read(struct lmk04828_t *spidev, size_t len)
{
    struct spi_transfer t = {
            .rx_buf     = spidev->rx_buffer,
            .len        = len,
            .speed_hz   = spidev->speed_hz,
        };
    struct spi_message  m;

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    return lmk04828spi_sync(spidev, &m);
}


int lmk04828_write_reg(int reg, unsigned char value)
{
    unsigned char cmd[3]={0};
    unsigned char addr_h = reg >> 8;
    unsigned char addr_l = reg & 0xff;
    cmd[0] = addr_h & MASK_ADDR_H;
    cmd[0] &= ~ MASK_SEVE;
    cmd[0] &= ~ MASK_WRITE;
    cmd[1] = addr_l;
    cmd[2] = value;
    lmk04828->tx_buffer = cmd;
    lmk04828->speed_hz = SPI_SPEED_HZ;

    return lmk04828spi_sync_write(lmk04828, 3);
}
EXPORT_SYMBOL(lmk04828_write_reg);

int lmk04828_read_reg(int reg, unsigned char buff[1])
{
    unsigned char cmd[3]={0};
    unsigned char addr_h = reg >> 8;
    unsigned char addr_l = reg & 0xff;
    cmd[0] = addr_h & MASK_ADDR_H;
    cmd[0] &= ~ MASK_SEVE;
    cmd[0] |=  MASK_READ;
    cmd[1] = addr_l;
    cmd[2] = 0;

    lmk04828->tx_buffer = cmd;
    lmk04828->rx_buffer = buff;
    lmk04828->speed_hz = SPI_SPEED_HZ;

    return spi_write_then_read(lmk04828->spi, cmd,2, buff, 1);



}
EXPORT_SYMBOL(lmk04828_read_reg);

int lmk04828_spi_read(unsigned reg)
{
	unsigned char buf[3];
	int ret;

	buf[0] = 0x80 | (reg >> 8);
	buf[1] = reg & 0xFF;
	ret = spi_write_then_read(lmk04828->spi, &buf[0], 2, &buf[2], 1);

	dev_dbg(&lmk04828->spi->dev, "%s: REG: 0x%X VAL: 0x%X (%d)\n",
		__func__, reg, buf[2], ret);

	if (ret < 0) {
		dev_err(&lmk04828->spi->dev, "%s: failed (%d)\n",
			__func__, ret);
		return ret;
	}

	return buf[2];
}


int  lmk04828_open(struct inode *node, struct file *pfile)
{

    return 0;
}
int  lmk04828_release(struct inode *node, struct file *pfile)
{

    return 0;
}


loff_t lmk04828_llseek(struct file *pfile, loff_t off, int len)
{
    lmk04828->cur_index = off;
    return 0;
}

ssize_t lmk04828_read(struct file *pfile, char __user *buf, size_t size, loff_t  *off)
{
    unsigned char kbuf[1]={0};
    
    mutex_lock(&lmk04828->buf_lock);
    lmk04828_read_reg(lmk04828->cur_index, kbuf);
    mutex_unlock(&lmk04828->buf_lock);
    
    return copy_to_user(buf, kbuf, 1);
}

ssize_t  lmk04828_write(struct file *pfile, const char __user *buf,
        size_t size, loff_t  *off)
{

    unsigned char kbuf[1]={0};
    int ret=0;
    
    if ( 0 > copy_from_user(kbuf, buf, 1) ) {
        printk(KERN_INFO "%s %s %d \n","copy to kbuf eer",__func__,__LINE__);
    }
    
    mutex_lock(&lmk04828->buf_lock);
    ret = lmk04828_write_reg(lmk04828->cur_index,kbuf[0]);
    mutex_unlock(&lmk04828->buf_lock);

    return ret;
}

long  lmk04828_ioctl(struct file *pfile, unsigned int cmd, unsigned long arg)
{
    switch(cmd) {
        case GET_REG:

            break;
        case SET_REG:

            break;
        default:
            printk("invalid argument\n");
            return -EINVAL;
    }

    return 0;
}

int  lmk04828_reg_pll2_n(char enable, unsigned int val)
{
    int ret1,ret2,ret3;
    if (enable == 0) {
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
    

    if (0 > lmk04828_write_reg(0, 0x80) ) {
        return -1;
    }
    msleep(100);

    //PIN MUX SET
    if (0 > lmk04828_write_reg(0x14A, 0X33)) {
        return -1;
    }

    if (0 > lmk04828_write_reg(0, 0x10) ) {
        return -1;
    }
    msleep(100);

    if (!((0xD0 == lmk04828_spi_read(0x004)) && 
         (0x5B == lmk04828_spi_read(0x005)) && 
          (0x20 == lmk04828_spi_read(0x006)))) {
          
        dev_err(&lmk04828->spi->dev, "Initialization falied!\n");
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

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = lmk04828_open,
    .release = lmk04828_release,
    .read = lmk04828_read,
    .write = lmk04828_write,
    .llseek = lmk04828_llseek,
    .unlocked_ioctl = lmk04828_ioctl,
};

static int lmk04828_spi_probe(struct spi_device *spi)
{
    struct lmk04828_t   *lmk04828_data;
    struct device_node  *np;
    u32 addrtmp;
    dev_info(&spi->dev, "LMK04828 probed\n");

    /* Allocate driver data */
    lmk04828_data = kzalloc(sizeof(*lmk04828_data), GFP_KERNEL);
    if (!lmk04828_data)
        return -ENOMEM;

    /* Initialize the driver data */
    lmk04828_data->spi = spi;
    lmk04828_data->speed_hz = SPI_SPEED_HZ;
    spin_lock_init(&lmk04828_data->spi_lock);
    mutex_init(&lmk04828_data->buf_lock);

    INIT_LIST_HEAD(&lmk04828_data->device_entry);


    lmk04828_data->misc_dev.fops = &fops;
    lmk04828_data->misc_dev.name = "lmk04828";
    //主设备号恒为10,自动分配次设备号
    lmk04828_data->misc_dev.minor = MISC_DYNAMIC_MINOR;
    //3.注册misc设备
    misc_register(&lmk04828_data->misc_dev);

    np = of_find_node_by_name(NULL, "lmk04828-spi");
    if (NULL == np) {
        dev_err(&spi->dev, "node lmk04828 not find\n");
        return -1;
    }
    
    if(0 > of_property_read_u32_index(np, "pl-cs-addr" , 0, &addrtmp) ) {
        dev_err(&spi->dev, "pl-cs-addr  property not find\n");
    }
    if(0 > of_property_read_u32_index(np, "pl-cs-val" , 0, &lmk04828_data->pl_cs_val)) {
        dev_err(&spi->dev, "pl-cs-val  property not find\n");
    }
    lmk04828_data->pl_cs_addr = ioremap(addrtmp, 4);
    dev_info(&spi->dev,"val= %x, addrtmp = %x, ioremap-address= %x \n", lmk04828_data->pl_cs_val,
                        addrtmp, lmk04828_data->pl_cs_addr);

    iowrite32( lmk04828_data->pl_cs_val, lmk04828_data->pl_cs_addr);
    
    lmk04828 = lmk04828_data;
    spi_set_drvdata(spi, lmk04828_data);

    //LMK04828 REGISTER INIT
    lmk04828_reg_init();

    return 0;
}

static int lmk04828_spi_remove(struct spi_device *spi)
{
    struct lmk04828_t *lmk04828_data = spi_get_drvdata(spi);
    //注销misc设备
    misc_deregister(&lmk04828_data->misc_dev);
    //释放
    kfree(lmk04828_data);
    return 0;
}
static const struct of_device_id lmk04828_dt_ids[] = {
    { .compatible = "lmk04828" },
    {},
};

static const struct spi_device_id lmk04828_spi_id[] = {
    {"lmk04828"},
    {}
};
MODULE_DEVICE_TABLE(spi, lmk04828_spi_id);
static struct spi_driver lmk04828_spi_driver = {
    .driver = {
        .name   = "lmk04828",
        .owner = THIS_MODULE,
        .of_match_table = of_match_ptr(lmk04828_dt_ids),
    },
    .probe      = lmk04828_spi_probe,
    .remove     = lmk04828_spi_remove,
    .id_table   = lmk04828_spi_id,
};

module_spi_driver(lmk04828_spi_driver);
MODULE_AUTHOR("Thomas Chou <thomas@wytron.com.tw>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("lmk04828-spi");


