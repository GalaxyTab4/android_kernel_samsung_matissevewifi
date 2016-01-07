/*
 * Copyright (C) 2010 Trusted Logic S.A.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/i2c.h>
#include <linux/clk.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/pn547.h>
#include <linux/wakelock.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#ifdef CONFIG_NFC_PN547_8916_USE_BBCLK2
#include <linux/clk.h>
#endif
#define MAX_BUFFER_SIZE		512

#define MAX_TRY_I2C_READ	10
#define I2C_ADDR_READ_L		0x51
#define I2C_ADDR_READ_H		0x57
#define NFC_DEBUG	0

struct pn547_dev {
	wait_queue_head_t read_wq;
	struct mutex read_mutex;
	struct i2c_client *client;
	struct miscdevice pn547_device;
	void (*conf_gpio) (void);
	unsigned int ven_gpio;
	unsigned int firm_gpio;
	unsigned int irq_gpio;
#ifdef CONFIG_NFC_PN547_PVDD_EN_CONTROL
	unsigned int pvdd_en;
#endif
#ifdef CONFIG_NFC_PN547_8916_CLK_CTL
	unsigned int nfc_enable;
#endif
#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	unsigned int ese_pwr_req;
	struct mutex        p61_state_mutex; /* used to make p61_current_state flag secure */
	p61_access_state_t  p61_current_state; /* stores the current P61 state */
	bool                nfc_ven_enabled; /* stores the VEN pin state powered by Nfc */
	bool                spi_ven_enabled; /* stores the VEN pin state powered by Spi */
#endif

	atomic_t irq_enabled;
	atomic_t read_flag;
	bool cancel_read;
	struct wake_lock nfc_wake_lock;
#ifdef CONFIG_NFC_PN547_8916_USE_BBCLK2
	struct clk *nfc_clock;
#endif
#ifdef CONFIG_NFC_PN547_LDO_CONTROL
	struct regulator *L16;
#endif

};
#ifdef CONFIG_NFC_PN547_8916_CLK_CTL
#define PN547_NFC_GET_INFO(dev) i2c_get_clientdata(to_i2c_client(dev))
#endif
#ifdef CONFIG_NFC_PN547_LDO_CONTROL
int nfc_power_onoff(struct pn547_dev *data, bool onoff);
#endif
static irqreturn_t pn547_dev_irq_handler(int irq, void *dev_id)
{
	struct pn547_dev *pn547_dev = dev_id;

	if (!gpio_get_value(pn547_dev->irq_gpio)) {
#if NFC_DEBUG
		pr_err("%s, irq_gpio = %d\n", __func__,
			gpio_get_value(pn547_dev->irq_gpio));
#endif
		return IRQ_HANDLED;
	}

	/* Wake up waiting readers */
	atomic_set(&pn547_dev->read_flag, 1);
	wake_up(&pn547_dev->read_wq);


#if NFC_DEBUG
	pr_info("pn547 : call\n");
#endif
	wake_lock_timeout(&pn547_dev->nfc_wake_lock, 2*HZ);
	return IRQ_HANDLED;
}


static ssize_t pn547_dev_read(struct file *filp, char __user *buf,
			      size_t count, loff_t *offset)
{
	struct pn547_dev *pn547_dev = filp->private_data;
	char tmp[MAX_BUFFER_SIZE] = {0, };
	int ret = 0;
#ifdef CONFIG_NFC_PN544
	int readingWatchdog = 0;
#endif

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	pr_debug("%s : reading %zu bytes. irq=%s\n", __func__, count,
		 gpio_get_value(pn547_dev->irq_gpio) ? "1" : "0");

#if NFC_DEBUG
	pr_info("pn547 : + r\n");
#endif

	mutex_lock(&pn547_dev->read_mutex);

#ifdef CONFIG_NFC_PN544
wait_irq:
#endif

	if (!gpio_get_value(pn547_dev->irq_gpio)) {
		atomic_set(&pn547_dev->read_flag, 0);
		if (filp->f_flags & O_NONBLOCK) {
			pr_info("%s : O_NONBLOCK\n", __func__);
			ret = -EAGAIN;
			goto fail;
		}

#if NFC_DEBUG
		pr_info("pn547: wait_event_interruptible : in\n");
#endif
		if (!gpio_get_value(pn547_dev->irq_gpio))
			ret = wait_event_interruptible(pn547_dev->read_wq,
				atomic_read(&pn547_dev->read_flag));

#if NFC_DEBUG
		pr_info("pn547 :   h\n");
#endif

		if (pn547_dev->cancel_read) {
			pn547_dev->cancel_read = false;
			ret = -1;
			goto fail;
		}

		if (ret)
			goto fail;

	}

	/* Read data */
	ret = i2c_master_recv(pn547_dev->client, tmp, count);

#ifdef CONFIG_NFC_PN544
	/* If bad frame(from 0x51 to 0x57) is received from pn65n,
	* we need to read again after waiting that IRQ is down.
	* if data is not ready, pn65n will send from 0x51 to 0x57. */
	if ((I2C_ADDR_READ_L <= tmp[0] && tmp[0] <= I2C_ADDR_READ_H)
		&& readingWatchdog < MAX_TRY_I2C_READ) {
		pr_warn("%s: data is not ready yet.data = 0x%x, cnt=%d\n",
			__func__, tmp[0], readingWatchdog);
		usleep_range(2000, 2000); /* sleep 2ms to wait for IRQ */
		readingWatchdog++;
		goto wait_irq;
	}
#endif

#if NFC_DEBUG
	pr_info("pn547: i2c_master_recv\n");
#endif
	mutex_unlock(&pn547_dev->read_mutex);
	if (ret < 0) {
		pr_err("%s: i2c_master_recv returned %d\n", __func__,
				ret);
		return ret;
	}

	if (ret > count) {
		pr_err("%s: received too many bytes from i2c (%d)\n",
				__func__, ret);
		return -EIO;
	}

	if (copy_to_user(buf, tmp, ret)) {
		pr_err("%s : failed to copy to user space\n", __func__);
		return -EFAULT;
	}
	return ret;

fail:
	mutex_unlock(&pn547_dev->read_mutex);
	return ret;
}

