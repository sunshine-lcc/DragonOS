#include "apic.h"
#include "../../../common/kprint.h"
#include "../../../common/printk.h"
#include "../../../common/cpu.h"
#include "../../../common/glib.h"
#include "../../../exception/gate.h"
#include "../../acpi/acpi.h"

#include <exception/softirq.h>
#include <process/process.h>
#include <sched/sched.h>

// 导出定义在irq.c中的中段门表
extern void (*interrupt_table[24])(void);

bool flag_support_apic = false;
bool flag_support_x2apic = false;
uint local_apic_version;
uint local_apic_max_LVT_entries;

static struct acpi_Multiple_APIC_Description_Table_t *madt;
static struct acpi_IO_APIC_Structure_t *io_apic_ICS;
/**
 * @brief 初始化io_apic
 *
 */
void apic_io_apic_init()
{

    ul madt_addr;
    acpi_iter_SDT(acpi_get_MADT, &madt_addr);
    madt = (struct acpi_Multiple_APIC_Description_Table_t *)madt_addr;

    // kdebug("MADT->local intr controller addr=%#018lx", madt->Local_Interrupt_Controller_Address);
    // kdebug("MADT->length= %d bytes", madt->header.Length);
    //  寻找io apic的ICS
    void *ent = (void *)(madt_addr) + sizeof(struct acpi_Multiple_APIC_Description_Table_t);
    struct apic_Interrupt_Controller_Structure_header_t *header = (struct apic_Interrupt_Controller_Structure_header_t *)ent;
    while (header->length > 2)
    {
        header = (struct apic_Interrupt_Controller_Structure_header_t *)ent;
        if (header->type == 1)
        {
            struct acpi_IO_APIC_Structure_t *t = (struct acpi_IO_APIC_Structure_t *)ent;
            // kdebug("IO apic addr = %#018lx", t->IO_APIC_Address);
            io_apic_ICS = t;
            break;
        }

        ent += header->length;
    }
    // kdebug("Global_System_Interrupt_Base=%d", io_apic_ICS->Global_System_Interrupt_Base);

    apic_ioapic_map.addr_phys = io_apic_ICS->IO_APIC_Address;
    apic_ioapic_map.virtual_index_addr = (unsigned char *)APIC_IO_APIC_VIRT_BASE_ADDR;
    apic_ioapic_map.virtual_data_addr = (uint *)(APIC_IO_APIC_VIRT_BASE_ADDR + 0x10);
    apic_ioapic_map.virtual_EOI_addr = (uint *)(APIC_IO_APIC_VIRT_BASE_ADDR + 0x40);

    // kdebug("(ul)apic_ioapic_map.virtual_index_addr=%#018lx", (ul)apic_ioapic_map.virtual_index_addr);
    // 填写页表，完成地址映射
    mm_map_phys_addr((ul)apic_ioapic_map.virtual_index_addr, apic_ioapic_map.addr_phys, PAGE_2M_SIZE, PAGE_KERNEL_PAGE | PAGE_PWT | PAGE_PCD);

    // 设置IO APIC ID 为0x0f000000
    *apic_ioapic_map.virtual_index_addr = 0x00;
    io_mfence();
    *apic_ioapic_map.virtual_data_addr = 0x0f000000;
    io_mfence();

    // kdebug("I/O APIC ID:%#010x", ((*apic_ioapic_map.virtual_data_addr) >> 24) & 0xff);
    io_mfence();

    // 获取IO APIC Version
    *apic_ioapic_map.virtual_index_addr = 0x01;
    io_mfence();
    kdebug("IO APIC Version=%d, Max Redirection Entries=%d", *apic_ioapic_map.virtual_data_addr & 0xff, (((*apic_ioapic_map.virtual_data_addr) >> 16) & 0xff) + 1);

    // 初始化RTE表项，将所有RTE表项屏蔽
    for (int i = 0x10; i < 0x40; i += 2)
    {
        // 以0x20为起始中断向量号，初始化RTE
        apic_ioapic_write_rte(i, 0x10020 + ((i - 0x10) >> 1));
    }

    // 不需要手动启动IO APIC，只要初始化了RTE寄存器之后，io apic就会自动启用了。
    // 而且不是每台电脑都有RCBA寄存器，因此不需要手动启用IO APIC
    /*
           // get RCBA address
           io_out32(0xcf8, 0x8000f8f0);
           uint x = io_in32(0xcfc);
           uint *p;
           printk_color(RED, BLACK, "Get RCBA Address:%#010x\n", x);
           x = x & 0xffffc000UL;
           printk_color(RED, BLACK, "Get RCBA Address:%#010x\n", x);

           // get OIC address
           if (x > 0xfec00000 && x < 0xfee00000)
           {
               p = (unsigned int *)(x + 0x31feUL-apic_ioapic_map.addr_phys+apic_ioapic_map.virtual_index_addr);
           }

           // enable IOAPIC
           x = (*p & 0xffffff00) | 0x100;
           io_mfence();
           *p = x;
           io_mfence();
           */
}

