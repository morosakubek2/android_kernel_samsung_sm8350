/**
 * The device control driver for JIIOV's fingerprint sensor.
 *
 * Copyright (C) 2020 JIIOV Corporation. <http://www.jiiov.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
**/

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_wakeup.h>
#include <linux/cdev.h>
#include <net/sock.h>
#include "jiiov_platform.h"
#include <linux/init.h>
#include <linux/types.h>
#include <linux/netlink.h>

#include "../include/oplus_fp_common.h"
#include <linux/msm_drm_notify.h>
#include <linux/notifier.h>
#include <linux/fb.h>


#define ANC_COMPATIBLE_SW_FP    "jiiov,fingerprint"
#define ANC_DEVICE_NAME         "jiiov_fp"

#define ANC_DEVICE_MAJOR        0    /* default to dynamic major */
static int anc_major_num = ANC_DEVICE_MAJOR;

#define ANC_WAKELOCK_HOLD_TIME  400  /* ms */

typedef struct platform_device anc_device_t;
typedef struct platform_driver anc_driver_t;

static int anc_gpio_pwr_flag = 0;

static const char * const pctl_names[] = {
    "anc_reset_reset",
    "anc_reset_active",
};

struct vreg_config {
	char *name;
	unsigned long vmin;
	unsigned long vmax;
	int ua_load;
};

#define ANC_VREG_LDO_NAME    "ldo"
static struct vreg_config const vreg_conf[] = {
    { ANC_VREG_LDO_NAME, 3300000UL, 3300000UL, 150000, },
};

struct anc_data {
    struct device *dev;
    struct class *dev_class;
    dev_t dev_num;
    struct cdev cdev;

    struct pinctrl *fingerprint_pinctrl;
    struct pinctrl_state *pinctrl_state[ARRAY_SIZE(pctl_names)];
    struct regulator *vreg[ARRAY_SIZE(vreg_conf)];
    struct wakeup_source *fp_wakelock;
    int pwr_gpio;
    int rst_gpio;
    struct mutex lock;

#ifdef ANC_USE_NETLINK
    struct fp_underscreen_info fp_tpinfo;
    struct notifier_block notifier;
    char fb_black;
#endif
};

static struct anc_data *g_anc_data;

#define NETLINK_ANC     30
#define USER_PORT       100


static struct sock *gp_netlink_sock = NULL;
extern struct net init_net; // by default

static inline int netlink_send_message(const char *p_buffer, uint16_t length)
{
    struct nlmsghdr *p_nlmsghdr;
    int ret;
    struct sk_buff *p_sk_buff = nlmsg_new(length, GFP_ATOMIC);
    if (!p_sk_buff) {
        pr_debug("netlink alloc failure\n");
        return -ENOMEM;
    }

    p_nlmsghdr = nlmsg_put(p_sk_buff, 0, 0, NETLINK_ANC, length, 0);
    if (!p_nlmsghdr) {
        pr_debug("nlmsg_put failure\n");
        nlmsg_free(p_sk_buff);
        return -ENOMEM;
    }

    memcpy(nlmsg_data(p_nlmsghdr), p_buffer, length);
    ret = netlink_unicast(gp_netlink_sock, p_sk_buff, USER_PORT, MSG_DONTWAIT);

    return ret;
}

static inline int netlink_send_message_to_user(const char *p_buffer, size_t length)
{
    if (p_buffer && length > 0) {
        pr_debug("send message to user: %d\n", *p_buffer);
        return netlink_send_message(p_buffer, length);
    }
    return -EINVAL;
}

static inline void netlink_receive_message(struct sk_buff *p_sk_buffer)
{
    if (p_sk_buffer->len >= nlmsg_total_size(0)) {
        struct nlmsghdr *nlh = nlmsg_hdr(p_sk_buffer);
        char *p_user_message = NLMSG_DATA(nlh);
        if (p_user_message) {
            pr_debug("received message: %s, length: %lu\n", p_user_message, strlen(p_user_message));
            netlink_send_message_to_user(p_user_message, strlen(p_user_message));
        }
    }
}

static struct netlink_kernel_cfg g_netlink_kernel_config = {
    .input = netlink_receive_message, // set recv callback
};