static ssize_t pn547_dev_write(struct file *filp, const char __user *buf,
			       size_t count, loff_t *offset)
{
	struct pn547_dev *pn547_dev;
	char tmp[MAX_BUFFER_SIZE] = {0, };
	int ret = 0, retry = 2;

	pn547_dev = filp->private_data;

#if NFC_DEBUG
	pr_info("pn547 : + w\n");
#endif

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	if (copy_from_user(tmp, buf, count)) {
		pr_err("%s : failed to copy from user space\n", __func__);
		return -EFAULT;
	}

	pr_debug("%s : writing %zu bytes.\n", __func__, count);
	/* Write data */
	do {
		retry--;
		ret = i2c_master_send(pn547_dev->client, tmp, count);
		if (ret == count)
			break;
		usleep_range(6000, 10000); /* Retry, chip was in standby */
#if NFC_DEBUG
		pr_debug("%s : retry = %d\n", __func__, retry);
#endif
	} while (retry);

#if NFC_DEBUG
	pr_info("pn547 : - w\n");
#endif

	if (ret != count) {
		pr_err("%s : i2c_master_send returned %d\n", __func__, ret);
		ret = -EIO;
	}

	return ret;
}

#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
static void p61_update_access_state(struct pn547_dev *pn547_dev, p61_access_state_t current_state)
{
    pr_info("%s: Enter current_state = %d\n", __func__, pn547_dev->p61_current_state);
    if (current_state != P61_STATE_INVALID)
        pn547_dev->p61_current_state = current_state;
    pr_info("%s: Exit current_state = %d\n", __func__, pn547_dev->p61_current_state);
}

static void p61_get_access_state(struct pn547_dev *pn547_dev, p61_access_state_t *current_state)
{

    if (current_state == NULL) {
        //*current_state = P61_STATE_INVALID;
        pr_err("%s : invalid state of p61_access_state_t current state  \n", __func__);
    } else {
        *current_state = pn547_dev->p61_current_state;
    }
}
static void p61_access_lock(struct pn547_dev *pn547_dev)
{
    pr_info("%s: Enter\n", __func__);
    mutex_lock(&pn547_dev->p61_state_mutex);
    pr_info("%s: Exit\n", __func__);
}
static void p61_access_unlock(struct pn547_dev *pn547_dev)
{
    pr_info("%s: Enter\n", __func__);
    mutex_unlock(&pn547_dev->p61_state_mutex);
    pr_info("%s: Exit\n", __func__);
}
#endif

static int pn547_dev_open(struct inode *inode, struct file *filp)
{
	struct pn547_dev *pn547_dev = container_of(filp->private_data,
						   struct pn547_dev,
						   pn547_device);
	filp->private_data = pn547_dev;

	pr_debug("%s : %d,%d\n", __func__, imajor(inode), iminor(inode));

	return 0;
}

