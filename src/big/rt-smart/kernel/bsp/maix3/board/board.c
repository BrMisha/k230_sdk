/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2021-01-30     lizhirui     first version
 */

#include <rthw.h>
#include <rtthread.h>
#include <rtdevice.h>

#include "board.h"
#include "tick.h"

#include "drv_uart.h"
#include "encoding.h"
#include "stack.h"
#include "sbi.h"
#include "riscv.h"
#include "stack.h"
#include "sysctl_boot.h"

#ifdef RT_USING_USERSPACE
    #include "riscv_mmu.h"
    #include "mmu.h"
    #include "page.h"
    #include "lwp_arch.h"
    #include "ioremap.h"

    //这个结构体描述了buddy system的页分配范围
    rt_region_t init_page_region =
    {
        (rt_size_t)RT_HW_PAGE_START,
        (rt_size_t)RT_HW_PAGE_END
    };

    //内核页表
    volatile rt_size_t MMUTable[__SIZE(VPN2_BIT)] __attribute__((aligned(4 * 1024)));
    rt_mmu_info mmu_info;

#endif

//初始化BSS节区
void init_bss(void)
{
    unsigned int *dst;

    dst = &__bss_start;
    while (dst < &__bss_end)
    {
        *dst++ = 0;
    }
}

#define MEM_RESVERD_SIZE    0x1000      /*隔离区*/
#define MEM_IPCM_BASE 0x100000
#define MEM_IPCM_SIZE 0xff000


void init_ipcm_mem(void)
{
    rt_uint32_t *dst;
    int i = 0;
    dst = rt_ioremap((void *)(MEM_IPCM_BASE + MEM_IPCM_SIZE - MEM_RESVERD_SIZE - MEM_RESVERD_SIZE), MEM_RESVERD_SIZE);
    if(dst == RT_NULL) {
        rt_kprintf("ipcm ioremap error\n");
    }
    rt_memset((void *)dst, 0, MEM_RESVERD_SIZE);
    for(i = 0; i < (0x1000 / 4); i++) {
        if(dst[i] != 0) {
            rt_kprintf("memest error addr:%p value:%d\n", &dst[i], dst[i]);
        }
    }
    rt_iounmap((void *)dst);
}

static void __rt_assert_handler(const char *ex_string, const char *func, rt_size_t line)
{
    rt_kprintf("(%s) assertion failed at function:%s, line number:%d \n", ex_string, func, line);
    asm volatile("ebreak":::"memory");
}

// IOMUX configuration structures
typedef struct {
    rt_uint32_t st : 1;
    rt_uint32_t ds : 4;
    rt_uint32_t pd : 1;
    rt_uint32_t pu : 1;
    rt_uint32_t oe_en : 1;
    rt_uint32_t ie_en : 1;
    rt_uint32_t msc : 1;
    rt_uint32_t sl : 1;
    rt_uint32_t io_sel : 3;
    rt_uint32_t resv0 : 17;
    rt_uint32_t pad_di : 1;
} mux_config_t;

#define MUXPIN_NUM_IO (64)
typedef struct {
    mux_config_t io[MUXPIN_NUM_IO];
} muxpin_t;