/**
 * @brief 初始化AP处理器的Local apic
 *
 */
void apic_init_ap_core_local_apic()
{
    kinfo("Initializing AP-core's local apic...");
    uint eax, edx;
    // 启用xAPIC 和x2APIC
    __asm__ __volatile__("movq  $0x1b, %%rcx   \n\t" // 读取IA32_APIC_BASE寄存器
                         "rdmsr  \n\t"
                         "bts $10,   %%rax  \n\t"
                         "bts $11,   %%rax   \n\t"
                         "wrmsr  \n\t"
                         "movq $0x1b,    %%rcx   \n\t"
                         "rdmsr  \n\t"
                         : "=a"(eax), "=d"(edx)::"memory");

    // kdebug("After enable xAPIC and x2APIC: edx=%#010x, eax=%#010x", edx, eax);

    // 检测是否成功启用xAPIC和x2APIC
    if (eax & 0xc00)
        kinfo("xAPIC & x2APIC enabled!");
    // 设置SVR寄存器，开启local APIC、禁止EOI广播

    // enable SVR[8]
    __asm__ __volatile__("movq 	$0x80f,	%%rcx	\n\t"
                         "rdmsr	\n\t"
                         "bts	$8,	%%rax	\n\t"
                         //				"bts	$12,	%%rax\n\t"
                         "wrmsr	\n\t"
                         "movq 	$0x80f,	%%rcx	\n\t"
                         "rdmsr	\n\t"
                         : "=a"(eax), "=d"(edx)
                         :
                         : "memory");

    if (eax & 0x100)
        printk_color(RED, YELLOW, "SVR[8] enabled\n");
    if (edx & 0x1000)
        printk_color(RED, YELLOW, "SVR[12] enabled\n");

    // get local APIC ID
    __asm__ __volatile__("movq $0x802,	%%rcx	\n\t"
                         "rdmsr	\n\t"
                         : "=a"(eax), "=d"(edx)
                         :
                         : "memory");

    printk_color(RED, YELLOW, "x2APIC ID:%#010x\n", eax);

    // 由于尚未配置LVT对应的处理程序，因此先屏蔽所有的LVT

    // mask all LVT
    __asm__ __volatile__(             //"movq 	$0x82f,	%%rcx	\n\t"	//CMCI
                                      //"wrmsr	\n\t"
        "movq 	$0x832,	%%rcx	\n\t" // Timer
        "wrmsr	\n\t"
        "movq 	$0x833,	%%rcx	\n\t" // Thermal Monitor
        "wrmsr	\n\t"
        "movq 	$0x834,	%%rcx	\n\t" // Performance Counter
        "wrmsr	\n\t"
        "movq 	$0x835,	%%rcx	\n\t" // LINT0
        "wrmsr	\n\t"
        "movq 	$0x836,	%%rcx	\n\t" // LINT1
        "wrmsr	\n\t"
        "movq 	$0x837,	%%rcx	\n\t" // Error
        "wrmsr	\n\t"
        :
        : "a"(0x10000), "d"(0x00)
        : "memory");

    /*
    io_mfence();
    *(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_LVT_CMCI) = 0x1000000;
    io_mfence();
    kdebug("cmci = %#018lx", *(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_LVT_CMCI));
    */
    //*(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_LVT_TIMER) = 0x10000;
    // io_mfence();
    /*
    *(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_LVT_THERMAL) = 0x1000000;
    io_mfence();
    *(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_LVT_PERFORMANCE_MONITOR) = 0x1000000;
    io_mfence();
    *(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_LVT_LINT0) = 0x1000000;
    io_mfence();
    *(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_LVT_LINT1) = 0x1000000;
    io_mfence();
    *(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_LVT_ERROR) = 0x1000000;
    io_mfence();
    */
    kdebug("All LVT Masked");
}
/**
 * @brief 初始化local apic
 *
 */