static long pn547_dev_ioctl(struct file *filp,
			   unsigned int cmd, unsigned long arg)
{
	struct pn547_dev *pn547_dev = filp->private_data;

#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	p61_access_lock(pn547_dev);
	switch (cmd) {
	case PN547_SET_PWR:
	{
		p61_access_state_t current_state = P61_STATE_INVALID;
		p61_get_access_state(pn547_dev, &current_state);

		if (arg == 2) {
			if (current_state == P61_STATE_SPI)
			{
			    /* NFCC fw/download should not be allowed if p61 is used
			     * by SPI
			     */
			    pr_info("%s NFCC should not be allowed to reset/FW download \n", __func__);
			    p61_access_unlock(pn547_dev);
			    return -EBUSY; /* Device or resource busy */
			}
			pn547_dev->nfc_ven_enabled = true;
			if (pn547_dev->spi_ven_enabled == false)
			{
				/* power on with firmware download (requires hw reset)
				 */
		#ifdef CONFIG_SEC_MILLETWIFI_COMMON
				gpio_direction_output(pn547_dev->ven_gpio, 1);
		#endif
				gpio_set_value_cansleep(pn547_dev->ven_gpio, 1);
				gpio_set_value(pn547_dev->firm_gpio, 1);
				usleep_range(4900, 5000);
				gpio_set_value_cansleep(pn547_dev->ven_gpio, 0);
				usleep_range(4900, 5000);
				gpio_set_value_cansleep(pn547_dev->ven_gpio, 1);
				usleep_range(4900, 5000);
				if (atomic_read(&pn547_dev->irq_enabled) == 0) {
					atomic_set(&pn547_dev->irq_enabled, 1);
					enable_irq(pn547_dev->client->irq);
					enable_irq_wake(pn547_dev->client->irq);
				}
				pr_info("%s power on with firmware, irq=%d\n", __func__,
				atomic_read(&pn547_dev->irq_enabled));
			}
		} else if (arg == 1) {
			/* power on */
			if (pn547_dev->conf_gpio)
				pn547_dev->conf_gpio();
			if (current_state != P61_STATE_SPI) {
			    p61_update_access_state(pn547_dev, P61_STATE_IDLE);
			}
			gpio_set_value(pn547_dev->firm_gpio, 0);

			pn547_dev->nfc_ven_enabled = true;
			if (pn547_dev->spi_ven_enabled == false) {
#ifdef CONFIG_SEC_MILLETWIFI_COMMON
				gpio_direction_output(pn547_dev->ven_gpio, 1);
#endif
				gpio_set_value_cansleep(pn547_dev->ven_gpio, 1);
			}
			usleep_range(4900, 5000);
			if (atomic_read(&pn547_dev->irq_enabled) == 0) {
				atomic_set(&pn547_dev->irq_enabled, 1);
				enable_irq(pn547_dev->client->irq);
				enable_irq_wake(pn547_dev->client->irq);
			}
			pr_info("%s power on, irq=%d\n", __func__,
				atomic_read(&pn547_dev->irq_enabled));
		} else if (arg == 0) {
			/* power off */
			if (atomic_read(&pn547_dev->irq_enabled) == 1) {
				atomic_set(&pn547_dev->irq_enabled, 0);
				disable_irq_wake(pn547_dev->client->irq);
				disable_irq_nosync(pn547_dev->client->irq);
			}
			if (current_state != P61_STATE_SPI) {
				p61_update_access_state(pn547_dev, P61_STATE_IDLE);
			}
			pr_info("%s power off, irq=%d\n", __func__,
				atomic_read(&pn547_dev->irq_enabled));
			gpio_set_value(pn547_dev->firm_gpio, 0);

			pn547_dev->nfc_ven_enabled = false;
			/* Don't change Ven state if spi made it high */
			if (pn547_dev->spi_ven_enabled == false) {
#ifdef CONFIG_SEC_MILLETWIFI_COMMON
				gpio_direction_output(pn547_dev->ven_gpio, 0);
#endif
				gpio_set_value_cansleep(pn547_dev->ven_gpio, 0);
			}
			usleep_range(4900, 5000);
		} else if (arg == 3) {
			pr_info("%s Read Cancel\n", __func__);
			pn547_dev->cancel_read = true;
			atomic_set(&pn547_dev->read_flag, 1);
			wake_up(&pn547_dev->read_wq);
		} else {
			pr_err("%s bad arg %lu\n", __func__, arg);
			/* changed the p61 state to idle*/
			p61_access_unlock(pn547_dev);
			return -EINVAL;
		}
	}
	break;

	case P61_SET_SPI_PWR:
	{
		    p61_access_state_t current_state = P61_STATE_INVALID;
		    p61_get_access_state(pn547_dev, &current_state);
		    if (arg == 1) {
		        pr_info("%s : PN61_SET_SPI_PWR - power on ese\n", __func__);
		        if (current_state == P61_STATE_IDLE) {
		            p61_update_access_state(pn547_dev, P61_STATE_SPI);
		            pn547_dev->spi_ven_enabled = true;
		            if (pn547_dev->nfc_ven_enabled == false) {
		                /* provide power to NFCC if, NFC service not provided */
		                gpio_set_value(pn547_dev->ven_gpio, 1);
		                msleep(10);
		            }
		            /* pull the gpio to high once NFCC is power on*/
		            gpio_set_value(pn547_dev->ese_pwr_req, 1);
		        } else {
		            pr_info("%s : PN61_SET_SPI_PWR -  p61 is busy \n", __func__);
		            p61_access_unlock(pn547_dev);
		            return -EBUSY; /* Device or resource busy */
		        }
		    } else if (arg == 0) {
		        pr_info("%s : PN61_SET_SPI_PWR - power off ese\n", __func__);
		        if ( current_state == P61_STATE_SPI){
		            gpio_set_value(pn547_dev->ese_pwr_req, 0);
		            pn547_dev->spi_ven_enabled = false;
		            if (pn547_dev->nfc_ven_enabled == false) {
		                gpio_set_value(pn547_dev->ven_gpio, 0);
		                msleep(10);
		            }
		            p61_update_access_state(pn547_dev, P61_STATE_IDLE);
		        } else {
		            pr_err("%s : PN61_SET_SPI_PWR - failed, current_state = %d \n",
		                    __func__, pn547_dev->p61_current_state);
		            p61_access_unlock(pn547_dev);
		            return -EPERM; /* Operation not permitted */
		        }
		    }else if (arg == 2) {
		        pr_info("%s : PN61_SET_SPI_PWR - reset\n", __func__);
		        if ((current_state == P61_STATE_IDLE) || (current_state == P61_STATE_SPI)) {
		            if (pn547_dev->spi_ven_enabled == false)
		            {
		                pn547_dev->spi_ven_enabled = true;
		                if (pn547_dev->nfc_ven_enabled == false) {
		                    /* provide power to NFCC if, NFC service not provided */
		                    gpio_set_value(pn547_dev->ven_gpio, 1);
		                    msleep(10);
		                }
		            }
		            gpio_set_value(pn547_dev->ese_pwr_req, 0);
		            msleep(10);
		            gpio_set_value(pn547_dev->ese_pwr_req, 1);
		            msleep(10);
		        } else {
		            pr_info("%s : PN61_SET_SPI_PWR - reset  p61 is busy \n", __func__);
		            p61_access_unlock(pn547_dev);
		            return -EBUSY; /* Device or resource busy */
		        }
		    }
		    else {
		        pr_info("%s bad ese pwr arg %lu\n", __func__, arg);
		        p61_access_unlock(pn547_dev);
		        return -EBADRQC; /* Invalid request code */
		    }
	}
	break;

	case P61_GET_PWR_STATUS:
	{
		p61_access_state_t current_state = P61_STATE_INVALID;
		p61_get_access_state(pn547_dev, &current_state);
		put_user(current_state, (int __user *)&arg);
	}
	break;

	case P61_SET_WIRED_ACCESS:
	{
		p61_access_state_t current_state = P61_STATE_INVALID;
		p61_get_access_state(pn547_dev, &current_state);
		if (arg == 1) {
			pr_info("%s : P61_SET_WIRED_ACCESS - enabling\n", __func__);
			if (current_state == P61_STATE_IDLE){
				p61_update_access_state(pn547_dev, P61_STATE_WIRED);
			} else {
				pr_info("%s : P61_SET_WIRED_ACCESS -  p61 is busy \n", __func__);
				p61_access_unlock(pn547_dev);
				return -EBUSY; /* Device or resource busy */
			}
		} else if (arg == 0) {
			pr_info("%s : P61_SET_WIRED_ACCESS - disabling \n", __func__);
			if (current_state == P61_STATE_WIRED){
				p61_update_access_state(pn547_dev, P61_STATE_IDLE);
			} else {
				pr_err("%s : P61_SET_WIRED_ACCESS - failed, current_state = %d \n",
						__func__, pn547_dev->p61_current_state);
				p61_access_unlock(pn547_dev);
				return -EPERM; /* Operation not permitted */
			}
		}
		else {
			pr_info("%s P61_SET_WIRED_ACCESS - bad arg %lu\n", __func__, arg);
			p61_access_unlock(pn547_dev);
			return -EBADRQC; /* Invalid request code */
		}
	}
	break;

	default:
		pr_err("%s bad ioctl %u\n", __func__, cmd);
		p61_access_unlock(pn547_dev);
		return -EINVAL;
	}
	p61_access_unlock(pn547_dev);
#else	/*CONFIG_NFC_PN547_ESE_SUPPORT*/
	switch (cmd) {
	case PN547_SET_PWR:
		if (arg == 2) {
			/* power on with firmware download (requires hw reset)
			 */
			gpio_set_value_cansleep(pn547_dev->ven_gpio, 1);
			gpio_set_value(pn547_dev->firm_gpio, 1);
			usleep_range(10000, 10050);
			gpio_set_value_cansleep(pn547_dev->ven_gpio, 0);
			usleep_range(10000, 10050);
			gpio_set_value_cansleep(pn547_dev->ven_gpio, 1);
			usleep_range(10000, 10050);
			if (atomic_read(&pn547_dev->irq_enabled) == 0) {
				atomic_set(&pn547_dev->irq_enabled, 1);
				enable_irq(pn547_dev->client->irq);
				enable_irq_wake(pn547_dev->client->irq);
			}
			pr_info("%s power on with firmware, irq=%d\n", __func__,
				atomic_read(&pn547_dev->irq_enabled));
#ifdef CONFIG_NFC_PN547_8916_CLK_CTL
			pn547_dev->nfc_enable = 1;
			if (IS_ERR(pn547_dev->nfc_clock)) {
				pr_err("[NFC] %s: Couldn't get D1)\n",
							__func__);
			} else {
				//clk_put(pn547_dev->nfc_clock);
				pr_info("%s power set and clk_prepare_enable\n", __func__);
				if (clk_prepare_enable(pn547_dev->nfc_clock))
					pr_err("[NFC] %s: Couldn't prepare D1\n",
							__func__);
			}
#endif
		} else if (arg == 1) {
			/* power on */
			if (pn547_dev->conf_gpio)
				pn547_dev->conf_gpio();
			gpio_set_value(pn547_dev->firm_gpio, 0);
			gpio_set_value_cansleep(pn547_dev->ven_gpio, 1);
			usleep_range(10000, 10050);
			if (atomic_read(&pn547_dev->irq_enabled) == 0) {
				atomic_set(&pn547_dev->irq_enabled, 1);
				enable_irq(pn547_dev->client->irq);
				enable_irq_wake(pn547_dev->client->irq);
			}
			pr_info("%s power on, irq=%d\n", __func__,
				atomic_read(&pn547_dev->irq_enabled));
#ifdef CONFIG_NFC_PN547_8916_CLK_CTL
			pn547_dev->nfc_enable = 1;
			if (IS_ERR(pn547_dev->nfc_clock)) {
				pr_err("[NFC] %s: Couldn't get D1)\n",
							__func__);
			} else {
				//clk_put(pn547_dev->nfc_clock);
				pr_info("%s power on and clk_prepare_enable\n", __func__);
				if (clk_prepare_enable(pn547_dev->nfc_clock))
					pr_err("[NFC] %s: Couldn't prepare D1\n",
							__func__);
			}
#endif
		} else if (arg == 0) {
			/* power off */
			if (atomic_read(&pn547_dev->irq_enabled) == 1) {
				atomic_set(&pn547_dev->irq_enabled, 0);
				disable_irq_wake(pn547_dev->client->irq);
				disable_irq_nosync(pn547_dev->client->irq);
			}
			pr_info("%s power off, irq=%d\n", __func__,
				atomic_read(&pn547_dev->irq_enabled));
			gpio_set_value(pn547_dev->firm_gpio, 0);
			gpio_set_value_cansleep(pn547_dev->ven_gpio, 0);
			usleep_range(10000, 10050);
#ifdef CONFIG_NFC_PN547_8916_CLK_CTL
			pn547_dev->nfc_enable = 0;
			if(pn547_dev->nfc_clock)
			{
				clk_disable_unprepare(pn547_dev->nfc_clock);
				pr_info("%s power off and clk_disable_unprepare\n", __func__);
			}
#endif
		} else if (arg == 3) {
			pr_info("%s Read Cancel\n", __func__);
			pn547_dev->cancel_read = true;
			atomic_set(&pn547_dev->read_flag, 1);
			wake_up(&pn547_dev->read_wq);
		} else {
			pr_err("%s bad arg %lu\n", __func__, arg);
			return -EINVAL;
		}
		break;
	default:
		pr_err("%s bad ioctl %u\n", __func__, cmd);
		return -EINVAL;
	}
#endif	/*CONFIG_NFC_PN547_ESE_SUPPORT*/
	return 0;
}

