/*
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define DRIVER_NAME	"xilinx_sdfec"
#define DRIVER_VERSION	"0.3"
#define DRIVER_MAX_DEV	BIT(MINORBITS)

#if 0
/**
 * struct jhello_dev - Driver data for SDFEC
 * @regs: device physical base address
 * @dev: pointer to device struct
 * @state: State of the SDFEC device
 * @config: Configuration of the SDFEC device
 * @intr_enabled: indicates IRQ enabled
 * @wr_protect: indicates Write Protect enabled
 * @isr_err_count: Count of ISR errors
 * @cecc_count: Count of Correctable ECC errors (SBE)
 * @uecc_count: Count of Uncorrectable ECC errors (MBE)
 * @open_count: Count of char device being opened
 * @irq: IRQ number
 * @jhello_cdev: Character device handle
 * @waitq: Driver wait queue
 *
 * This structure contains necessary state for SDFEC driver to operate
 */
struct jhello_dev {
	void __iomem *regs;
	struct device *dev;
	enum jhello_state state;
	struct jhello_config config;
	bool intr_enabled;
	bool wr_protect;
	atomic_t isr_err_count;
	atomic_t cecc_count;
	atomic_t uecc_count;
	atomic_t open_count;
	int  irq;
	struct cdev jhello_cdev;
	wait_queue_head_t waitq;
};

static inline void
jhello_regwrite(struct jhello_dev *xsdfec, u32 addr, u32 value)
{
	if (xsdfec->wr_protect) {
		dev_err(xsdfec->dev, "SDFEC in write protect");
		return;
	}

	dev_dbg(xsdfec->dev,
		"Writing 0x%x to offset 0x%x", value, addr);
	iowrite32(value, xsdfec->regs + addr);
}

static inline u32
jhello_regread(struct jhello_dev *xsdfec, u32 addr)
{
	u32 rval;

	rval = ioread32(xsdfec->regs + addr);
	dev_dbg(xsdfec->dev,
		"Read value = 0x%x from offset 0x%x",
		rval, addr);
	return rval;
}


static int
jhello_dev_open(struct inode *iptr, struct file *fptr)
{
	struct jhello_dev *xsdfec;

	xsdfec = container_of(iptr->i_cdev, struct jhello_dev, jhello_cdev);
	if (!xsdfec)
		return  -EAGAIN;

	/* Only one open per device at a time */
	if (!atomic_dec_and_test(&xsdfec->open_count)) {
		atomic_inc(&xsdfec->open_count);
		return -EBUSY;
	}

	fptr->private_data = xsdfec;
	return 0;
}

static int
jhello_isr_enable(struct jhello_dev *xsdfec, bool enable)
{
	u32 mask_read;

	if (enable) {
		/* Enable */
		jhello_regwrite(xsdfec, XSDFEC_IER_ADDR,
				XSDFEC_ISR_MASK);
		mask_read = jhello_regread(xsdfec, XSDFEC_IMR_ADDR);
		if (mask_read & XSDFEC_ISR_MASK) {
			dev_err(xsdfec->dev,
				"SDFEC enabling irq with IER failed");
			return -EIO;
		}
	} else {
		/* Disable */
		jhello_regwrite(xsdfec, XSDFEC_IDR_ADDR,
				XSDFEC_ISR_MASK);
		mask_read = jhello_regread(xsdfec, XSDFEC_IMR_ADDR);
		if ((mask_read & XSDFEC_ISR_MASK) != XSDFEC_ISR_MASK) {
			dev_err(xsdfec->dev,
				"SDFEC disabling irq with IDR failed");
			return -EIO;
		}
	}
	return 0;
}

static int
jhello_set_irq(struct jhello_dev *xsdfec, void __user *arg)
{
	struct jhello_irq  irq;
	int err = 0;

	err = copy_from_user(&irq, arg, sizeof(irq));
	if (err) {
		dev_err(xsdfec->dev, "%s failed for SDFEC%d",
			__func__, xsdfec->config.fec_id);
		return -EFAULT;
	}

	/* Setup tlast related IRQ */
	if (irq.enable_isr) {
		err = jhello_isr_enable(xsdfec, true);
		if (err < 0)
			return err;
	}

	/* Setup ECC related IRQ */
	if (irq.enable_ecc_isr) {
		err = jhello_ecc_isr_enable(xsdfec, true);
		if (err < 0)
			return err;
	}

	return 0;
}
#endif