static inline int anc_netlink_init(void)
{
    gp_netlink_sock = netlink_kernel_create(&init_net, NETLINK_ANC, &g_netlink_kernel_config);
    if (!gp_netlink_sock) {
        pr_debug("netlink_kernel_create error!\n");
        return -ENOMEM;
    }
    pr_debug("anc_netlink_init\n");
    return 0;
}

static inline void anc_netlink_exit(void)
{
    if (gp_netlink_sock) {
        netlink_kernel_release(gp_netlink_sock);
        gp_netlink_sock = NULL;
    }
    pr_debug("anc_netlink_exit!\n");
}

static int vreg_setup(struct anc_data *data, const char *name, bool enable)
{
    struct device *dev = data->dev;
    struct regulator *vreg;
    size_t i;
    int rc = 0;

    for (i = 0; i < ARRAY_SIZE(data->vreg); i++) {
        if (!strncmp(vreg_conf[i].name, name, strlen(vreg_conf[i].name))) {
            vreg = data->vreg[i];
            if (enable) {
                if (!vreg) {
                    vreg = regulator_get(dev, name);
                    if (IS_ERR(vreg)) {
                        dev_err(dev, "Unable to get %s\n", name);
                        return PTR_ERR(vreg);
                    }
                    data->vreg[i] = vreg;
                }

                if (regulator_count_voltages(vreg) > 0) {
                    rc = regulator_set_voltage(vreg, vreg_conf[i].vmin, vreg_conf[i].vmax);
                    if (rc) {
                        dev_err(dev, "Unable to set voltage on %s: %d\n", name, rc);
                        return rc;
                    }
                }

                rc = regulator_set_load(vreg, vreg_conf[i].ua_load);
                if (rc) {
                    dev_err(dev, "Unable to set current on %s: %d\n", name, rc);
                    return rc;
                }

                rc = regulator_enable(vreg);
                if (rc) {
                    dev_err(dev, "Error enabling %s: %d\n", name, rc);
                    regulator_put(vreg);
                    data->vreg[i] = NULL;
                    return rc;
                }
            } else {
                if (vreg) {
                    if (regulator_is_enabled(vreg)) {
                        regulator_disable(vreg);
                        dev_info(dev, "Disabled %s\n", name);
                    }
                    regulator_put(vreg);
                    data->vreg[i] = NULL;
                }
            }
            return rc;
        }
    }

    dev_err(dev, "Regulator %s not found\n", name);
    return -EINVAL;
}

/*-----------------------------------netlink-------------------------------*/
#ifdef ANC_USE_NETLINK
unsigned int lasttouchmode = 0;
static inline int anc_opticalfp_tp_handler(struct fp_underscreen_info *tp_info)
{
    char netlink_msg;

    if (tp_info->touch_state == lasttouchmode) {
        return 0;
    }

    netlink_msg = (tp_info->touch_state == 1) ? ANC_NETLINK_EVENT_TOUCH_DOWN : ANC_NETLINK_EVENT_TOUCH_UP;
    __pm_wakeup_event(g_anc_data->fp_wakelock, msecs_to_jiffies(ANC_WAKELOCK_HOLD_TIME));
    
    netlink_send_message_to_user(&netlink_msg, sizeof(netlink_msg));
    lasttouchmode = tp_info->touch_state;

    return 0;
}

static inline int anc_fb_state_chg_callback(struct notifier_block *nb,
        unsigned long val, void *data)
{
    struct anc_data *anc_data = container_of(nb, struct anc_data, notifier);
    struct msm_drm_notifier *evdata = data;
    char netlink_msg = (char)ANC_NETLINK_EVENT_INVALID;
    
    if (val == MSM_DRM_ONSCREENFINGERPRINT_EVENT) {
        uint8_t op_mode = *(uint8_t *)evdata->data;

        if (op_mode == ANC_UI_READY) {
            netlink_msg = ANC_NETLINK_EVENT_UI_READY;
            netlink_send_message_to_user(&netlink_msg, sizeof(netlink_msg));
        } else if (op_mode != ANC_UI_DISAPPREAR) {
            pr_err("[anc] Unknown MSM_DRM_ONSCREENFINGERPRINT_EVENT: %d\n", op_mode);
        }
        return NOTIFY_OK;
    }

    if (val == MSM_DRM_EARLY_EVENT_BLANK && evdata && evdata->data) {
        unsigned int blank = *(unsigned int *)(evdata->data);

        if (blank == MSM_DRM_BLANK_POWERDOWN) {
            anc_data->fb_black = 1;
            netlink_msg = ANC_NETLINK_EVENT_SCR_OFF;
            netlink_send_message_to_user(&netlink_msg, sizeof(netlink_msg));
        } else if (blank == MSM_DRM_BLANK_UNBLANK) {
            anc_data->fb_black = 0;
            netlink_msg = ANC_NETLINK_EVENT_SCR_ON;
            netlink_send_message_to_user(&netlink_msg, sizeof(netlink_msg));
        } else {
            pr_err("[anc] Unknown screen state: %d\n", blank);
        }
    }

    return NOTIFY_OK;
}
static struct notifier_block anc_noti_block = {
 .notifier_call = anc_fb_state_chg_callback,
};