static const struct file_operations pn547_dev_fops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
	.read = pn547_dev_read,
	.write = pn547_dev_write,
	.open = pn547_dev_open,
	.unlocked_ioctl = pn547_dev_ioctl,
};

#ifdef CONFIG_NFC_PN547_LDO_CONTROL
int nfc_power_onoff(struct pn547_dev *data, bool onoff)
{
	int ret = -1;

	data->L16 = devm_regulator_get(&data->client->dev, "nfc_ldo");
	if (IS_ERR(data->L16)) {
		pr_err("[NFC] %s: regulator pointer null nfc_ldo, rc=%d\n",
			__func__, ret);
		return PTR_ERR(data->L16);
	}
	ret = regulator_set_voltage(data->L16, 1800000, 1800000);
	if (ret) {
		pr_err("[NFC] %s: set voltage failed on nfc_ldo, rc=%d\n",
			__func__, ret);
		return ret;
	}


	if (onoff) {
		ret = regulator_enable(data->L16);
		regulator_set_mode(data->L16, REGULATOR_MODE_NORMAL);
		if (ret) {
			pr_err("[NFC] %s: Failed to enable regulator nfc_ldo.\n",
				__func__);
			return ret;
		}

	} else {
		ret = regulator_disable(data->L16);
		if (ret) {
			pr_err("[NFC] %s: Failed to disable regulator nfc_ldo.\n",
				__func__);
			return ret;
		}
	}
	return 0;
}
#endif