static int
jhello_probe(struct platform_device *pdev)
{
	struct jhello_dev *xsdfec;
	struct device *dev;
	struct device *dev_create;
	struct resource *res;
	int err;
	bool irq_enabled = true;

	xsdfec = devm_kzalloc(&pdev->dev, sizeof(*xsdfec), GFP_KERNEL);
	if (!xsdfec)
		return -ENOMEM;

	xsdfec->dev = &pdev->dev;
	xsdfec->config.fec_id = atomic_read(&jhello_ndevs);

	dev = xsdfec->dev;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	xsdfec->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(xsdfec->regs)) {
		dev_err(dev, "Unable to map resource");
		err = PTR_ERR(xsdfec->regs);
		goto err_jhello_dev;
	}

	xsdfec->irq = platform_get_irq(pdev, 0);
	if (xsdfec->irq < 0) {
		dev_dbg(dev, "platform_get_irq failed");
		irq_enabled = false;
	}

	err = jhello_parse_of(xsdfec);
	if (err < 0)
		goto err_jhello_dev;

	/* Save driver private data */
	platform_set_drvdata(pdev, xsdfec);

	if (irq_enabled) {
		init_waitqueue_head(&xsdfec->waitq);
		/* Register IRQ thread */
		err = devm_request_threaded_irq(dev, xsdfec->irq, NULL,
						jhello_irq_thread,
						IRQF_ONESHOT,
						"xilinx-sdfec16",
						xsdfec);
		if (err < 0) {
			dev_err(dev, "unable to request IRQ%d", xsdfec->irq);
			goto err_jhello_dev;
		}
	}

	cdev_init(&xsdfec->jhello_cdev, &jhello_fops);
	xsdfec->jhello_cdev.owner = THIS_MODULE;
	err = cdev_add(&xsdfec->jhello_cdev,
		       MKDEV(MAJOR(jhello_devt), xsdfec->config.fec_id), 1);
	if (err < 0) {
		dev_err(dev, "cdev_add failed");
		err = -EIO;
		goto err_jhello_dev;
	}

	if (!jhello_class) {
		err = -EIO;
		dev_err(dev, "xsdfec class not created correctly");
		goto err_jhello_cdev;
	}

	dev_create = device_create(jhello_class, dev,
				   MKDEV(MAJOR(jhello_devt),
					 xsdfec->config.fec_id),
				   xsdfec, "xsdfec%d", xsdfec->config.fec_id);
	if (IS_ERR(dev_create)) {
		dev_err(dev, "unable to create device");
		err = PTR_ERR(dev_create);
		goto err_jhello_cdev;
	}

	atomic_set(&xsdfec->open_count, 1);
	dev_info(dev, "XSDFEC%d Probe Successful", xsdfec->config.fec_id);
	atomic_inc(&jhello_ndevs);
	return 0;

	/* Failure cleanup */
err_jhello_cdev:
	cdev_del(&xsdfec->jhello_cdev);
err_jhello_dev:
	return err;
}

static int
jhello_remove(struct platform_device *pdev)
{
	struct jhello_dev *xsdfec;
	struct device *dev = &pdev->dev;

	xsdfec = platform_get_drvdata(pdev);
	if (!xsdfec)
		return -ENODEV;
	dev = xsdfec->dev;
	if (!jhello_class) {
		dev_err(dev, "jhello_class is NULL");
		return -EIO;
	}

	device_destroy(jhello_class,
		       MKDEV(MAJOR(jhello_devt), xsdfec->config.fec_id));
	cdev_del(&xsdfec->jhello_cdev);
	atomic_dec(&jhello_ndevs);
	return 0;
}

static const struct of_device_id jhello_of_match[] = {
	{ .compatible = "xlnx,sd-fec-1.1", },
	{ /* end of table */ }
};
MODULE_DEVICE_TABLE(of, jhello_of_match);

static struct platform_driver jhello_driver = {
	.driver = {
		.name = "xilinx-sdfec",
		.of_match_table = jhello_of_match,
	},
	.probe = jhello_probe,
	.remove =  jhello_remove,
};

#if 1
/*memory {
        device_type = "memory";
        reg = <0x00000000 0x30000000>;
};*/