void apic_local_apic_init()
{
    // 映射Local APIC 寄存器地址
    mm_map_phys_addr(APIC_LOCAL_APIC_VIRT_BASE_ADDR, 0xfee00000, PAGE_2M_SIZE, PAGE_KERNEL_PAGE | PAGE_PWT | PAGE_PCD);
    uint a, b, c, d;

    cpu_cpuid(1, 0, &a, &b, &c, &d);

    // kdebug("CPUID 0x01, eax:%#010lx, ebx:%#010lx, ecx:%#010lx, edx:%#010lx", a, b, c, d);

    // 判断是否支持APIC和xAPIC
    if ((1 << 9) & d)
    {
        flag_support_apic = true;
        kdebug("This computer support APIC&xAPIC");
    }
    else
    {
        flag_support_apic = false;
        kerror("This computer does not support APIC&xAPIC");
        while (1)
            ;
    }

    // 判断是否支持x2APIC
    if ((1 << 21) & c)
    {
        flag_support_x2apic = true;
        kdebug("This computer support x2APIC");
    }
    else
    {
        kerror("This computer does not support x2APIC");
    }

    uint eax, edx;
    // 启用xAPIC 和x2APIC
    __asm__ __volatile__("movq  $0x1b, %%rcx   \n\t" // 读取IA32_APIC_BASE寄存器
                         "rdmsr  \n\t"
                         "bts $10,   %%rax  \n\t"
                         "bts $11,   %%rax   \n\t"
                         "wrmsr  \n\t"
                         "movq $0x1b,    %%rcx   \n\t"
                         "rdmsr  \n\t"
                         : "=a"(eax), "=d"(edx)::"memory");

    // kdebug("After enable xAPIC and x2APIC: edx=%#010x, eax=%#010x", edx, eax);

    // 检测是否成功启用xAPIC和x2APIC
    if (eax & 0xc00)
        kinfo("xAPIC & x2APIC enabled!");
    /*
        io_mfence();
        uint *svr = (uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_SVR);
        uint tmp_svr = *svr;
        tmp_svr &= (~(1 << 12));
        tmp_svr |= (1 << 8);
        kdebug("tmp_svr = %#018lx", tmp_svr);
        io_mfence();
        *svr = tmp_svr;
        io_mfence();
        kdebug("svr = %#018lx", *svr);
    */
    // 设置SVR寄存器，开启local APIC、禁止EOI广播

    __asm__ __volatile__("movq $0x80f, %%rcx    \n\t"
                         "rdmsr  \n\t"
                         "bts $8, %%rax  \n\t"
                         //                         "bts $12, %%rax \n\t"
                         "movq $0x80f, %%rcx    \n\t"
                         "wrmsr  \n\t"
                         "movq $0x80f , %%rcx   \n\t"
                         "rdmsr \n\t"
                         : "=a"(eax), "=d"(edx)::"memory");

    /*
   //enable SVR[8]
    __asm__ __volatile__(	"movq 	$0x80f,	%%rcx	\n\t"
                "rdmsr	\n\t"
                "bts	$8,	%%rax	\n\t"
                "bts	$12,%%rax\n\t"
                "wrmsr	\n\t"
                "movq 	$0x80f,	%%rcx	\n\t"
                "rdmsr	\n\t"
                :"=a"(eax),"=d"(edx)
                :
                :"memory");
                */
    // kdebug("After setting SVR: edx=%#010x, eax=%#010x", edx, eax);

    if (eax & 0x100)
        kinfo("APIC Software Enabled.");
    if (eax & 0x1000)
        kinfo("EOI-Broadcast Suppression Enabled.");

    // 获取Local APIC的基础信息 （参见英特尔开发手册Vol3A 10-39）
    //                          Table 10-6. Local APIC Register Address Map Supported by x2APIC
    // 获取 Local APIC ID
    // 0x802处是x2APIC ID 位宽32bits 的 Local APIC ID register
    /*
    __asm__ __volatile__("movq $0x802, %%rcx    \n\t"
                         "rdmsr  \n\t"
                         : "=a"(eax), "=d"(edx)::"memory");
    */
    // kdebug("get Local APIC ID: edx=%#010x, eax=%#010x", edx, eax);
    // kdebug("local_apic_id=%#018lx", *(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_ID));

    // 获取Local APIC Version
    // 0x803处是 Local APIC Version register
    __asm__ __volatile__("movq $0x803, %%rcx    \n\t"
                         "rdmsr  \n\t"
                         : "=a"(eax), "=d"(edx)::"memory");

    local_apic_max_LVT_entries = ((eax >> 16) & 0xff) + 1;
    local_apic_version = eax & 0xff;

    kdebug("local APIC Version:%#010x,Max LVT Entry:%#010x,SVR(Suppress EOI Broadcast):%#04x\t", local_apic_version, local_apic_max_LVT_entries, (eax >> 24) & 0x1);

    if ((eax & 0xff) < 0x10)
    {
        kdebug("82489DX discrete APIC");
    }
    else if (((eax & 0xff) >= 0x10) && ((eax & 0xff) <= 0x15))
        kdebug("Integrated APIC.");

    // 由于尚未配置LVT对应的处理程序，因此先屏蔽所有的LVT

    // mask all LVT
    __asm__ __volatile__(             //"movq 	$0x82f,	%%rcx	\n\t"	//CMCI
                                      //"wrmsr	\n\t"
        "movq 	$0x832,	%%rcx	\n\t" // Timer
        "wrmsr	\n\t"
        "movq 	$0x833,	%%rcx	\n\t" // Thermal Monitor
        "wrmsr	\n\t"
        "movq 	$0x834,	%%rcx	\n\t" // Performance Counter
        "wrmsr	\n\t"
        "movq 	$0x835,	%%rcx	\n\t" // LINT0
        "wrmsr	\n\t"
        "movq 	$0x836,	%%rcx	\n\t" // LINT1
        "wrmsr	\n\t"
        "movq 	$0x837,	%%rcx	\n\t" // Error
        "wrmsr	\n\t"
        :
        : "a"(0x10000), "d"(0x00)
        : "memory");

    /*
    io_mfence();
    *(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_LVT_CMCI) = 0x1000000;
    io_mfence();
    kdebug("cmci = %#018lx", *(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_LVT_CMCI));
    */
    *(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_LVT_TIMER) = 0x10000;
    io_mfence();
    /*
    *(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_LVT_THERMAL) = 0x1000000;
    io_mfence();
    *(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_LVT_PERFORMANCE_MONITOR) = 0x1000000;
    io_mfence();
    *(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_LVT_LINT0) = 0x1000000;
    io_mfence();
    *(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_LVT_LINT1) = 0x1000000;
    io_mfence();
    *(uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_LVT_ERROR) = 0x1000000;
    io_mfence();
    */
    kdebug("All LVT Masked");

    /*
    // 获取TPR寄存器的值
    __asm__ __volatile__("movq $0x808, %%rcx    \n\t"
                         "rdmsr  \n\t"
                         : "=a"(eax), "=d"(edx)::"memory");
    kdebug("LVT_TPR=%#010x", eax);

    // 获取PPR寄存器的值
    __asm__ __volatile__("movq $0x80a, %%rcx    \n\t"
                         "rdmsr  \n\t"
                         : "=a"(eax), "=d"(edx)::"memory");
    kdebug("LVT_PPR=%#010x", eax);
    */
}