#ifdef CONFIG_NFC_PN547_8916_CLK_CTL
#ifdef CONFIG_PM
static int pn547_nfc_suspend(struct device *dev)
{
	struct pn547_dev *info = PN547_NFC_GET_INFO(dev);

#ifdef CONFIG_NFC_PN547_8916_CLK_CTL
	if(info->nfc_enable == 1){
		if(info->nfc_clock)
		{
			clk_disable_unprepare(info->nfc_clock);
			pr_info("%s power off and clk_disable_unprepare\n", __func__);
		}
	}
#endif

	return 0;
}

static int pn547_nfc_resume(struct device *dev)
{
	struct pn547_dev *info = PN547_NFC_GET_INFO(dev);

#ifdef CONFIG_NFC_PN547_8916_CLK_CTL
		if(info->nfc_enable == 1){
			if (IS_ERR(info->nfc_clock)) {
				pr_err("[NFC] %s: Couldn't get D1)\n",
							__func__);
			} else {
				//clk_put(pn547_dev->nfc_clock);
				pr_info("%s power set and clk_prepare_enable\n", __func__);
				if (clk_prepare_enable(info->nfc_clock))
					pr_err("[NFC] %s: Couldn't prepare D1\n",
							__func__);
			}
		}
#endif

	return 0;
}