static const struct {
    const char *name;
    ANC_NETLINK_EVENT_TYPE event;
} netlink_event_map[] = {
    {"test", ANC_NETLINK_EVENT_TEST},
    {"irq", ANC_NETLINK_EVENT_IRQ},
    {"screen_off", ANC_NETLINK_EVENT_SCR_OFF},
    {"screen_on", ANC_NETLINK_EVENT_SCR_ON},
    {"touch_down", ANC_NETLINK_EVENT_TOUCH_DOWN},
    {"touch_up", ANC_NETLINK_EVENT_TOUCH_UP},
    {"ui_ready", ANC_NETLINK_EVENT_UI_READY},
    {"exit", ANC_NETLINK_EVENT_EXIT},
};

/**
 * sysfs node to forward netlink event
 */
static inline ssize_t forward_netlink_event_set(struct device *p_dev,
                                                struct device_attribute *p_attr, 
                                                const char *p_buffer, 
                                                size_t count)
{
    ANC_NETLINK_EVENT_TYPE netlink_msg = ANC_NETLINK_EVENT_INVALID;
    size_t i;

    pr_info("forward netlink event: %s\n", p_buffer);

    for (i = 0; i < ARRAY_SIZE(netlink_event_map); i++) {
        if (!strncmp(p_buffer, netlink_event_map[i].name, strlen(netlink_event_map[i].name))) {
            netlink_msg = netlink_event_map[i].event;
            break;
        }
    }

    if (netlink_msg == ANC_NETLINK_EVENT_INVALID) {
        pr_err("unsupported netlink event: %s\n", p_buffer);
        return -EINVAL;
    }

    return netlink_send_message_to_user((const char *)&netlink_msg, sizeof(netlink_msg));
}

// Define the device attribute with write-only permissions
static DEVICE_ATTR(netlink_event, S_IWUSR, NULL, forward_netlink_event_set);
#endif
/*-------------------------------------------------------------------------*/

/**
 * sysfs node to select the set of pins (GPIOS) defined in a pin control node of
 * the device tree
 */
static inline int select_pin_ctl(struct anc_data *data, const char *name)
{
    size_t i;
    int rc;
    struct device *dev = data->dev;

    dev_info(dev, "%s: name is %s\n", __func__, name);
    for (i = 0; i < ARRAY_SIZE(data->pinctrl_state); i++) {
        const char *n = pctl_names[i];

        if (!strncmp(n, name, strlen(n))) {
            rc = pinctrl_select_state(data->fingerprint_pinctrl, data->pinctrl_state[i]);
            if (rc)
                dev_err(dev, "cannot select %s\n", name);
            else
                dev_info(dev, "Selected %s\n", name);
            goto exit;
        }
    }

    rc = -EINVAL;
    dev_err(dev, "%s: %s not found\n", __func__, name);

exit:
    return rc;
}

static inline ssize_t pinctl_set(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    int rc;
    struct anc_data *data = dev_get_drvdata(dev);

    mutex_lock(&data->lock);
    rc = select_pin_ctl(data, buf);
    mutex_unlock(&data->lock);

    return rc ? rc : count;
}
static DEVICE_ATTR(pinctl_set, S_IWUSR, NULL, pinctl_set);