/**
 * @brief 初始化apic控制器
 *
 */
void apic_init()
{
    // 初始化中断门， 中断使用rsp0防止在软中断时发生嵌套，然后处理器重新加载导致数据被抹掉
    for (int i = 32; i <= 55; ++i)
        set_intr_gate(i, 0, interrupt_table[i - 32]);
    // 初始化主芯片
    io_out8(0x20, 0x11); // 初始化主芯片的icw1
    io_out8(0x21, 0x20); // 设置主芯片的中断向量号为0x20(0x20-0x27)
    io_out8(0x21, 0x04); // 设置int2端口级联从芯片
    io_out8(0x21, 0x01); // 设置为AEOI模式、FNM、无缓冲

    // 初始化从芯片
    io_out8(0xa0, 0x11);
    io_out8(0xa1, 0x28); // 设置从芯片的中断向量号为0x28(0x28-0x2f)
    io_out8(0xa1, 0x02); // 设置从芯片连接到主芯片的int2
    io_out8(0xa1, 0x01);

    //  屏蔽类8259A芯片
    io_mfence();
    io_out8(0x21, 0xff);
    io_mfence();
    io_out8(0xa1, 0xff);
    io_mfence();

    kdebug("8259A Masked.");

    // enable IMCR
    io_out8(0x22, 0x70);
    io_out8(0x23, 0x01);

    apic_local_apic_init();

    apic_io_apic_init();

    sti();
}
/**
 * @brief 中断服务程序
 *
 * @param rsp 中断栈指针
 * @param number 中断向量号
 */