static SIMPLE_DEV_PM_OPS(pn547_nfc_pm_ops, pn547_nfc_suspend, pn547_nfc_resume);
#endif
#endif

#ifdef CONFIG_OF
static int pn547_parse_dt(struct device *dev,
	struct pn547_i2c_platform_data *pdata)
{
	struct device_node *np = dev->of_node;

	pdata->irq_gpio = of_get_named_gpio_flags(np, "pn547,irq-gpio",
		0, &pdata->irq_gpio_flags);

	pdata->ven_gpio = of_get_named_gpio_flags(np, "pn547,ven-gpio",
		0, &pdata->ven_gpio_flags);

	pdata->firm_gpio = of_get_named_gpio_flags(np, "pn547,firm-gpio",
		0, &pdata->firm_gpio_flags);
#ifdef CONFIG_NFC_PN547_PVDD_EN_CONTROL
	pdata->pvdd_en = of_get_named_gpio_flags(np, "pn547,pvdd_en",
		0, &pdata->pvdd_en_flags);
#endif
#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	pdata->ese_pwr_req = of_get_named_gpio_flags(np, "pn547,pwr_req",
		0, &pdata->ese_pwr_req_flags);
#endif
#ifdef CONFIG_NFC_PN547_8916_USE_BBCLK2
  	pdata->nfc_clock = clk_get(dev, "nfc_clock");
	if (IS_ERR(pdata->nfc_clock)) {
		pr_err("[NFC] %s: Couldn't get D1)\n",
					__func__);
	} else {
		clk_put(pdata->nfc_clock);
#ifndef CONFIG_NFC_PN547_8916_CLK_CTL
		if (clk_prepare_enable(pdata->nfc_clock))
			pr_err("[NFC] %s: Couldn't prepare D1\n",
					__func__);
#endif
	}
#endif
	if (pdata->firm_gpio < 0)
		of_property_read_u32(np, "pn547,firm-expander-gpio",
			&pdata->firm_gpio);

	pr_info("%s: irq : %u, ven : %u, firm : %u\n",
			__func__, pdata->irq_gpio, pdata->ven_gpio,
			pdata->firm_gpio);
	return 0;
}
#else
static int pn547_parse_dt(struct device *dev,
	struct pn547_i2c_platform_data *pdata)
{
	return -ENODEV;
}
#endif

