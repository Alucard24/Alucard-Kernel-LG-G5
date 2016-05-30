#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/uaccess.h>        /* copy_to_user */
#include <linux/compat.h>


#include "broadcast_tdmb_typedef.h"
#include "broadcast_tdmb_drv_ifdef.h"

//#include <soc/qcom/lge/board_lge.h>

#define BROADCAST_TDMB_NUM_DEVS     1 /**< support this many devices */

static struct class *broadcast_tdmb_class;
static dev_t broadcast_tdmb_dev;
static Device_drv *device_drv = NULL;
struct broadcast_tdmb_chdevice
{
    struct cdev cdev;
    struct device *dev;
    wait_queue_head_t wq_read;
    void *cookie;
};

static struct broadcast_tdmb_chdevice tdmb_dev;

static int broadcast_tdmb_power_on(void)
{
    int rc = ERROR;
    rc = device_drv->broadcast_drv_if_power_on();
    device_drv->broadcast_drv_if_user_stop( 0 );


    return rc;
}

static int broadcast_tdmb_power_off(void)
{
    int rc = ERROR;
    rc = device_drv->broadcast_drv_if_power_off();
    device_drv->broadcast_drv_if_user_stop( 0 );
    return rc;
}

static int broadcast_tdmb_open(void)
{
    int rc = ERROR;
    rc = device_drv->broadcast_drv_if_init();
    return rc;
}

static int broadcast_tdmb_close(void)
{
    int rc = ERROR;
    device_drv->broadcast_drv_if_user_stop(0);
    rc = device_drv->broadcast_drv_if_stop();
    return rc;
}

static int broadcast_tdmb_set_channel(void __user *arg)
{
    int rc = ERROR;
    struct broadcast_tdmb_set_ch_info udata;

    if(copy_from_user(&udata, arg, sizeof(struct broadcast_tdmb_set_ch_info)))
    {
        printk("broadcast_tdmb_set_ch fail!!! \n");
        rc = ERROR;
    }
    else
    {
        device_drv->broadcast_drv_if_user_stop( 0 );
        printk("broadcast_tdmb_set_ch ch_num = %d, mode = %d, sub_ch_id = %d \n", udata.ch_num, udata.mode, udata.sub_ch_id);
        rc = device_drv->broadcast_drv_if_set_channel(udata.ch_num, udata.sub_ch_id, udata.mode);
    }

    return rc;

}

static int broadcast_tdmb_resync(void __user *arg)
{
    return 0;
}

static int broadcast_tdmb_detect_sync(void __user *arg)
{
    int rc = ERROR;
    int udata;
    int __user* puser = (int __user*)arg;
    udata = *puser;
    rc = device_drv->broadcast_drv_if_detect_sync(udata);

    return rc;
}

static int broadcast_tdmb_get_sig_info(void __user *arg)
{
    int rc = ERROR;
    struct broadcast_tdmb_sig_info udata;

    if((void *)arg == NULL) return rc;
    rc = device_drv->broadcast_drv_if_get_sig_info(&udata);

    if(copy_to_user((void *)arg, &udata, sizeof(struct broadcast_tdmb_sig_info)))
    {
        printk("broadcast_tdmb_get_sig_info copy_to_user error!!! \n");
        rc = ERROR;
    }
    else
    {
        rc = OK;
    }

    return rc;
}

static int broadcast_tdmb_get_ch_info(void __user *arg)
{
    int rc = ERROR;
    char fic_kernel_buffer[400];
    unsigned int fic_len = 0;

    struct broadcast_tdmb_ch_info __user* puserdata = (struct broadcast_tdmb_ch_info __user*)arg;

    if((puserdata == NULL)||( puserdata->ch_buf_addr == 0))
    {
        printk("broadcast_tdmb_get_ch_info argument error\n");
        return rc;
    }

    memset(fic_kernel_buffer, 0x00, sizeof(fic_kernel_buffer));
    rc = device_drv->broadcast_drv_if_get_fic(fic_kernel_buffer, &fic_len);

    if(rc == OK)
    {
        if(copy_to_user((void __user*)((unsigned long)puserdata->ch_buf_addr), (void*)fic_kernel_buffer, fic_len))
        {
            fic_len = 0;
            rc = ERROR;
        }
    }
    else
    {
        fic_len = 0;
        rc = ERROR;
    }

    puserdata->buf_len = fic_len;

    return rc;
}