void do_IRQ(struct pt_regs *rsp, ul number)
{
    switch (number & 0x80) // 以0x80为界限，低于0x80的是外部中断控制器，高于0x80的是Local APIC
    {
    case 0x00: // 外部中断控制器
        /* code */
        {
            irq_desc_t *irq = &interrupt_desc[number - 32];

            // 执行中断上半部处理程序
            if (irq->handler != NULL)
                irq->handler(number, irq->parameter, rsp);
            else
                kwarn("Intr vector [%d] does not have a handler!");

            // 向中断控制器发送应答消息
            if (irq->controller != NULL && irq->controller->ack != NULL)
                irq->controller->ack(number);
            else
            {

                // 向EOI寄存器写入0x00表示结束中断
                __asm__ __volatile__("movq	$0x00,	%%rdx	\n\t"
                                     "movq	$0x00,	%%rax	\n\t"
                                     "movq 	$0x80b,	%%rcx	\n\t"
                                     "wrmsr	\n\t" ::
                                         : "memory");
            }
        }
        break;
    case 0x80:
        printk_color(RED, BLACK, "SMP IPI [ %d ]\n", number);
        apic_local_apic_edge_ack(number);
    default:
        kwarn("do IRQ receive: %d", number);
        break;
    }

    // 检测是否有未处理的软中断
    if (softirq_status != 0)
        do_softirq();

    // 检测当前进程是否可被调度
    struct process_control_block *current_proc = get_current_pcb();
    if (current_proc->flags & PROC_NEED_SCHED)
        sched_cfs();
}

/**
 * @brief 读取RTE寄存器
 * 由于RTE位宽为64位而IO window寄存器只有32位，因此需要两次读取
 * @param index 索引值
 * @return ul
 */
ul apic_ioapic_read_rte(unsigned char index)
{
    // 由于处理器的乱序执行的问题，需要加入内存屏障以保证结果的正确性。
    ul ret;
    // 先读取高32bit
    *apic_ioapic_map.virtual_index_addr = index + 1;
    io_mfence();

    ret = *apic_ioapic_map.virtual_data_addr;
    ret <<= 32;
    io_mfence();

    // 读取低32bit
    *apic_ioapic_map.virtual_index_addr = index;
    io_mfence();
    ret |= *apic_ioapic_map.virtual_data_addr;
    io_mfence();

    return ret;
}

/**
 * @brief 写入RTE寄存器
 *
 * @param index 索引值
 * @param value 要写入的值
 */
void apic_ioapic_write_rte(unsigned char index, ul value)
{
    // 先写入低32bit
    *apic_ioapic_map.virtual_index_addr = index;
    io_mfence();

    *apic_ioapic_map.virtual_data_addr = value & 0xffffffff;
    io_mfence();
    // 再写入高32bit
    value >>= 32;
    io_mfence();
    *apic_ioapic_map.virtual_index_addr = index + 1;
    io_mfence();
    *apic_ioapic_map.virtual_data_addr = value & 0xffffffff;
    io_mfence();
}

// =========== 中断控制操作接口 ============
void apic_ioapic_enable(ul irq_num)
{
    ul index = 0x10 + ((irq_num - 32) << 1);
    ul value = apic_ioapic_read_rte(index);
    value &= (~0x10000UL);
    apic_ioapic_write_rte(index, value);
}

void apic_ioapic_disable(ul irq_num)
{
    ul index = 0x10 + ((irq_num - 32) << 1);
    ul value = apic_ioapic_read_rte(index);
    value |= (0x10000UL);
    apic_ioapic_write_rte(index, value);
}

ul apic_ioapic_install(ul irq_num, void *arg)
{
    struct apic_IO_APIC_RTE_entry *entry = (struct apic_IO_APIC_RTE_entry *)arg;
    // RTE表项值写入对应的RTE寄存器
    apic_ioapic_write_rte(0x10 + ((irq_num - 32) << 1), *(ul *)entry);
    return 0;
}