static int pn547_probe(struct i2c_client *client,
		       const struct i2c_device_id *id)
{
	int ret;
	int err;
	struct pn547_i2c_platform_data *platform_data;
	struct pn547_dev *pn547_dev;
	struct pinctrl *nfc_pinctrl;
	struct pinctrl_state *nfc_suspend;
	struct pinctrl_state *nfc_active;
	int addr=0x2B, addrcnt;
	char tmp[4] = {0x20, 0x00, 0x01, 0x01};

	if (client->dev.of_node) {
		platform_data = devm_kzalloc(&client->dev,
			sizeof(struct pn547_i2c_platform_data), GFP_KERNEL);
		if (!platform_data) {
			pr_err("%s Failed to allocate memory\n", __func__);
			return -ENOMEM;
		}
		err = pn547_parse_dt(&client->dev, platform_data);
		if (err)
			return err;
	} else {
		platform_data = client->dev.platform_data;
	}

	if (platform_data == NULL) {
		pr_err("%s : nfc probe fail\n", __func__);
		return -ENODEV;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("%s : need I2C_FUNC_I2C\n", __func__);
		return -ENODEV;
	}
#ifdef CONFIG_NFC_PN547_PVDD_EN_CONTROL
	ret = gpio_request(platform_data->pvdd_en, "nfc_pvdd_en");
	if (ret) {
		pr_err("failed to request about pvdd_en pin\n");
		return -ENODEV;
	}
#endif
	ret = gpio_request(platform_data->irq_gpio, "nfc_int");
	if (ret)
		return -ENODEV;
	ret = gpio_request(platform_data->ven_gpio, "nfc_ven");
	if (ret)
		goto err_ven;
	ret = gpio_request(platform_data->firm_gpio, "nfc_firm");
	if (ret)
		goto err_firm;
#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	ret = gpio_request(platform_data->ese_pwr_req, "ese_pwr");
	if (ret)
		goto err_ese;
#endif
	/* Get pinctrl if target uses pinctrl */
	nfc_pinctrl = devm_pinctrl_get(&client->dev);
	if (IS_ERR(nfc_pinctrl)) {
		pr_info("Target does not use pinctrl\n");
		nfc_pinctrl = NULL;
	} else {
		nfc_suspend = pinctrl_lookup_state(nfc_pinctrl, "nfc_suspend");
		nfc_active = pinctrl_lookup_state(nfc_pinctrl, "nfc_active");
		if (IS_ERR(nfc_suspend)) {
			pr_info("%s fail to suspend lookup_state\n", __func__);
			goto err_exit;
		}
		if (IS_ERR(nfc_active)) {
			pr_info("%s fail to active lookup_state\n", __func__);
			goto err_exit;
		}
		ret = pinctrl_select_state(nfc_pinctrl, nfc_suspend);
		if (ret != 0) {
			pr_err("%s: fail to select_state suspend\n", __func__);
			goto err_exit;
		}
		ret = pinctrl_select_state(nfc_pinctrl, nfc_active);
		if (ret != 0) {
			pr_err("%s: fail to select_state active\n", __func__);
			goto err_exit;
		}
		devm_pinctrl_put(nfc_pinctrl);
	}

	pn547_dev = kzalloc(sizeof(*pn547_dev), GFP_KERNEL);
	if (pn547_dev == NULL) {
		dev_err(&client->dev,
			"failed to allocate memory for module data\n");
		ret = -ENOMEM;
		goto err_exit;
	}

	pr_info("%s : IRQ num %d\n", __func__, client->irq);
#ifdef CONFIG_NFC_PN547_PVDD_EN_CONTROL
	pn547_dev->pvdd_en = platform_data->pvdd_en;
#endif
	pn547_dev->irq_gpio = platform_data->irq_gpio;
	pn547_dev->ven_gpio = platform_data->ven_gpio;
	pn547_dev->firm_gpio = platform_data->firm_gpio;
	pn547_dev->conf_gpio = platform_data->conf_gpio;
#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	pn547_dev->ese_pwr_req = platform_data->ese_pwr_req;
#endif
#ifdef CONFIG_NFC_PN547_8916_USE_BBCLK2
	pn547_dev->nfc_clock = platform_data->nfc_clock;
#endif
#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	pn547_dev->p61_current_state = P61_STATE_IDLE;
	pn547_dev->nfc_ven_enabled = false;
	pn547_dev->spi_ven_enabled = false;
#endif
	pn547_dev->client = client;
#ifdef CONFIG_NFC_PN547_8916_CLK_CTL
	pn547_dev->nfc_enable = 0;
#endif
	/* init mutex and queues */
	init_waitqueue_head(&pn547_dev->read_wq);
	mutex_init(&pn547_dev->read_mutex);
#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	mutex_init(&pn547_dev->p61_state_mutex);
#endif

	pn547_dev->pn547_device.minor = MISC_DYNAMIC_MINOR;
#ifdef CONFIG_NFC_PN547
	pn547_dev->pn547_device.name = "pn547";
#else
	pn547_dev->pn547_device.name = "pn544";
#endif
	pn547_dev->pn547_device.fops = &pn547_dev_fops;

	ret = misc_register(&pn547_dev->pn547_device);
	if (ret) {
		pr_err("%s : misc_register failed\n", __FILE__);
		goto err_misc_register;
	}

	/* request irq.  the irq is set whenever the chip has data available
	 * for reading.  it is cleared when all data has been read.
	 */
	pr_info("%s : requesting IRQ %d\n", __func__, client->irq);
#ifdef CONFIG_NFC_PN547_PVDD_EN_CONTROL
	gpio_direction_output(pn547_dev->pvdd_en, 1);
#endif
	gpio_direction_input(pn547_dev->irq_gpio);
	gpio_direction_output(pn547_dev->ven_gpio, 0);
	gpio_direction_output(pn547_dev->firm_gpio, 0);
#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	gpio_direction_output(pn547_dev->ese_pwr_req, 0);
#endif

	i2c_set_clientdata(client, pn547_dev);
	wake_lock_init(&pn547_dev->nfc_wake_lock,
			WAKE_LOCK_SUSPEND, "nfc_wake_lock");

#ifdef CONFIG_NFC_PN547_LDO_CONTROL
	nfc_power_onoff(pn547_dev,1);
#endif
	ret = request_irq(client->irq, pn547_dev_irq_handler,
			  IRQF_TRIGGER_RISING, "pn547", pn547_dev);
	if (ret) {
		dev_err(&client->dev, "request_irq failed\n");
		goto err_request_irq_failed;
	}
	disable_irq_nosync(pn547_dev->client->irq);
	atomic_set(&pn547_dev->irq_enabled, 0);

	gpio_set_value(pn547_dev->firm_gpio, 1); /* add firmware pin */
	msleep(20);
	gpio_set_value(pn547_dev->ven_gpio, 1);
	usleep_range(4900, 5000);
	gpio_set_value(pn547_dev->ven_gpio, 0);
	usleep_range(4900, 5000);
	gpio_set_value(pn547_dev->ven_gpio, 1);
	usleep_range(4900, 5000);

	/*for (addr = 0x2B; addr > 0x27; addr--) */
	{
		client->addr = addr;
		addrcnt = 2;

		do {
			ret = i2c_master_send(client, tmp, 4);
			if (ret > 0) {
				pr_info("%s : i2c addr=0x%X\n",
					__func__, client->addr);
				break;
			}
		} while (addrcnt--);

		/*if (ret > 0)
			break;*/
	}

	if(ret <= 0)
		client->addr = 0x2B;

	gpio_set_value(pn547_dev->ven_gpio, 0);
	gpio_set_value(pn547_dev->firm_gpio, 0); /* add */

	if (ret < 0) {
		pr_err("%s : fail to get i2c addr\n", __func__);
		goto err_request_irq_failed;
	}
	else
		pr_info("%s : success\n", __func__);
	return 0;

err_request_irq_failed:
	misc_deregister(&pn547_dev->pn547_device);
	wake_lock_destroy(&pn547_dev->nfc_wake_lock);
err_misc_register:
	mutex_destroy(&pn547_dev->read_mutex);
#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	mutex_destroy(&pn547_dev->p61_state_mutex);
#endif
	kfree(pn547_dev);
err_exit:
#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	gpio_free(platform_data->ese_pwr_req);
err_ese:
#endif
	gpio_free(platform_data->firm_gpio);
err_firm:
	gpio_free(platform_data->ven_gpio);
err_ven:
	gpio_free(platform_data->irq_gpio);
	pr_err("[pn547] pn547_probe fail!\n");
	return ret;
}