static int broadcast_tdmb_get_dmb_data(void __user *arg)
{
    int                rc                    = ERROR;
    char*            read_buffer_ptr        = NULL;
    unsigned int     read_buffer_size     = 0;
    unsigned int     read_packet_cnt     = 0;
    unsigned int     copied_buffer_size     = 0;

    struct broadcast_tdmb_data_info __user* puserdata = (struct broadcast_tdmb_data_info  __user*)arg;

    while(1)
    {
        rc = device_drv->broadcast_drv_if_get_msc(&read_buffer_ptr, &read_buffer_size, puserdata->data_buf_size - copied_buffer_size);
        if((rc != OK) ||(read_buffer_size ==0))
        {
            break;
        }

        if ( puserdata->data_buf_size < copied_buffer_size + read_buffer_size )
        {
            printk("broadcast_tdmb_get_dmb_data, output buffer is small\n");
            break;
        }
        if(copy_to_user((void __user*)(((unsigned long)puserdata->data_buf_addr) + copied_buffer_size), (void*)read_buffer_ptr, read_buffer_size))
        {
            puserdata->copied_size= 0;
            puserdata->packet_cnt = 0;
            rc = ERROR;
            printk("broadcast_tdmb_get_dmb_data copy_to_user error\n");
            break;
        }
        else
        {
            copied_buffer_size += read_buffer_size;
            puserdata->copied_size = copied_buffer_size;

            read_packet_cnt = copied_buffer_size/188;
            puserdata->packet_cnt = read_packet_cnt;
            rc = OK;
        }
    }

    if(puserdata->copied_size)
    {
        rc = OK;
    }

    return rc;
}

static int8 broadcast_tdmb_reset_ch(void)
{
    int rc = ERROR;
    rc = device_drv->broadcast_drv_if_reset_ch();

    return rc;
}

static int8 broadcast_tdmb_user_stop(void __user *arg)
{
    int udata;
    int __user* puser = (int __user*)arg;

    udata = *puser;

    //printk("broadcast_tdmb_user_stop data =(%d)\n", udata);
    device_drv->broadcast_drv_if_user_stop( udata );

    return OK;
}

static int8 broadcast_tdmb_select_antenna(void __user *arg)
{
    int rc = ERROR;
    int udata;
    int __user* puser = (int __user*)arg;

    udata = *puser;
    rc = device_drv->broadcast_drv_if_select_antenna(udata);

    return rc;
}

static int8 broadcast_tdmb_set_nation(void __user *arg)
{
    int rc = ERROR;
    int udata;
    int __user* puser = (int __user*)arg;

    udata = *puser;
    rc = device_drv->broadcast_drv_if_set_nation(udata);

    return rc;
}

static Dynamic_32_64 broadcast_tdmb_open_control(struct inode *inode, struct file *file)
{
    struct broadcast_tdmb_chdevice *the_dev =
           container_of(inode->i_cdev, struct broadcast_tdmb_chdevice, cdev);

    printk("broadcast_tdmb_open_control start \n");

    file->private_data = the_dev;

    printk("broadcast_tdmb_open_control OK \n");

    return nonseekable_open(inode, file);
}