void apic_ioapic_uninstall(ul irq_num)
{
    // 将对应的RTE表项设置为屏蔽状态
    apic_ioapic_write_rte(0x10 + ((irq_num - 32) << 1), 0x10000UL);
}

void apic_ioapic_level_ack(ul irq_num) // 电平触发
{
    // 向EOI寄存器写入0x00表示结束中断
    /*io_mfence();
    uint *eoi = (uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_EOI);
    *eoi = 0x00;
    io_mfence(); */

    __asm__ __volatile__("movq	$0x00,	%%rdx	\n\t"
                         "movq	$0x00,	%%rax	\n\t"
                         "movq 	$0x80b,	%%rcx	\n\t"
                         "wrmsr	\n\t" ::
                             : "memory");
    *apic_ioapic_map.virtual_EOI_addr = irq_num;
}

void apic_ioapic_edge_ack(ul irq_num) // 边沿触发
{

    // 向EOI寄存器写入0x00表示结束中断
    /*
        uint *eoi = (uint *)(APIC_LOCAL_APIC_VIRT_BASE_ADDR + LOCAL_APIC_OFFSET_Local_APIC_EOI);
        *eoi = 0x00;

        */
    __asm__ __volatile__("movq	$0x00,	%%rdx	\n\t"
                         "movq	$0x00,	%%rax	\n\t"
                         "movq 	$0x80b,	%%rcx	\n\t"
                         "wrmsr	\n\t" ::
                             : "memory");
}

/**
 * @brief local apic 边沿触发应答
 *
 * @param irq_num
 */
void apic_local_apic_edge_ack(ul irq_num)
{
    // 向EOI寄存器写入0x00表示结束中断
    __asm__ __volatile__("movq	$0x00,	%%rdx	\n\t"
                         "movq	$0x00,	%%rax	\n\t"
                         "movq 	$0x80b,	%%rcx	\n\t"
                         "wrmsr	\n\t" ::
                             : "memory");
}

/**
 * @brief 读取指定类型的 Interrupt Control Structure
 *
 * @param type ics的类型
 * @param ret_vaddr 对应的ICS的虚拟地址数组
 * @param total 返回数组的元素总个数
 * @return uint
 */
uint apic_get_ics(const uint type, ul ret_vaddr[], uint *total)
{
    void *ent = (void *)(madt) + sizeof(struct acpi_Multiple_APIC_Description_Table_t);
    struct apic_Interrupt_Controller_Structure_header_t *header = (struct apic_Interrupt_Controller_Structure_header_t *)ent;
    bool flag = false;

    uint cnt = 0;

    while (header->length > 2)
    {
        header = (struct apic_Interrupt_Controller_Structure_header_t *)ent;
        if (header->type == type)
        {
            ret_vaddr[cnt++] = (ul)ent;
            flag = true;
        }
        ent += header->length;
    }

    *total = cnt;
    if (!flag)
        return APIC_E_NOTFOUND;
    else
        return APIC_SUCCESS;
}

/**
 * @brief 构造RTE Entry结构体
 *
 * @param entry 返回的结构体
 * @param vector 中断向量
 * @param deliver_mode 投递模式
 * @param dest_mode 目标模式
 * @param deliver_status 投递状态
 * @param polarity 电平触发极性
 * @param irr 远程IRR标志位（只读）
 * @param trigger 触发模式
 * @param mask 屏蔽标志位，（0为未屏蔽， 1为已屏蔽）
 * @param dest_apicID 目标apicID
 */
void apic_make_rte_entry(struct apic_IO_APIC_RTE_entry *entry, uint8_t vector, uint8_t deliver_mode, uint8_t dest_mode,
                         uint8_t deliver_status, uint8_t polarity, uint8_t irr, uint8_t trigger, uint8_t mask, uint8_t dest_apicID)
{

    entry->vector = vector;
    entry->deliver_mode = deliver_mode;
    entry->dest_mode = dest_mode;
    entry->deliver_status = deliver_status;
    entry->polarity = polarity;
    entry->remote_IRR = irr;
    entry->trigger_mode = trigger;
    entry->mask = mask;

    entry->reserved = 0;

    if (dest_mode == DEST_PHYSICAL)
    {
        entry->destination.physical.phy_dest = dest_apicID;
        entry->destination.physical.reserved1 = 0;
        entry->destination.physical.reserved2 = 0;
    }
    else
    {
        entry->destination.logical.logical_dest = dest_apicID;
        entry->destination.logical.reserved1 = 0;
    }
}