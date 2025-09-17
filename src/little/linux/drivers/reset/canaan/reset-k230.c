/*
 * Copyright (c) 2016-2017 Linaro Ltd.
 * Copyright (c) 2022, Canaan Bright Sight Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <dt-bindings/reset/canaan-k230-reset.h>

// #define K230_RESET_DEBUG

struct k230_reset_controller {
    spinlock_t                  lock;
    void __iomem                *membase;
    struct reset_controller_dev rst;
};

typedef enum
{
    W1C = 1<<0,
    W1T = 1<<1,
    WE = 1<<2, //write enable
    RWSC = 1<<3,
    RW_0 = 1<<4,
    RW_1 = 1<<5,
} reset_type_t;

typedef struct {
    uint32_t reset;
    uint32_t type;
    uint8_t reg_offset;
    uint8_t reset_bit;
    uint8_t done_bit;
    uint32_t mask_rw;
} reset_t;

#define to_k230_reset_controller(_rst) \
    container_of(_rst, struct k230_reset_controller, rst)

reset_t k_reset[] = {
    {SYSCTL_RESET_CPU0_CORE, WE|W1T|W1C, 0x4, 0, 12, 0x0},
    {SYSCTL_RESET_CPU0_FLUSH, WE|RWSC, 0x4, 4, 4, 0x0},
    {SYSCTL_RESET_CPU1_CORE, WE|RW_1|W1C, 0xc, 0, 12, 0x1},
    {SYSCTL_RESET_CPU1_FLUSH, WE|RWSC, 0xc, 4, 4, 0x1},
    {SYSCTL_RESET_AI, W1T|W1C, 0x14, 0, 31, 0x0},
    {SYSCTL_RESET_VPU, W1T|W1C, 0x1c, 0, 31, 0x0},
    {SYSCTL_RESET_HS, W1T|W1C, 0x2c, 0, 4, 0x0},
    {SYSCTL_RESET_HS_AHB, W1T|W1C, 0x2c, 1, 5, 0x0},
    {SYSCTL_RESET_SDIO0, W1T|W1C, 0x34, 0, 28, 0x0},
    {SYSCTL_RESET_SDIO1, W1T|W1C, 0x34, 1, 29, 0x0},
    {SYSCTL_RESET_SDIO_AXI, W1T|W1C, 0x34, 2, 30, 0x0},
    {SYSCTL_RESET_USB0, W1T|W1C, 0x3c, 0, 28, 0x0},
    {SYSCTL_RESET_USB1, W1T|W1C, 0x3c, 1, 29, 0x0},
    {SYSCTL_RESET_USB0_AHB, W1T|W1C, 0x3c, 0, 30, 0x0},
    {SYSCTL_RESET_USB1_AHB, W1T|W1C, 0x3c, 1, 31, 0x0},
    {SYSCTL_RESET_SPI0, W1T|W1C, 0x44, 0, 28, 0x0},
    {SYSCTL_RESET_SPI1, W1T|W1C, 0x44, 1, 29, 0x0},
    {SYSCTL_RESET_SPI2, W1T|W1C, 0x44, 2, 30, 0x0},
    {SYSCTL_RESET_SEC, W1T|W1C, 0x4c, 0, 31, 0x0},
    {SYSCTL_RESET_PDMA, W1T|W1C, 0x54, 0, 28, 0x0},
    {SYSCTL_RESET_SDMA, W1T|W1C, 0x54, 1, 29, 0x0},
    {SYSCTL_RESET_DECOMPRESS, W1T|W1C, 0x5c, 0, 31, 0x0},
    {SYSCTL_RESET_SRAM, W1T|W1C, 0x64, 0, 28, 0x2},
    {SYSCTL_RESET_SHRM_AXIM, W1T|W1C, 0x64, 2, 30, 0x2},
    {SYSCTL_RESET_SHRM_AXIS, W1T|W1C, 0x64, 3, 31, 0x2},
    {SYSCTL_RESET_SHRM_APB, RW_0, 0x64, 1, 0, 0x2},
    {SYSCTL_RESET_NONAI2D, W1T|W1C, 0x6c, 0, 31, 0x0},
    {SYSCTL_RESET_MCTL, W1T|W1C, 0x74, 0, 31, 0x0},
    {SYSCTL_RESET_ISP, W1T|W1C, 0x80, 6, 29, 0x39f},
    {SYSCTL_RESET_ISP_DW, W1T|W1C, 0x80, 5, 28, 0x39f},
    {SYSCTL_RESET_CSI0_APB, RW_0, 0x80, 0, 0, 0x39f},
    {SYSCTL_RESET_CSI1_APB, RW_0, 0x80, 1, 0, 0x39f},
    {SYSCTL_RESET_CSI2_APB, RW_0, 0x80, 2, 0, 0x39f},
    {SYSCTL_RESET_CSI_DPHY_APB, RW_0, 0x80, 3, 0, 0x39f},
    {SYSCTL_RESET_ISP_AHB, RW_0, 0x80, 4, 0, 0x39f},
    {SYSCTL_RESET_M0, RW_0, 0x80, 7, 0, 0x39f},
    {SYSCTL_RESET_M1, RW_0, 0x80, 8, 0, 0x39f},
    {SYSCTL_RESET_M2, RW_0, 0x80, 9, 0, 0x39f},
    {SYSCTL_RESET_DPU, W1T|W1C, 0x88, 0, 31, 0x0},
    {SYSCTL_RESET_DISP, W1T|W1C, 0x90, 0, 31, 0x0},
    {SYSCTL_RESET_GPU, W1T|W1C, 0x98, 0, 31, 0x0},
    {SYSCTL_RESET_AUDIO, W1T|W1C, 0xa4, 0, 31, 0x0},
    {SYSCTL_RESET_TIMER0, RW_0, 0x20, 0, 0, 0xff0ff},
    {SYSCTL_RESET_TIMER1, RW_0, 0x20, 1, 0, 0xff0ff},
    {SYSCTL_RESET_TIMER2, RW_0, 0x20, 2, 0, 0xff0ff},
    {SYSCTL_RESET_TIMER3, RW_0, 0x20, 3, 0, 0xff0ff},
    {SYSCTL_RESET_TIMER4, RW_0, 0x20, 4, 0, 0xff0ff},
    {SYSCTL_RESET_TIMER5, RW_0, 0x20, 5, 0, 0xff0ff},
    {SYSCTL_RESET_TIMER_APB, RW_0, 0x20, 6, 0, 0xff0ff},
    {SYSCTL_RESET_HDI, RW_0, 0x20, 7, 0, 0xff0ff},
    {SYSCTL_RESET_WDT0, RW_0, 0x20, 12, 0, 0xff0ff},
    {SYSCTL_RESET_WDT1, RW_0, 0x20, 13, 0, 0xff0ff},
    {SYSCTL_RESET_WDT0_APB, RW_0, 0x20, 14, 0, 0xff0ff},
    {SYSCTL_RESET_WDT1_APB, RW_0, 0x20, 15, 0, 0xff0ff},
    {SYSCTL_RESET_TS_APB, RW_0, 0x20, 16, 0, 0xff0ff},
    {SYSCTL_RESET_MAILBOX, RW_0, 0x20, 17, 0, 0xff0ff},
    {SYSCTL_RESET_STC, RW_0, 0x20, 18, 0, 0xff0ff},
    {SYSCTL_RESET_PMU, RW_0, 0x20, 19, 0, 0xff0ff},
    {SYSCTL_RESET_LS_APB, RW_0, 0x24, 0, 0, 0x7e7fff},
    {SYSCTL_RESET_UART0, RW_0, 0x24, 1, 0, 0x7e7fff},
    {SYSCTL_RESET_UART1, RW_0, 0x24, 2, 0, 0x7e7fff},
    {SYSCTL_RESET_UART2, RW_0, 0x24, 3, 0, 0x7e7fff},
    {SYSCTL_RESET_UART3, RW_0, 0x24, 4, 0, 0x7e7fff},
    {SYSCTL_RESET_UART4, RW_0, 0x24, 5, 0, 0x7e7fff},
    {SYSCTL_RESET_I2C0, RW_0, 0x24, 6, 0, 0x7e7fff},
    {SYSCTL_RESET_I2C1, RW_0, 0x24, 7, 0, 0x7e7fff},
    {SYSCTL_RESET_I2C2, RW_0, 0x24, 8, 0, 0x7e7fff},
    {SYSCTL_RESET_I2C3, RW_0, 0x24, 9, 0, 0x7e7fff},
    {SYSCTL_RESET_I2C4, RW_0, 0x24, 10, 0, 0x7e7fff},
    {SYSCTL_RESET_JAMLINK0_APB, RW_0, 0x24, 11, 0, 0x7e7fff},
    {SYSCTL_RESET_JAMLINK1_APB, RW_0, 0x24, 12, 0, 0x7e7fff},
    {SYSCTL_RESET_JAMLINK2_APB, RW_0, 0x24, 13, 0, 0x7e7fff},
    {SYSCTL_RESET_JAMLINK3_APB, RW_0, 0x24, 14, 0, 0x7e7fff},
    {SYSCTL_RESET_CODEC_APB, RW_0, 0x24, 17, 0, 0x7e7fff},
    {SYSCTL_RESET_GPIO_DB, RW_0, 0x24, 18, 0, 0x7e7fff},
    {SYSCTL_RESET_GPIO_APB, RW_0, 0x24, 19, 0, 0x7e7fff},
    {SYSCTL_RESET_ADC, RW_0, 0x24, 20, 0, 0x7e7fff},
    {SYSCTL_RESET_ADC_APB, RW_0, 0x24, 21, 0, 0x7e7fff},
    {SYSCTL_RESET_PWM_APB, RW_0, 0x24, 22, 0, 0x7e7fff},
    {SYSCTL_RESET_SPI2AXI, RW_0, 0xa8, 0, 0, 0x1},
};

static int k230_reset_of_xlate(struct reset_controller_dev *rcdev, const struct of_phandle_args *reset_spec)
{
    uint32_t reset;

    reset  = reset_spec->args[0];

    return reset;
}

static int k230_reset(struct reset_controller_dev *rcdev, unsigned long id) 
{
    struct k230_reset_controller *rstc = to_k230_reset_controller(rcdev);
    unsigned long flags;
    uint32_t reset = id;

    uint32_t type = k_reset[reset].type;
    uint8_t offset = k_reset[reset].reg_offset;
    uint8_t reset_bit = k_reset[reset].reset_bit;
    uint8_t done_bit = k_reset[reset].done_bit;
    uint32_t mask_rw = k_reset[reset].mask_rw;
    uint32_t data = 0;

    spin_lock_irqsave(&rstc->lock, flags);
    /* clear done bit */
    if(type & W1C)
    {
        data = readl(rstc->membase+offset) & mask_rw;

        data |= (1 << done_bit);
        if(type & WE)
        {
            data |= (1 << (done_bit + 16));  /* write enable */
        }
        writel(data, rstc->membase+offset);
    }
    /* set reset bit */
    data = readl(rstc->membase+offset) & mask_rw;

    if((type & W1T) || (type & RWSC) || (type & RW_1))
    {
        data |= (1 << reset_bit);
    }
    else if(type & RW_0)
    {
        data &= ~(1 << reset_bit);
    }
    if(type & WE)
    {
        data |= (1 << (reset_bit + 16));  /* write enable */
    }
    writel(data, rstc->membase+offset);
    spin_unlock_irqrestore(&rstc->lock, flags);

    msleep(1);
    
    /* clear reset bit */
    if((type & RW_0) || (type & RW_1))
    {
        spin_lock_irqsave(&rstc->lock, flags);
        data = readl(rstc->membase+offset) & mask_rw;

        if(type & RW_0) data |= (1 << reset_bit);
        else if(type & RW_1) data &= ~(1 << reset_bit);

        if(type & WE)
        {
            data |= (1 << (reset_bit + 16));  /* write enable */
        }
        writel(data, rstc->membase+offset);
        spin_unlock_irqrestore(&rstc->lock, flags);
    }
    
    /* check done bit */
    if(type & W1C)
    {
        if(readl(rstc->membase+offset) & (1 << done_bit))
            return 0;

        return -1;
    }
    if(type & RWSC)
    {
        if((readl(rstc->membase+offset) & (1 << done_bit)) == 0)
            return 0;

        return -1;
    }
    return 0;
}