static int
jmem_write(void)
{
	dma_unmap_single( NULL, read_virt_addr, user_buffer, length);
	
	// The user_buffer containing 0xDB is correctly copied to 0x30000000 for length 288.
	nbytes_failed = copy_from_user( read_virt_addr, user_buffer, length);
	
	// read_phys_addr returned from dma_map_single is 0x31000000
	read_phys_addr = dma_map_single(NULL, read_virt_addr, length, DMA_TO_DEVICE);
	
	read_phys_addr = 0x30000000;  //Force read_phys_addr equal to ioremap read_phys_addr arg
	
	write_phys_addr = dma_map_single( NULL, write_virt_addr, length, DMA_FROM_DEVICE);
	// Because write_virt_addr is 0xF0200000, dma_map_single returns 0x30200000
	
	write_phys_addr = 0x38000000;  //Force write_phys_addr equal to ioremap write_phys_addr arg
	
	// FPGA readaddr reg set to read_phys_addr, FPGA writeaddr reg set to write_phys_addr
	// Set FPGA the readlen and writelen registers
	// DMA read status is busy
	// FPGA rd status is busy, the user_buffer data never arrives at write_phys_addr
}

static int
jmem_read(void)
{
	dma_unmap_single( NULL, write_virt_addr, length, DMA_FROM_DEVICE);
	nbytes_failed = copy_to_user( user_buffer, write_virt_addr, length);
}

#endif

static int __init jhello_init_mod(void)
{
	int err=0;

#if 0
	jhello_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(jhello_class)) {
		err = PTR_ERR(jhello_class);
		pr_err("%s : Unable to register xsdfec class", __func__);
		return err;
	}

	err = alloc_chrdev_region(&jhello_devt,
				  0, DRIVER_MAX_DEV, DRIVER_NAME);
	if (err < 0) {
		pr_err("%s : Unable to get major number", __func__);
		goto err_jhello_class;
	}

	err = platform_driver_register(&jhello_driver);
	if (err < 0) {
		pr_err("%s Unabled to register %s driver",
		       __func__, DRIVER_NAME);
		goto err_jhello_drv;
	}
	return 0;

	/* Error Path */
err_jhello_drv:
	unregister_chrdev_region(jhello_devt, DRIVER_MAX_DEV);
err_jhello_class:
	class_destroy(jhello_class);

//#else

#include <../arch/arm/include/asm/memory.h>  //for virt_to_phys
#define LENGTH 4064
	int status = 0, i;
	char *vbufptrs, *pbufptrs;
	phys_addr_t kbuffer_phys;
	char *kbuffer_p = (char *)kmalloc((size_t)LENGTH, GFP_ATOMIC | __GFP_COLD | __GFP_DMA);
	if (kbuffer_p != NULL) {
		printk(KERN_INFO "pbdma::init_module: %d PL READaddress=%x\n", i, (int)kbuffer_p);
		status += 0;
		vbufptrs[i] = kbuffer_p;
		printk(KERN_INFO "pbdma::init_module: writing to address %x\n", (int)kbuffer_p);
		memset( kbuffer_p, 0x5a, LENGTH);
		kbuffer_phys = virt_to_phys(kbuffer_p);
		pbufptrs[i] = kbuffer_phys;
		printk(KERN_INFO "pbdma::init_module: physical address %x pageoffset %x physoffset %x\n",
			(int) kbuffer_phys, (int) PAGE_OFFSET, (int) PHYS_OFFSET);
	}
#else
	read_phys_addr = 0x30000000;  write_phys_addr = 0x38000000;
	read_virt_addr = ioremap (read_phys_addr, 0x07E00000);	//read_virt_addr is 0xF1000000
	write_virt_addr = ioremap(write_phys_addr, 0x00100000);//write_virt_addr is 0xF0200000
	mmio = ioremap(0xB8800000, 0x200); // FPGA registers
#endif

	return err;
}

static void __exit jhello_cleanup_mod(void)
{
#if 0
	platform_driver_unregister(&jhello_driver);
	unregister_chrdev_region(jhello_devt, DRIVER_MAX_DEV);
	class_destroy(jhello_class);
	jhello_class = NULL;
#else
#endif
}

module_init(jhello_init_mod);
module_exit(jhello_cleanup_mod);

MODULE_AUTHOR("Jacky Inc");
MODULE_DESCRIPTION("Jacky-Hello Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