static int pn547_remove(struct i2c_client *client)
{
	struct pn547_dev *pn547_dev;

	pn547_dev = i2c_get_clientdata(client);
#ifdef CONFIG_NFC_PN547_8916_USE_BBCLK2
	if(pn547_dev->nfc_clock)
		clk_disable_unprepare(pn547_dev->nfc_clock);
#endif
	wake_lock_destroy(&pn547_dev->nfc_wake_lock);
	free_irq(client->irq, pn547_dev);
	misc_deregister(&pn547_dev->pn547_device);
	mutex_destroy(&pn547_dev->read_mutex);
#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	mutex_destroy(&pn547_dev->p61_state_mutex);
#endif
	gpio_free(pn547_dev->irq_gpio);
	gpio_free(pn547_dev->ven_gpio);
	gpio_free(pn547_dev->firm_gpio);
#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	pn547_dev->p61_current_state = P61_STATE_INVALID;
	pn547_dev->nfc_ven_enabled = false;
	pn547_dev->spi_ven_enabled = false;
#endif
	kfree(pn547_dev);

	return 0;
}

static const struct i2c_device_id pn547_id[] = {
	{"pn547", 0},
	{}
};

#ifdef CONFIG_OF
static struct of_device_id nfc_match_table[] = {
	{ .compatible = "pn547",},
	{},
};
#else
#define nfc_match_table NULL
#endif

static struct i2c_driver pn547_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "pn547",
#ifdef CONFIG_NFC_PN547_8916_CLK_CTL
#ifdef CONFIG_PM
		.pm = &pn547_nfc_pm_ops,
#endif
#endif
		.of_match_table = nfc_match_table,
	},
	.id_table = pn547_id,
	.probe = pn547_probe,
	.remove = pn547_remove,
};

/*
 * module load/unload record keeping
 */

static int __init pn547_dev_init(void)
{
	pr_info("Loading pn547 driver\n");
#ifdef CONFIG_NFC_PN547_LDO_CONTROL
	if(poweroff_charging)
		return 0;
	else
#endif
		return i2c_add_driver(&pn547_driver);
}

module_init(pn547_dev_init);

static void __exit pn547_dev_exit(void)
{
	pr_info("Unloading pn547 driver\n");
	i2c_del_driver(&pn547_driver);
}

module_exit(pn547_dev_exit);

MODULE_AUTHOR("Sylvain Fonteneau");
MODULE_DESCRIPTION("NFC PN547 driver");
MODULE_LICENSE("GPL");