static int k230_reset_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
    struct k230_reset_controller *rstc = to_k230_reset_controller(rcdev);
    unsigned long flags;
    uint32_t reset = id;

    uint32_t type = k_reset[reset].type;
    uint8_t offset = k_reset[reset].reg_offset;
    uint8_t reset_bit = k_reset[reset].reset_bit;
    uint8_t done_bit = k_reset[reset].done_bit;
    uint32_t mask_rw = k_reset[reset].mask_rw;
    uint32_t data = 0;

    spin_lock_irqsave(&rstc->lock, flags);
    /* clear done bit */
    if(type & W1C)
    {
        data = readl(rstc->membase+offset) & mask_rw;

        data |= (1 << done_bit);
        if(type & WE)
        {
            data |= (1 << (done_bit + 16));  /* write enable */
        }
        writel(data, rstc->membase+offset);
    }
    /* set reset bit */
    data = readl(rstc->membase+offset) & mask_rw;

    if((type & W1T) || (type & RWSC) || (type & RW_1))
    {
        data |= (1 << reset_bit);
    }
    else if(type & RW_0)
    {
        data &= ~(1 << reset_bit);
    }
    if(type & WE)
    {
        data |= (1 << (reset_bit + 16));  /* write enable */
    }
    writel(data, rstc->membase+offset);
    spin_unlock_irqrestore(&rstc->lock, flags);

    return 0;
}