static inline int anc_reset(struct anc_data *data)
{
    int rc;
    pr_err("anc reset\n");
    mutex_lock(&data->lock);
    rc = select_pin_ctl(data, "anc_reset_reset");
    //T2 >= 10ms
    mdelay(10);
    rc |= select_pin_ctl(data, "anc_reset_active");
    mdelay(10);
    mutex_unlock(&data->lock);

    return rc;
}

static inline ssize_t hw_reset_set(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    int rc;
    struct anc_data *data = dev_get_drvdata(dev);

    if (!strncmp(buf, "reset", strlen("reset"))) {
        pr_info("hw_reset\n");
        rc = anc_reset(data);
    } else {
        rc = -EINVAL;
    }

    return rc ? rc : count;
}
static DEVICE_ATTR(hw_reset, S_IWUSR, NULL, hw_reset_set);

static inline void anc_power_onoff(struct anc_data *data, int power_onoff)
{
    pr_info("%s: power_onoff = %d \n", __func__, power_onoff);
    if (anc_gpio_pwr_flag == 1) {
        gpio_set_value(data->pwr_gpio, power_onoff);
    } else {
        vreg_setup(data, ANC_VREG_LDO_NAME, power_onoff);
    }
}

static inline void device_power_up(struct anc_data *data)
{
    pr_info("device power up\n");
    anc_power_onoff(data, 1);
}

/**
 * sysfs node to power on/power off the sensor
 */
static inline ssize_t device_power_set(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    ssize_t rc = count;
    struct anc_data *data = dev_get_drvdata(dev);

    mutex_lock(&data->lock);
    if (!strncmp(buf, "on", strlen("on"))) {
        pr_info("device power on\n");
        anc_power_onoff(data, 1);
    } else if (!strncmp(buf, "off", strlen("off"))) {
        pr_info("device power off\n");
        anc_power_onoff(data, 0);
    } else {
        rc = -EINVAL;
    }
    mutex_unlock(&data->lock);

    return rc;
}
static DEVICE_ATTR(device_power, S_IWUSR, NULL, device_power_set);

static struct attribute *attributes[] = {
    &dev_attr_pinctl_set.attr,
    &dev_attr_device_power.attr,
    &dev_attr_hw_reset.attr,
#ifdef ANC_USE_NETLINK
    &dev_attr_netlink_event.attr,
#endif
    NULL
};

static const struct attribute_group attribute_group = {
    .attrs = attributes,
};

static inline int anc_request_named_gpio(struct anc_data *data, const char *label, int *gpio)
{
    struct device *dev = data->dev;
    struct device_node *np = dev->of_node;
    int rc = of_get_named_gpio(np, label, 0);

    if (rc < 0) {
        dev_err(dev, "Failed to get '%s'\n", label);
        return rc;
    }
    *gpio = rc;

    rc = devm_gpio_request(dev, *gpio, label);
    if (rc) {
        dev_err(dev, "Failed to request gpio %d\n", *gpio);
        return rc;
    }
    dev_info(dev, "%s %d\n", label, *gpio);

    return 0;
}

static inline int anc_gpio_init(struct device *dev, struct anc_data *data)
{
    int rc;
    size_t i;
    struct device_node *np = dev->of_node;

    if (!np) {
        dev_err(dev, "No OF node found\n");
        return -EINVAL;
    }

    if (of_property_read_bool(np, "anc,enable-via-gpio")) {
        dev_info(dev, "Using GPIO power\n");
        anc_gpio_pwr_flag = 1;
    }

    if ((rc = anc_request_named_gpio(data, "anc,gpio_rst", &data->rst_gpio)))
        return rc;

    if (anc_gpio_pwr_flag) {
        if ((rc = anc_request_named_gpio(data, "anc,gpio_pwr", &data->pwr_gpio)))
            return rc;

        if ((rc = gpio_direction_output(data->pwr_gpio, 0)))
            return rc;
    }

    data->fingerprint_pinctrl = devm_pinctrl_get(dev);
    if (IS_ERR(data->fingerprint_pinctrl)) {
        rc = PTR_ERR(data->fingerprint_pinctrl);
        if (rc == -EPROBE_DEFER) {
            dev_info(dev, "Pinctrl not ready\n");
        } else {
            dev_err(dev, "Target does not use pinctrl\n");
            data->fingerprint_pinctrl = NULL;
            rc = -EINVAL;
        }
        return rc;
    }

    for (i = 0; i < ARRAY_SIZE(data->pinctrl_state); i++) {
        const char *n = pctl_names[i];
        struct pinctrl_state *state = pinctrl_lookup_state(data->fingerprint_pinctrl, n);
        if (IS_ERR(state)) {
            dev_err(dev, "Cannot find '%s'\n", n);
            return -EINVAL;
        }
        dev_info(dev, "Found pin control %s\n", n);
        data->pinctrl_state[i] = state;
    }

    return 0;
}