static long broadcast_tdmb_ioctl_control(struct file *filep, unsigned int cmd,    unsigned long arg)
{
    int rc = -EINVAL;
    void __user *argp = (void __user *)arg;

    switch (cmd)
    {
    case LGE_BROADCAST_TDMB_IOCTL_ON:
        rc = broadcast_tdmb_power_on();
        //printk("LGE_BROADCAST_TDMB_IOCTL_ON OK %d \n", rc);
        break;
    case LGE_BROADCAST_TDMB_IOCTL_OFF:
        rc = broadcast_tdmb_power_off();
        //printk("LGE_BROADCAST_TDMB_IOCTL_OFF OK %d \n", rc);
        break;
    case LGE_BROADCAST_TDMB_IOCTL_OPEN:
        rc = broadcast_tdmb_open();
        //printk("LGE_BROADCAST_TDMB_IOCTL_OPEN OK %d \n", rc);
        break;
    case LGE_BROADCAST_TDMB_IOCTL_CLOSE:
        broadcast_tdmb_close();
        //printk("LGE_BROADCAST_TDMB_IOCTL_CLOSE OK \n");
        rc = 0;
        break;
    case LGE_BROADCAST_TDMB_IOCTL_SET_CH:
        rc = broadcast_tdmb_set_channel(argp);
        //printk("LGE_BROADCAST_TDMB_IOCTL_SET_CH result = %d \n", rc);
        break;
    case LGE_BROADCAST_TDMB_IOCTL_RESYNC:
        rc = broadcast_tdmb_resync(argp);
        //printk("LGE_BROADCAST_TDMB_IOCTL_RESYNC result = %d \n", rc);
        break;
    case LGE_BROADCAST_TDMB_IOCTL_DETECT_SYNC:
        rc = broadcast_tdmb_detect_sync(argp);
        //printk("LGE_BROADCAST_TDMB_IOCTL_DETECT_SYNC result = %d \n", rc);
        break;
    case LGE_BROADCAST_TDMB_IOCTL_GET_SIG_INFO:
        rc = broadcast_tdmb_get_sig_info(argp);
        //printk("LGE_BROADCAST_TDMB_IOCTL_GET_SIG_INFO result = %d \n", rc);
        break;
    case LGE_BROADCAST_TDMB_IOCTL_GET_CH_INFO:
        rc = broadcast_tdmb_get_ch_info(argp);
        //printk("LGE_BROADCAST_TDMB_IOCTL_GET_CH_INFO result = %d \n", rc);
        break;

    case LGE_BROADCAST_TDMB_IOCTL_RESET_CH:
        //printk("LGE_BROADCAST_TDMB_IOCTL_RESET_CH result = %d \n", rc);
        rc = broadcast_tdmb_reset_ch();
        break;

    case LGE_BROADCAST_TDMB_IOCTL_USER_STOP:
        //printk("LGE_BROADCAST_TDMB_IOCTL_USER_STOP !!! \n");
        rc = broadcast_tdmb_user_stop(argp);
        break;

    case LGE_BROADCAST_TDMB_IOCTL_GET_DMB_DATA:
        rc = broadcast_tdmb_get_dmb_data(argp);
        //printk("LGE_BROADCAST_TDMB_IOCTL_GET_DMB_DATA TBD... !!! \n");
        break;

    case LGE_BROADCAST_TDMB_IOCTL_SELECT_ANTENNA:
        rc = broadcast_tdmb_select_antenna(argp);
        break;

    case LGE_BROADCAST_TDMB_IOCTL_SET_NATION:
        rc = broadcast_tdmb_set_nation(argp);
        break;

    default:
        printk("broadcast_tdmb_ioctl_control OK \n");
        rc = -EINVAL;
        break;
    }

    return rc;
}
#ifdef CONFIG_COMPAT
static long broadcast_tdmb_compat_ioctl_control(struct file *flip, unsigned int cmd, unsigned long arg)
{
    return broadcast_tdmb_ioctl_control(flip, cmd, (unsigned long)compat_ptr(arg));
}
#else
#define broadcast_tdmb_compat_ioctl_contrl NULL
#endif

static Dynamic_32_64 broadcast_tdmb_release_control(struct inode *inode, struct file *file)
{
    return 0;
}

static const struct file_operations broadcast_tdmb_fops_control =
{
    .owner = THIS_MODULE,
    .open = broadcast_tdmb_open_control,
    .unlocked_ioctl = broadcast_tdmb_ioctl_control,
#ifdef CONFIG_COMPAT
    .compat_ioctl = broadcast_tdmb_compat_ioctl_control,
#endif
    .release = broadcast_tdmb_release_control,
};

static int broadcast_tdmb_device_init(struct broadcast_tdmb_chdevice *pbroadcast, int index)
{
    int rc;

    cdev_init(&pbroadcast->cdev, &broadcast_tdmb_fops_control);

    pbroadcast->cdev.owner = THIS_MODULE;

    rc = cdev_add(&pbroadcast->cdev, broadcast_tdmb_dev, 1);

    pbroadcast->dev = device_create(broadcast_tdmb_class, NULL, MKDEV(MAJOR(broadcast_tdmb_dev), 0),
                     NULL, "broadcast%d", index);

    printk("broadcast_tdmb_device_add add add%d broadcast_tdmb_dev = %d \n", rc, MKDEV(MAJOR(broadcast_tdmb_dev), 0));

    if (IS_ERR(pbroadcast->dev)) {
        rc = PTR_ERR(pbroadcast->dev);
        pr_err("device_create failed: %d\n", rc);
        rc = -1;
    }

    printk("broadcast_tdmb_device_init start %d\n", rc);

    return rc;
}