static int k230_reset_deassert(struct reset_controller_dev *rcdev, unsigned long id)
{
    struct k230_reset_controller *rstc = to_k230_reset_controller(rcdev);
    unsigned long flags;
    uint32_t reset = id;

    uint32_t type = k_reset[reset].type;
    uint8_t offset = k_reset[reset].reg_offset;
    uint8_t reset_bit = k_reset[reset].reset_bit;
    uint8_t done_bit = k_reset[reset].done_bit;
    uint32_t mask_rw = k_reset[reset].mask_rw;
    uint32_t data = 0;

        /* clear reset bit */
    if((type & RW_0) || (type & RW_1))
    {
        spin_lock_irqsave(&rstc->lock, flags);
        data = readl(rstc->membase+offset) & mask_rw;

        if(type & RW_0) data |= (1 << reset_bit);
        else if(type & RW_1) data &= ~(1 << reset_bit);

        if(type & WE)
        {
            data |= (1 << (reset_bit + 16));  /* write enable */
        }
        writel(data, rstc->membase+offset);
        spin_unlock_irqrestore(&rstc->lock, flags);
    }
    
    /* check done bit */
    if(type & W1C)
    {
        if(readl(rstc->membase+offset) & (1 << done_bit))
            return 0;

        return -1;
    }
    if(type & RWSC)
    {
        if((readl(rstc->membase+offset) & (1 << done_bit)) == 0)
            return 0;

        return -1;
    }

    return 0;
}