static inline int anc_open(struct inode *inode, struct file *filp)
{
    struct anc_data *dev_data;
    dev_data = container_of(inode->i_cdev, struct anc_data, cdev);
    filp->private_data = dev_data;
    return 0;
}

static inline long anc_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int rc = 0;
    struct anc_data *dev_data = filp->private_data;

    if (_IOC_TYPE(cmd) != ANC_IOC_MAGIC)
        return -ENOTTY;

    pr_info("%s: cmd = %d\n", __func__, _IOC_NR(cmd));

    switch (cmd) {
    case ANC_IOC_RESET:
        pr_info("%s: reset\n", __func__);
        rc = anc_reset(dev_data);
        break;
    case ANC_IOC_ENABLE_POWER:
        pr_info("%s: enable power\n", __func__);
        anc_power_onoff(dev_data, 1);
        break;
    case ANC_IOC_DISABLE_POWER:
        pr_info("%s: disable power\n", __func__);
        anc_power_onoff(dev_data, 0);
        break;
    case ANC_IOC_CLEAR_FLAG:
#ifdef ANC_USE_NETLINK
        lasttouchmode = 0;
        pr_info("%s: clear tp flag\n", __func__);
#endif
        break;
    default:
        rc = -EINVAL;
        break;
    }
    return rc;
}

#ifdef CONFIG_COMPAT
static inline long anc_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    return anc_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations anc_fops = {
    .owner = THIS_MODULE,
    .open = anc_open,
    .unlocked_ioctl = anc_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = anc_compat_ioctl,
#endif
};

static inline int anc_probe(anc_device_t *pdev)
{
    struct device *dev = &pdev->dev;
    struct anc_data *dev_data;
    int rc = 0;

    dev_info(dev, "Anc Probe\n");

    /* Allocate device data */
    dev_data = devm_kzalloc(dev, sizeof(*dev_data), GFP_KERNEL);
    if (!dev_data) {
        dev_err(dev, "%s: Failed to allocate memory for device data\n", __func__);
        return -ENOMEM;
    }

    dev_data->dev = dev;
    g_anc_data = dev_data;
    platform_set_drvdata(pdev, dev_data);

    // Create device class
    dev_data->dev_class = class_create(THIS_MODULE, ANC_DEVICE_NAME);
    if (IS_ERR(dev_data->dev_class)) {
        dev_err(dev, "%s: Failed to create class\n", __func__);
        rc = PTR_ERR(dev_data->dev_class);
        goto free_dev_data;
    }

    // Allocate or register char device region
    if (anc_major_num) {
        dev_data->dev_num = MKDEV(anc_major_num, 0);
        rc = register_chrdev_region(dev_data->dev_num, 1, ANC_DEVICE_NAME);
    } else {
        rc = alloc_chrdev_region(&dev_data->dev_num, 0, 1, ANC_DEVICE_NAME);
        if (rc < 0) {
            dev_err(dev, "%s: Failed to allocate char device region\n", __func__);
            goto destroy_class;
        }
        anc_major_num = MAJOR(dev_data->dev_num);
        dev_info(dev, "%s: Major number of device = %d\n", __func__, anc_major_num);
    }

    // Create device
    if (IS_ERR(device_create(dev_data->dev_class, NULL, dev_data->dev_num, dev_data, ANC_DEVICE_NAME))) {
        dev_err(dev, "%s: Failed to create char device\n", __func__);
        rc = PTR_ERR(device_create);
        goto unregister_region;
    }

    // Initialize character device
    cdev_init(&dev_data->cdev, &anc_fops);
    dev_data->cdev.owner = THIS_MODULE;
    rc = cdev_add(&dev_data->cdev, dev_data->dev_num, 1);
    if (rc) {
        dev_err(dev, "%s: Failed to add char device\n", __func__);
        goto destroy_device;
    }

    mutex_init(&dev_data->lock);

    // Initialize GPIOs
    rc = anc_gpio_init(dev, dev_data);
    if (rc) {
        dev_err(dev, "%s: Failed to init GPIO\n", __func__);
        goto del_cdev;
    }

    // Power up the device
    dev_info(dev, "%s: Enabling hardware\n", __func__);
    device_power_up(dev_data);

    g_anc_data->fp_wakelock = wakeup_source_register(NULL, "anc_fp_wakelock");

#ifdef ANC_USE_NETLINK
    // Register framebuffer notifier callback
    dev_data->notifier = anc_noti_block;
    rc = msm_drm_register_client(&dev_data->notifier);
    if (rc < 0) {
        dev_err(dev, "%s: Failed to register fb notifier client\n", __func__);
        goto unregister_wakelock;
    }
#endif

    // Create sysfs group
    rc = sysfs_create_group(&dev->kobj, &attribute_group);
    if (rc) {
        dev_err(dev, "%s: Failed to create sysfs group\n", __func__);
        goto unregister_notifier;
    }

    dev_info(dev, "%s: Probe Success\n", __func__);
    return 0;

unregister_notifier:
#ifdef ANC_USE_NETLINK
    msm_drm_unregister_client(&dev_data->notifier);
unregister_wakelock:
#endif
    wakeup_source_unregister(g_anc_data->fp_wakelock);
del_cdev:
    cdev_del(&dev_data->cdev);
destroy_device:
    device_destroy(dev_data->dev_class, dev_data->dev_num);
unregister_region:
    unregister_chrdev_region(dev_data->dev_num, 1);
destroy_class:
    class_destroy(dev_data->dev_class);
free_dev_data:
    devm_kfree(dev, dev_data);
    return rc;
}