// Configure SPI0 pins (QSPI0_CONF1: pins 14-17) for Linux fbtft driver
static void init_spi0_pins(void)
{
    volatile muxpin_t *muxpin = (volatile muxpin_t *)IOMUX_BASE_ADDR;

    // Configure pins 14-17 for SPI function (io_sel = 3)
    // Pin 14: SPI0 CS - moderate drive strength (ds=3) to reduce overshoot/ringing
    mux_config_t cs_pin = {.st = 1, .ds = 0x3, .pd = 0, .pu = 1, .oe_en = 1,
                          .ie_en = 0, .msc = 1, .sl = 1, .io_sel = 3};
    // Pin 15: SPI0 CLK - moderate drive strength for cleaner edges
    mux_config_t clk_pin = {.st = 1, .ds = 0x3, .pd = 0, .pu = 0, .oe_en = 1,
                           .ie_en = 0, .msc = 1, .sl = 1, .io_sel = 3};
    // Pin 16: SPI0 D0 (MOSI) - moderate drive strength
    mux_config_t d0_pin = {.st = 1, .ds = 0x3, .pd = 0, .pu = 0, .oe_en = 1,
                          .ie_en = 1, .msc = 1, .sl = 1, .io_sel = 3};
    // Pin 17: SPI0 D1 (MISO) - moderate drive strength
    mux_config_t d1_pin = {.st = 1, .ds = 0x3, .pd = 0, .pu = 0, .oe_en = 1,
                          .ie_en = 1, .msc = 1, .sl = 1, .io_sel = 3};

    muxpin->io[14] = cs_pin;
    muxpin->io[15] = clk_pin;
    muxpin->io[16] = d0_pin;
    muxpin->io[17] = d1_pin;
}

//BSP的C入口
void primary_cpu_entry(void)
{
    extern void entry(void);

    //初始化BSS
    init_bss();
    //关中断
    rt_hw_interrupt_disable();
    rt_assert_set_hook(__rt_assert_handler);

    // Configure SPI0 pins for Linux
    init_spi0_pins();

    //启动RT-Thread Smart内核
    entry();
}

#define IOREMAP_SIZE (1ul << 30)

//这个初始化程序由内核主动调用，此时调度器还未启动，因此在此不能使用依赖线程上下文的函数
void rt_hw_board_init(void)
{
#ifdef RT_USING_USERSPACE
    // rt_hw_mmu_map_init actually allocate a region for ioremap,
    // set (0xC000-0000 to 0xFFFF-FFFF) as IOREMAP(or kernel virt addr allocation) region
    // any peripherals in this region should be accessed after ioremap
    rt_hw_mmu_map_init(&mmu_info, (void *)(0x100000000UL - IOREMAP_SIZE), IOREMAP_SIZE, (rt_size_t *)MMUTable, 0);
    rt_page_init(init_page_region);
    rt_hw_mmu_kernel_map_init(&mmu_info, 0x00000000UL, (0x100000000UL - IOREMAP_SIZE));

    //将第3GB MMIO区域设置为无Cache与Strong Order访存模式
    MMUTable[2] &= ~PTE_CACHE;
    MMUTable[2] &= ~PTE_SHARE;
    MMUTable[2] |= PTE_SO;

    rt_hw_mmu_switch((void *)MMUTable);
#endif
#ifdef RT_USING_HEAP
    // rt_kprintf("heap: [0x%08x - 0x%08x]\n", (rt_ubase_t) RT_HW_HEAP_BEGIN, (rt_ubase_t) RT_HW_HEAP_END);
    /* initialize memory system */
    rt_system_heap_init(RT_HW_HEAP_BEGIN, RT_HW_HEAP_END);
#endif
#if  MEM_IPCM_SIZE >  MEM_RESVERD_SIZE
    init_ipcm_mem();
    /* initalize interrupt */
#endif 
    rt_hw_interrupt_init();

    /* initialize hardware interrupt */

    rt_hw_tick_init();

#ifdef RT_USING_CONSOLE
    if(!rt_strcmp(RT_CONSOLE_DEVICE_NAME, "uart"))
        rt_hw_uart_init();
#ifdef RT_USING_IPCM
extern int rt_virt_tty_device_init();;
    if(!rt_strcmp(RT_CONSOLE_DEVICE_NAME, "virt-tty"))
        rt_virt_tty_device_init();
#endif
#endif /* RT_USING_CONSOLE */

#ifdef RT_USING_COMPONENTS_INIT
    rt_components_board_init();
#endif
}

void rt_hw_cpu_reset(void)
{
    // sbi_shutdown();
    sysctl_boot_reset_soc();
    while(1);
}
MSH_CMD_EXPORT_ALIAS(rt_hw_cpu_reset, reboot, reset machine);