static const struct reset_control_ops k230_reset_ops = {
    .reset          = k230_reset,
    .assert         = k230_reset_assert,
    .deassert       = k230_reset_deassert,
};

static int k230_reset_probe(struct platform_device *pdev)
{
    struct k230_reset_controller *rstc;
    struct resource *res;

    rstc = devm_kmalloc(&pdev->dev, sizeof(*rstc), GFP_KERNEL);
    if (!rstc) {
        pr_err("k230_reset_init dev_kmalloc error!");
        return -1;
    }

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    rstc->membase = devm_ioremap(&pdev->dev, res->start, resource_size(res));
    if (!rstc->membase) {
        pr_err("k230_reset_init devm_ioremap error!");
        return -1;
    }
    #ifdef K230_RESET_DEBUG
        pr_info("[K230_RESET]:sysctl reset phy addr 0x%08x",(int)res->start);
    #endif

    spin_lock_init(&rstc->lock);
    rstc->rst.owner = THIS_MODULE;
    rstc->rst.ops = &k230_reset_ops;
    rstc->rst.of_node = pdev->dev.of_node;
    rstc->rst.of_reset_n_cells = 1;
    rstc->rst.of_xlate = k230_reset_of_xlate;
    if(0 == reset_controller_register(&rstc->rst)) {
    #ifdef K230_RESET_DEBUG
        pr_info("[K230_RESET]:k230_reset_probe ok!");
    #endif
    } else {
        pr_info("[K230_RESET]:k230_reset_probe error!");
    }
    return 0;
}

void k230_reset_exit(struct k230_reset_controller *rstc)
{
    reset_controller_unregister(&rstc->rst);
}

static const struct of_device_id k230_reset_match[] = {
    { .compatible = "canaan,k230-sysctl-reset",},
    {},
};
MODULE_DEVICE_TABLE(of, k230_reset_match);

static struct platform_driver k230_reset_driver = {
    .probe = k230_reset_probe,
    .driver = {
        .name = "k230-sysctl-reset",
        .of_match_table = k230_reset_match,
    }
};

static int __init k230_reset_init(void)
{
    return platform_driver_register(&k230_reset_driver);
}
arch_initcall(k230_reset_init);

MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:k230-sysctl-reset");
MODULE_DESCRIPTION("Canaan K230 Reset Driver");

/* how to reset device in the device driver:
    1. write reset attribute in reset_consumer.dtsi
    2. use device_reset(&platform_device->device) api to reset device.
    for example: please see emac driver and dtsi
*/