int broadcast_tdmb_is_on(void)
{
    int rc = FALSE;

    rc = device_drv->broadcast_drv_if_is_on();

    return rc;
}
EXPORT_SYMBOL(broadcast_tdmb_is_on);

int8 broadcast_tdmb_blt_power_on(void)
{
    int rc = ERROR;
    rc = device_drv->broadcast_drv_if_power_on();

    device_drv->broadcast_drv_if_user_stop( 0 );

    return rc;

}
EXPORT_SYMBOL(broadcast_tdmb_blt_power_on);

int8 broadcast_tdmb_blt_power_off(void)
{
    int rc = ERROR;

    rc = device_drv->broadcast_drv_if_power_off();

    device_drv->broadcast_drv_if_user_stop( 0 ); /* 0 or 1 */

    return rc;

}
EXPORT_SYMBOL(broadcast_tdmb_blt_power_off);

int8 broadcast_tdmb_blt_open(void)
{
    int rc = ERROR;

    rc = device_drv->broadcast_drv_if_init();

    return rc;
}
EXPORT_SYMBOL(broadcast_tdmb_blt_open);

int8 broadcast_tdmb_blt_close(void)
{
    int rc = ERROR;

    rc = device_drv->broadcast_drv_if_stop();

    return rc;
}
EXPORT_SYMBOL(broadcast_tdmb_blt_close);

int8 broadcast_tdmb_blt_tune_set_ch(int32 freq_num)
{
    int8 rc = ERROR;
    int32 freq_number = freq_num;
    uint8 sub_ch_id = 0;
    uint8 op_mode = 2;

    rc = device_drv->broadcast_drv_if_set_channel(freq_number, sub_ch_id, op_mode);

    return rc;
}
EXPORT_SYMBOL(broadcast_tdmb_blt_tune_set_ch);

int8 broadcast_tdmb_blt_get_sig_info(void* sig_info)
{
    int rc = ERROR;
    struct broadcast_tdmb_sig_info udata;

    if(sig_info == NULL)
    {
        return rc;
    }
    memset((void*)&udata, 0x00, sizeof(struct broadcast_tdmb_sig_info));

    rc = device_drv->broadcast_drv_if_get_sig_info(&udata);

    memcpy(sig_info, (void*)&udata, sizeof(struct broadcast_tdmb_sig_info));

    return rc;
}

int broadcast_tdmb_drv_check_module_init(void)
{
    int rc = ERROR;

    if(!device_drv) {
        printk("broadcast_tdmb_drv_check_module_init device driver is not registered yet\n");
        rc = OK;
    } else {
        printk("broadcast_tdmb_drv_check_module_init device driver already is registered\n");
        rc = ERROR;
    }
    return rc;
}
EXPORT_SYMBOL(broadcast_tdmb_drv_check_module_init);

int broadcast_tdmb_drv_start(Device_drv* dev)
{
    struct broadcast_tdmb_chdevice *pbroadcast = NULL;
    int rc = -ENODEV;

    if(dev != NULL)
        device_drv = dev;
    else {
        printk("broadcast_tdmb_drv_start device drv ptr is NULL\n");
        return rc;
    }

    if (!broadcast_tdmb_class) {

        broadcast_tdmb_class = class_create(THIS_MODULE, "broadcast_tdmb");
        if (IS_ERR(broadcast_tdmb_class)) {
            rc = PTR_ERR(broadcast_tdmb_class);
            pr_err("broadcast_tdmb_class: create device class failed: %d\n",
                rc);
            return rc;
        }

        rc = alloc_chrdev_region(&broadcast_tdmb_dev, 0, BROADCAST_TDMB_NUM_DEVS, "broadcast_tdmb");
        printk("broadcast_tdmb_drv_start add add%d broadcast_tdmb_dev = %d \n", rc, broadcast_tdmb_dev);
        if (rc < 0) {
            pr_err("broadcast_class: failed to allocate chrdev: %d\n",
                rc);
            return rc;
        }
    }

    pbroadcast = &tdmb_dev;

    rc = broadcast_tdmb_device_init(pbroadcast, 0);
    if (rc < 0) {
        return rc;
    }

    printk("broadcast_tdmb_drv_start start %d\n", rc);

    return rc;
}

EXPORT_SYMBOL(broadcast_tdmb_drv_start);

int broadcast_tdmb_get_stop_mode(void)
{
    return 0;
}

EXPORT_SYMBOL(broadcast_tdmb_get_stop_mode);