static inline int anc_remove(anc_device_t *pdev)
{
    struct anc_data *data = platform_get_drvdata(pdev);

    sysfs_remove_group(&pdev->dev.kobj, &attribute_group);
    mutex_destroy(&data->lock);
    wakeup_source_unregister(g_anc_data->fp_wakelock);
#ifdef ANC_USE_NETLINK
    msm_drm_unregister_client(&data->notifier);
#endif
    cdev_del(&data->cdev);
    device_destroy(data->dev_class, data->dev_num);
    unregister_chrdev_region(data->dev_num, 1);
    class_destroy(data->dev_class);
    return 0;
}

static struct of_device_id anc_of_match[] = {
    { .compatible = ANC_COMPATIBLE_SW_FP, },
    {}
};
MODULE_DEVICE_TABLE(of, anc_of_match);

static anc_driver_t anc_driver = {
    .driver = {
        .name  = ANC_DEVICE_NAME,
        .owner = THIS_MODULE,
        .of_match_table = anc_of_match,
    },
    .probe  = anc_probe,
    .remove = anc_remove,
};

static int __init ancfp_init(void)
{
    int rc;

    if ((FP_JIIOV_0302 != get_fpsensor_type()) && (FP_JIIOV_0301 != get_fpsensor_type())) {
        pr_err("%s, found not jiiov sensor\n", __func__);
        rc = -EINVAL;
        return rc;
    }

    rc = platform_driver_register(&anc_driver);

    if (!rc) {
        pr_info("%s OK\n", __func__);
    } else {
        pr_err("%s %d\n", __func__, rc);
    }

#ifdef ANC_USE_NETLINK
    anc_netlink_init();
    /*Register for receiving tp touch event.
     * Must register after get_fpsensor_type filtration as only one handler can be registered.
    */
    opticalfp_irq_handler_register(anc_opticalfp_tp_handler);
    pr_info("register tp event handler");
#endif

    return rc;
}

static void __exit ancfp_exit(void)
{
    pr_info("%s\n", __func__);
#ifdef ANC_USE_NETLINK
    anc_netlink_exit();
#endif
    platform_driver_unregister(&anc_driver);
}

late_initcall(ancfp_init);
module_exit(ancfp_exit);

MODULE_SOFTDEP("pre:oplus_fp_common");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("JIIOV");
MODULE_DESCRIPTION("JIIOV fingerprint sensor device driver");

