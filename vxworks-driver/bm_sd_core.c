/******************************************************************************
 * bm_sd_core.c
 *
 * BM1684X SD 卡协议层：卡上电识别、总线/速度配置、按扇区读写、容量识别。
 * 由 bm_emmc_core.c（eMMC 版本）改造而来，按用户明确要求"具体细节以 uboot
 * 为准"，本文件的卡识别时序【逐条对照】U-Boot 通用 MMC/SD 协议层
 * u-boot/drivers/mmc/mmc.c 里 SD 卡专属的函数改写，不再是只照搬思路：
 *   - mmc_go_idle()              -> CMD0
 *   - mmc_send_if_cond()         -> CMD8（SEND_IF_COND，区分 SD v1/v2）
 *   - sd_send_op_cond()          -> CMD55+ACMD41（轮询 OCR busy 位，取 HCS）
 *   - mmc_startup() 的 SD 分支    -> CMD2/CMD3/CMD9/CMD7，CSD 容量解析公式
 *   - sd_select_bus_width()      -> CMD55+ACMD6（切到 4 位总线）
 *
 * 与 eMMC 版本（bm_emmc_core.c）的关键差异（均来自 SD 协议本身，不是本项目
 * 自创）：
 *   - 没有 CMD1，识别态走 CMD8 + ACMD41（而不是 CMD1 OCR 轮询）；
 *   - RCA 由卡自己上报（CMD3 响应里取），不是主机指定，eMMC 是主机指定固定值；
 *   - 容量来自 CMD9 返回的 CSD 寄存器（按 U-Boot 公式解析），不是 EXT_CSD——
 *     SD 卡没有 EXT_CSD 这个东西；
 *   - 总线位宽切换用 ACMD6（APP_CMD 前缀的厂商无关标准命令），不是 eMMC 的
 *     CMD6 SWITCH 改 EXT_CSD 字段；
 *   - SD 卡可插拔，初始化前先查卡是否在位（eMMC 焊死在板上，不需要这一步）。
 *
 * TOP 域时钟使能/软复位、EMMC_CTRL_R 厂商位、DMA cache 维护这几块和 eMMC
 * 版本的处理思路相同（见 bm_emmc_core.c 顶部注释），不再重复展开，仅在下面
 * 对应位置注明与 SD 通道（devIndex=1，基址 0x50101000）相关的差异点。
 *
 * 【上板实测反馈的真实缺口，已修复】SD 卡槛有一个独立于 SDHCI 标准寄存器之外
 * 的供电开关 GPIO（SDIO_PWR_EN，port1a bit10，对照设备树 pwr-gpio 属性和
 * u-boot drivers/mmc/sdhci.c 里 sdhci_init()/sdhci_set_power() 的裸寄存器
 * 写法），eMMC 版本没有这个东西（eMMC 焊死供电不需要开关）。第一版漏了这步，
 * 导致卡槛没电、插着卡也检测不到（bm1684xSdhciCardPresent() 报
 * BM_SD_ENOCARD），见下面 bmSdPwrGpioInit()。
 *
 * 【上述修复仍不彻底，第二版补上】光改 GPIO 控制器自己的寄存器不够：对照
 * u-boot board/bitmain/bm1684/board.c 的 pinmux_config(PINMUX_SDIO) 才发现，
 * 这颗引脚归不归 GPIO 控制器管，还要先在芯片顶层 pad mux
 * （PINMUX_BASE+0x28=0x50010428，bit[5:4]）选一下；这一步没做的话，下面对
 * GPIO 控制器寄存器的读写大概率不会反映到物理引脚上。已在 bmSdPwrGpioInit()
 * 里补上这第一步，三步合并按 u-boot 真实顺序执行。
 ******************************************************************************/
#include "bm_sd_glue_cfg.h"
#ifdef BM1684X_SD

#include <string.h>
#include <stdio.h>
#include "bm_sd.h"
#include "bm1684xSdhciOsal.h"
#include "bm1684xSdhciHw.h"
#include "bm_sd_osal_os3.h"

#if defined(__aarch64__) || defined(__arm__)
#define BM_DSB()   __asm__ __volatile__ ("dsb sy" ::: "memory")
#else
#define BM_DSB()   __asm__ __volatile__ (""        ::: "memory")
#endif

/* 天脉3 平台提供的 DMA 缓冲区 cache 维护接口（已向用户确认函数名与参数），
 * 含义同 bm_emmc_core.c：写卡前 flush 发送缓冲区，读卡完成后 invalidate
 * 接收缓冲区。*/
extern void ACoreOs_cache_flush(void *addr, unsigned int len);
extern void ACoreOs_cache_invalidate(void *addr, unsigned int len);

/* 天脉3 平台提供的微秒级延时函数 */
extern void delay_us(unsigned int time_us);

/*--------------------------------------------------------------------------
 * SD 命令号（标准命令，不含厂商扩展；与 u-boot include/mmc.h 的
 * MMC_CMD_*/SD_CMD_* 数值逐一核对一致）
 *------------------------------------------------------------------------*/
#define SD_GO_IDLE_STATE          0    /* CMD0  */
#define SD_ALL_SEND_CID           2    /* CMD2  */
#define SD_SEND_RELATIVE_ADDR     3    /* CMD3  */
#define SD_APP_SET_BUS_WIDTH      6    /* ACMD6（须先发 CMD55）*/
#define SD_SELECT_CARD            7    /* CMD7  */
#define SD_SEND_IF_COND           8    /* CMD8  */
#define SD_SEND_CSD               9    /* CMD9  */
#define SD_SEND_STATUS            13   /* CMD13 */
#define SD_SET_BLOCKLEN           16   /* CMD16 */
#define SD_READ_SINGLE_BLOCK      17   /* CMD17 */
#define SD_READ_MULTIPLE_BLOCK    18   /* CMD18 */
#define SD_WRITE_BLOCK            24   /* CMD24 */
#define SD_WRITE_MULTIPLE_BLOCK   25   /* CMD25 */
#define SD_APP_CMD                55   /* CMD55，APP 命令前缀 */
#define SD_APP_SEND_OP_COND       41   /* ACMD41（须先发 CMD55）*/

/* OCR（CMD8/ACMD41 响应）相关位，数值与 u-boot include/mmc.h 一致 */
#define SD_OCR_BUSY               0x80000000U   /* 上电完成 */
#define SD_OCR_HCS                0x40000000U   /* 高容量卡（SDHC/SDXC） */
#define SD_OCR_VOLTAGE_WINDOW     0x00FF8000U   /* 主机支持电压窗口 2.7~3.6V */

/* CMD8 校验模式：bit8=主机支持 2.7~3.6V，低 8 位=校验码 0xAA（标准约定） */
#define SD_IF_COND_ARG            0x1AAU
#define SD_IF_COND_CHECK_PATTERN  0xAAU

/* 卡状态位（CMD13 响应），含义同 eMMC 版本 */
#define SD_STATUS_RDY_FOR_DATA    (1U << 8)
#define SD_STATUS_CURR_STATE_SH   9
#define SD_STATUS_CURR_STATE_MSK  0xFU
#define SD_STATE_PRG              7             /* 编程中 */

/*--------------------------------------------------------------------------
 * 内部状态
 *------------------------------------------------------------------------*/
static BM1684X_SDHCI_DEV *g_dev          = NULL;
static int  g_inited       = 0;
static u32  g_rca           = 0;       /* SD 卡自己上报的相对地址，CMD3 响应里取 */
static int  g_highCapacity = 0;       /* 1=SDHC/SDXC（块寻址） 0=SDSC（字节寻址） */
static u32  g_blockCount   = 0;       /* 用户区总扇区数（512B/扇区），由 CSD 解析得到 */

/*--------------------------------------------------------------------------
 * TOP 域时钟使能 + 软复位（SD 通道：BM1684X_CLK_AXI_SD/_SD_200M/_100K_SD，
 * 复位位 BM1684X_RST_SD）。原因与 eMMC 版本完全相同——BootROM 只在"eMMC 是
 * 引导介质"场景下替前级软件把时钟/复位做好，SD 通道同样没有人在更早期做过
 * 这一步，天脉3 不从 eMMC/SD 引导，所以本文件主动补上。
 *------------------------------------------------------------------------*/
static void bmSdTopDomainInit(void)
{
    volatile u32 *pClkEn = (volatile u32 *)(BM1684X_TOP_PHYS_BASE + BM1684X_TOP_CLOCK_EN0);
    volatile u32 *pRst   = (volatile u32 *)(BM1684X_TOP_PHYS_BASE + BM1684X_TOP_SOFT_RST0);

    *pClkEn |= (BM1684X_CLK_AXI_SD | BM1684X_CLK_SD_200M | BM1684X_CLK_100K_SD);
    BM_DSB();
    delay_us(10);

    *pRst &= ~BM1684X_RST_SD;
    BM_DSB();
    delay_us(1000);
    *pRst |= BM1684X_RST_SD;
    BM_DSB();
    delay_us(1000);
}

/*--------------------------------------------------------------------------
 * SD 卡槛供电开关 GPIO（SDIO_PWR_EN，对照 dts `sdhc@50101000` 节点的
 * `pwr-gpio = <&port1a 10 GPIO_ACTIVE_HIGH>;` 属性，端口基址即 u-boot
 * include/configs/bitmain_bm1684.h 里的 BM_PORTB_BASE=0x50027400）。
 *
 * 这是板级一个独立于 SDHCI 标准寄存器之外的负载开关：不驱动它，卡槛物理上
 * 就没有供电，插着卡也检测不到（之前漏了这一步，是 bm1684xSdhciCardPresent()
 * 报 BM_SD_ENOCARD 的真正原因）。u-boot 在三处分别处理，前两版只补了后两处，
 * 漏了第一处（管脚到底归不归 GPIO 控制器管的"门"），结果就是即便后面 GPIO
 * 寄存器全部按顺序写对了，物理引脚也可能压根没被路由到 GPIO 控制器上，写了
 * 等于没写：
 *   - 【这版新补的第一处】board/bitmain/bm1684/board.c pinmux_config()
 *     的 PINMUX_SDIO 分支：芯片顶层"引脚到底归哪个功能模块管"的选择
 *     （pad mux），地址是 PINMUX_BASE(=TOP_BASE+0x400=0x50010400)+0x28，
 *     bit[5:4] 写 0x1 ——选中"此引脚归 GPIO 管"，必须排在下面 GPIO controller
 *     自己的寄存器之前，否则 GPIO controller 的输出根本到不了芯片引脚上；
 *   - drivers/mmc/sdhci.c sdhci_init()：GPIO controller 内部选软件模式 +
 *     设为输出方向（+0x8 清 bit10，+0x4 置 bit10）；
 *   - drivers/mmc/sdhci.c sdhci_set_power()：实际上电时驱动高电平
 *     （+0x0 置 bit10）。
 * 本函数按"细节以 uboot 为准"原样合并这三步（天脉3 没有 DM_GPIO/DM_PINCTRL
 * 框架，直接按 u-boot 里裸寄存器写法照搬）。
 *------------------------------------------------------------------------*/
#define BM1684X_SD_PINMUX_BASE     0x50010400UL   /* TOP_BASE+0x400，u-boot PINMUX_BASE */
#define BM1684X_SD_PINMUX_OFF      0x28U          /* PINMUX_SDIO 分支写的偏移 */
#define BM1684X_SD_PINMUX_MASK     (0x3U << 4)    /* 该引脚功能选择字段，2 位 */
#define BM1684X_SD_PINMUX_GPIO_SEL (0x1U << 4)    /* 选中 GPIO 功能 */

#define BM1684X_SD_PWR_GPIO_BASE   0x50027400UL   /* port1a 控制器基址，u-boot BM_PORTB_BASE */
#define BM1684X_SD_PWR_GPIO_BIT    (1U << 10)      /* SDIO_PWR_EN，对应 GPIO42 */

static void bmSdPwrGpioInit(void)
{
    volatile u32 *pPinmux = (volatile u32 *)(BM1684X_SD_PINMUX_BASE + BM1684X_SD_PINMUX_OFF);
    volatile u32 *pSwMode = (volatile u32 *)(BM1684X_SD_PWR_GPIO_BASE + 0x8U);
    volatile u32 *pDir    = (volatile u32 *)(BM1684X_SD_PWR_GPIO_BASE + 0x4U);
    volatile u32 *pData   = (volatile u32 *)(BM1684X_SD_PWR_GPIO_BASE + 0x0U);

    /* 第一步（这版新补）：顶层 pad mux，把这个引脚的功能选成 GPIO（对照
     * board.c pinmux_config() 的 PINMUX_SDIO 分支）。不做这一步，下面对
     * GPIO 控制器寄存器的读写在硬件上可能根本不生效。 */
    *pPinmux = (*pPinmux & ~BM1684X_SD_PINMUX_MASK) | BM1684X_SD_PINMUX_GPIO_SEL;
    BM_DSB();

    /* 选软件模式（对照 u-boot sdhci_init() 里 +0x8 那行：清 bit10） */
    *pSwMode &= ~BM1684X_SD_PWR_GPIO_BIT;
    BM_DSB();

    /* 设为输出方向（对照 +0x4 那行：置 bit10） */
    *pDir |= BM1684X_SD_PWR_GPIO_BIT;
    BM_DSB();

    /* 驱动高电平给卡槛上电（对照 sdhci_set_power() 里 +0x0 那行：置 bit10） */
    *pData |= BM1684X_SD_PWR_GPIO_BIT;
    BM_DSB();
    delay_us(10000);   /* 给卡槛供电稳定留余量，求稳优先 */
}

/*--------------------------------------------------------------------------
 * 【临时排障用】命令发送的调试包装：上一版反馈"错误码 -1，定位到
 * bm1684xSdhciSendCmd 里返回的"，但该函数内部有两处都返回 -1（写命令寄存器
 * 前等 CMD_INHIBIT/DAT_INHIBIT 清零超时；写完命令后等 CMD_COMPLETE 超时），
 * 两种情况指向的硬件问题完全不同，且没说是哪条命令（CMD0/CMD8/ACMD41...）
 * 失败。引擎层 bm1684xSdhci.c 是 eMMC/SD 共用的公共文件，不在这里改它；改成
 * 在本文件每次调用前后读一把 PRESENT_STATE/INT_STATUS/ERR_INT_STATUS 原始
 * 寄存器值打出来，失败时就能直接从串口日志看出"卡在哪条命令、卡在哪一步"，
 * 不用再让用户自己去源码里对照行号。确认根因后会整段删除。*/
static int sdSendCmdDbg(const char *name, BM1684X_MMC_CMD *pCmd, BM1684X_MMC_DATA *pData)
{
    int ret = bm1684xSdhciSendCmd(g_dev, pCmd, pData);

    if (ret != 0)
    {
        volatile u32 *pState = (volatile u32 *)(BM1684X_SD_PHYS_BASE + SDHCI_PRESENT_STATE);
        volatile u16 *pIntSt = (volatile u16 *)(BM1684X_SD_PHYS_BASE + SDHCI_INT_STATUS);
        volatile u16 *pErrSt = (volatile u16 *)(BM1684X_SD_PHYS_BASE + SDHCI_ERR_INT_STATUS);
        volatile u16 *pClkCt = (volatile u16 *)(BM1684X_SD_PHYS_BASE + SDHCI_CLOCK_CONTROL);
        u32 state = *pState;
        u16 intSt = *pIntSt;
        u16 errSt = *pErrSt;
        u16 clkCt = *pClkCt;

        printf("[bm_sd] FAIL %s(CMD%u) ret=%d state=0x%08x int=0x%04x err=0x%04x"
               " clk=0x%04x resp0=0x%08x data=%s inhibit=%s%s clk:%s%s%s\r\n",
               name, (unsigned int)pCmd->cmdIdx, ret,
               (unsigned int)state, (unsigned int)intSt, (unsigned int)errSt,
               (unsigned int)clkCt, (unsigned int)pCmd->resp[0],
               pData ? (pData->flags & BM1684X_DATA_READ ? "READ" : "WRITE") : "-",
               (state & SDHCI_STATE_CMD_INHIBIT) ? "CMD," : "",
               (state & SDHCI_STATE_DAT_INHIBIT) ? "DAT," : "",
               (clkCt & SDHCI_CLK_INT_EN)     ? "INT_EN," : "INT_EN(0)!,",
               (clkCt & SDHCI_CLK_INT_STABLE) ? "STABLE," : "STABLE(0)!,",
               (clkCt & SDHCI_CLK_CARD_EN)    ? "CARD_EN" : "CARD_EN(0)!");
        /* 判断依据：
         *   - clk 那几位只要有任何一个打出 "(0)!"：说明发命令时 SDCLK 根本没有
         *     真正起来（内部时钟没使能/没稳定/没送到卡），这是比"卡没插好"更
         *     底层的问题——命令字节理论上都没法在总线上完整地发出去，需要回头
         *     查 bmSdTopDomainInit() 和 bm1684xSdhciSetClk() 的时钟源选择/分频。
         *   - clk 三位全部正常但仍然超时，分两种：
         *     - inhibit 仍带 CMD/DAT：命令寄存器都没机会写下去，卡在
         *       bm1684xSdhciSendCmd() 最前面那段"等 INHIBIT 清零"的超时；
         *     - inhibit 不带：命令已经写下去，但控制器既没报错也没报完成——
         *       多半是没有任何卡在线（总线悬空），控制器干等不到任何回应。
         *   - err 非 0：硬件已经识别出错误（如 bit0=Command Timeout
         *     Error，即卡确实没回应这条具体命令），这是最接近"卡有问题
         *     /协议参数不对"的信号，需要重点看是哪条 CMD/ACMD。
         *   - 带数据阶段（data=READ/WRITE）的命令专用判据：resp0 非 0（命令
         *     本身已经拿到 R1 响应）+ err=0 + inhibit 不带 DAT，说明 CMD 阶段
         *     成功，卡在了【数据阶段】（SDMA 一直没等到 XFER_COMPLETE）——
         *     和"命令本身没人理"是完全不同的两类问题，前者要查 DMA/缓冲区，
         *     后者要查总线/卡是否在线。resp0 仍是 0 则说明连命令的响应都没
         *     拿到，跟数据阶段无关。*/
    }
    return ret;
}

/*--------------------------------------------------------------------------
 * 等待卡回到可用状态（CMD13 轮询），逻辑与 eMMC 版本一致，SD/eMMC 共用同一套
 * 卡状态机定义（JEDEC/SD 协会两份规范这一段是一致的）。
 *------------------------------------------------------------------------*/
static int sdWaitReady(void)
{
    BM1684X_MMC_CMD cmd;
    int ret;
    u32 timeout = 1000000U;   /* 约 1s（每次 10us） */
    u32 state;

    for (;;)
    {
        memset(&cmd, 0, sizeof(cmd));
        cmd.cmdIdx   = SD_SEND_STATUS;
        cmd.cmdArg   = g_rca << 16;
        cmd.respType = BM1684X_RESP_R1;
        ret = sdSendCmdDbg("CMD13", &cmd, NULL);
        if (ret != 0)
            return BM_SD_EIO;

        state = (cmd.resp[0] >> SD_STATUS_CURR_STATE_SH) & SD_STATUS_CURR_STATE_MSK;
        if ((cmd.resp[0] & SD_STATUS_RDY_FOR_DATA) && (state != SD_STATE_PRG))
            return BM_SD_OK;

        if (timeout-- == 0U)
            return BM_SD_ETIMEOUT;
        delay_us(10);
    }
}

/*--------------------------------------------------------------------------
 * 发送一条 APP 命令（先 CMD55 再发实际的 ACMDxx），对应 u-boot mmc.c 里
 * sd_send_op_cond()/sd_select_bus_width() 开头都重复的那段 CMD55 前缀逻辑，
 * 这里抽成一个小函数避免重复。
 *------------------------------------------------------------------------*/
static int sdSendAppCmd(u32 acmdIdx, u32 acmdArg, u32 respType, BM1684X_MMC_CMD *pAcmdOut)
{
    BM1684X_MMC_CMD cmd;
    int ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx = SD_APP_CMD; cmd.cmdArg = g_rca << 16; cmd.respType = BM1684X_RESP_R1;
    ret = sdSendCmdDbg("CMD55", &cmd, NULL);
    if (ret != 0)
        return BM_SD_EIO;

    memset(pAcmdOut, 0, sizeof(*pAcmdOut));
    pAcmdOut->cmdIdx = acmdIdx; pAcmdOut->cmdArg = acmdArg; pAcmdOut->respType = respType;
    ret = sdSendCmdDbg("ACMD", pAcmdOut, NULL);
    if (ret != 0)
        return BM_SD_EIO;

    return BM_SD_OK;
}

/*--------------------------------------------------------------------------
 * LBA -> 命令参数（块寻址/字节寻址自适应，取决于 CMD8/ACMD41 探测到的卡容量
 * 类型，SDHC/SDXC 用块寻址，老的 SDSC 用字节寻址——这是 SD 协议本身的规则，
 * 不是本项目自创，对照 u-boot mmc_read_blocks()/mmc_write_blocks() 里
 * mmc->high_capacity 的用法）
 *------------------------------------------------------------------------*/
static u32 lbaToArg(u32 lba)
{
    return g_highCapacity ? lba : (lba * BM_SD_BLOCK_SIZE);
}

/*--------------------------------------------------------------------------
 * 初始化：控制器（含 PHY/时钟）+ SD 卡识别
 *------------------------------------------------------------------------*/
int bm_sd_init(void)
{
    BM1684X_MMC_CMD  cmd, acmd;
    int ret;
    int sdVersion2;
    u32 timeout;
    u32 csd[4];

    if (g_inited)
        return BM_SD_OK;

    /* 先把 TOP 域 SD 时钟/复位打开，再让引擎层去碰控制器寄存器 */
    bmSdTopDomainInit();

    /* 给卡槛供电（SDIO_PWR_EN GPIO）。必须在引擎初始化/卡检测之前做，否则
     * 卡槛没电，bm1684xSdhciCardPresent() 即使插着卡也会返回"不在位"。*/
    bmSdPwrGpioInit();

    /* 引擎初始化：内部完成 14 步 PHY 时序 + 控制器复位上电 + 识别时钟（200kHz）。
     * devIndex 传 BM1684X_SD_INDEX(=1)，base 传 SD 控制器基址 0x50101000——
     * 这两个参数会让引擎层 phyInit()/CardPresent() 走 SD 专属分支
     * （对照 u-boot bm_sdhci_phy_init() 里 host->index==1 的 SMPLDL BYPASS_EN
     * 分支，已在 bm1684xSdhci.c 里实现，本文件无需关心细节）。*/
    g_dev = bm1684xSdhciInit(&g_bm1684xOsalOs3Sd, BM1684X_SD_PHYS_BASE,
                              0U, BM1684X_SD_INDEX, BM_SD_USE_64BIT_DMA);
    if (g_dev == NULL)
        return BM_SD_EIO;

    /* 卡检测（软检测）：
     * 本应在此确认卡在位再继续。但上板实测发现：明明插着卡，SDHCI 的"卡在位"
     * 状态位（Present State bit16）始终读不到 1，导致这里直接返回 init fail -5。
     * 可能原因：本板的卡检测(CD#)信号没接到控制器、或控制器全复位后该位的消抖
     * 还没完成、或该引脚复用未到位。既然卡是确实插着的，硬卡在这一位上没有意义。
     *
     * 因此改成"软检测"：先轮询一小段时间给消抖留机会；若仍读不到，打印一条告警
     * 后【继续往下走】识别流程，不再直接返回 ENOCARD。这样做是安全的——引擎层
     * 发命令有 1s 超时保护，真要是没卡/没电，后续 CMD55/ACMD41 会超时返回别的
     * 错误码（比死活停在 -5 更能定位真正的卡点）。*/
    {
        int present = 0;
        u32 i;
        for (i = 0; i < 200U; i++) {          /* 最多约 1s，每次 5ms */
            if (bm1684xSdhciCardPresent(g_dev)) { present = 1; break; }
            delay_us(5000);
        }
        if (!present)
            printf("[bm_sd] WARN: 卡在位状态位读不到(可能CD线未接/消抖未完成)，"
                   "仍继续初始化；若实际无卡，后续命令会超时报错\r\n");
    }

    /* 置位 EMMC_CTRL_R 的 bit0（厂商区 +0x2C）。命名是 CARD_IS_EMMC，但对照
     * u-boot sdhci-bitmain.c 的 bm_sdhci_probe()：这一位是【不分 index、对
     * 任何使用本驱动的设备都无条件置位】的，SD/SDIO 通道（host->index==1）
     * 也一样设置——这不是笔误，已逐行核对 u-boot 源码确认。本文件按用户
     * "具体细节以 uboot 为准"的要求，照样替 SD 通道补上这一位。*/
    {
        volatile u16 *pVendorPtr = (volatile u16 *)(BM1684X_SD_PHYS_BASE + SDHCI_VENDOR_SPECIFIC_AREA);
        u16 vendorOff = (u16)(*pVendorPtr & 0x0FFFU);
        volatile u16 *pEmmcCtrl = (volatile u16 *)(BM1684X_SD_PHYS_BASE + vendorOff + SDHCI_EMMC_CTRL_R_OFF);
        *pEmmcCtrl |= 0x1U;
    }

    bm1684xSdhciSetBusWidth(g_dev, 1U);
    delay_us(2000);

    /* CMD0：复位到 idle，对应 u-boot mmc_go_idle() */
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx = SD_GO_IDLE_STATE; cmd.cmdArg = 0; cmd.respType = BM1684X_RESP_NONE;
    ret = sdSendCmdDbg("CMD0", &cmd, NULL);
    if (ret != 0)
        return BM_SD_EIO;
    delay_us(2000);

    /* CMD8：SEND_IF_COND，对应 u-boot mmc_send_if_cond()。成功且校验码回显
     * 正确 -> SD 2.0+卡（后续 ACMD41 要带 HCS 位）；超时/出错 -> 当作老的
     * SD 1.x 卡处理（不带 HCS，走字节寻址），不是阻塞性错误，继续往下走。*/
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx = SD_SEND_IF_COND; cmd.cmdArg = SD_IF_COND_ARG; cmd.respType = BM1684X_RESP_R7;
    ret = sdSendCmdDbg("CMD8", &cmd, NULL);
    sdVersion2 = (ret == 0) && ((cmd.resp[0] & 0xFFU) == SD_IF_COND_CHECK_PATTERN);

    /* CMD55+ACMD41：轮询 OCR busy 位，对应 u-boot sd_send_op_cond()。
     * 参数 = 主机电压窗口 | (SD2.0+卡 ? HCS : 0)。*/
    timeout = 1000U;   /* 最多约 2s */
    for (;;)
    {
        ret = sdSendAppCmd(SD_APP_SEND_OP_COND,
                            SD_OCR_VOLTAGE_WINDOW | (sdVersion2 ? SD_OCR_HCS : 0U),
                            BM1684X_RESP_R3, &acmd);
        if (ret != BM_SD_OK)
            return ret;
        if (acmd.resp[0] & SD_OCR_BUSY)
            break;
        if (timeout-- == 0U)
            return BM_SD_ETIMEOUT;
        delay_us(2000);
    }
    g_highCapacity = (acmd.resp[0] & SD_OCR_HCS) ? 1 : 0;

    /* CMD2：读 CID（进入识别态），对应 u-boot mmc_startup() 里
     * MMC_CMD_ALL_SEND_CID 那一段 */
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx = SD_ALL_SEND_CID; cmd.cmdArg = 0; cmd.respType = BM1684X_RESP_R2;
    ret = sdSendCmdDbg("CMD2", &cmd, NULL);
    if (ret != 0)
        return BM_SD_EIO;

    /* CMD3：SD 卡自己上报 RCA（不是主机指定），对应 u-boot mmc_startup() 里
     * SD_CMD_SEND_RELATIVE_ADDR 分支：response[0]>>16 取 RCA */
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx = SD_SEND_RELATIVE_ADDR; cmd.cmdArg = 0; cmd.respType = BM1684X_RESP_R6;
    ret = sdSendCmdDbg("CMD3", &cmd, NULL);
    if (ret != 0)
        return BM_SD_EIO;
    g_rca = (cmd.resp[0] >> 16) & 0xFFFFU;

    /* CMD9：读 CSD，对应 u-boot mmc_startup() 取容量的算法（SD 没有
     * EXT_CSD，容量必须从 CSD 解出来，这是与 eMMC 最大的不同点之一）：
     *   高容量(HCS)：csize = csd[1][5:0]<<16 | csd[2][31:16]，cmult 固定为 8
     *   低容量(非HCS)：csize = csd[1][9:0]<<2 | csd[2][31:30]，
     *                  cmult = csd[2][17:15]
     *   容量(字节) = (csize+1) << (cmult+2) * 块长(read_bl_len)
     * 块长固定按 512 处理（绝大多数现代 SD 卡 READ_BL_LEN=9 即 512B，
     * 先求稳不读 CSD 里的 READ_BL_LEN 字段做特殊适配）。*/
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx = SD_SEND_CSD; cmd.cmdArg = g_rca << 16; cmd.respType = BM1684X_RESP_R2;
    ret = sdSendCmdDbg("CMD9", &cmd, NULL);
    if (ret != 0)
        return BM_SD_EIO;
    csd[0] = cmd.resp[0]; csd[1] = cmd.resp[1]; csd[2] = cmd.resp[2]; csd[3] = cmd.resp[3];

    {
        u64 csize, cmult, capacityBytes;

        if (g_highCapacity)
        {
            csize = ((csd[1] & 0x3FU) << 16) | ((csd[2] & 0xFFFF0000U) >> 16);
            cmult = 8U;
        }
        else
        {
            csize = ((csd[1] & 0x3FFU) << 2) | ((csd[2] & 0xC0000000U) >> 30);
            cmult = (csd[2] & 0x00038000U) >> 15;
        }
        capacityBytes = (csize + 1U) << (cmult + 2U);
        capacityBytes *= BM_SD_BLOCK_SIZE;
        g_blockCount  = (u32)(capacityBytes / BM_SD_BLOCK_SIZE);
    }

    /* CMD7：选中卡，进入传输态 */
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx = SD_SELECT_CARD; cmd.cmdArg = g_rca << 16; cmd.respType = BM1684X_RESP_R1B;
    ret = sdSendCmdDbg("CMD7", &cmd, NULL);
    if (ret != 0)
        return BM_SD_EIO;

    /* CMD55+ACMD6：切到 4 位总线（卡侧），对应 u-boot sd_select_bus_width()。
     * cmdarg：4 位=2，1 位=0（标准 SD 总线宽度命令编码，与 eMMC EXT_CSD 字段
     * 编码无关，是 SD 协议自己的定义）。再切控制器侧寄存器，与 eMMC 版本一致，
     * 目标时钟 25MHz 低于 SD 默认速度档上限，不需要切高速档/调 DLL，
     * PHY 初始化阶段已是旁路状态，求稳优先。*/
#if (BM_SD_BUS_WIDTH == 4)
    ret = sdSendAppCmd(SD_APP_SET_BUS_WIDTH, 2U, BM1684X_RESP_R1, &acmd);
    if (ret == BM_SD_OK)
        bm1684xSdhciSetBusWidth(g_dev, 4U);
#else
    bm1684xSdhciSetBusWidth(g_dev, 1U);
#endif

    /* 切到工作时钟（默认 25MHz 安全档） */
    ret = bm1684xSdhciSetClk(g_dev, BM_SD_TRAN_CLK_HZ);
    if (ret != 0)
        return BM_SD_EIO;

    /* CMD16：设块长 512（块寻址下无副作用，字节寻址的老卡则是必需步骤） */
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx = SD_SET_BLOCKLEN; cmd.cmdArg = BM_SD_BLOCK_SIZE; cmd.respType = BM1684X_RESP_R1;
    (void)sdSendCmdDbg("CMD16", &cmd, NULL);

    g_inited = 1;
    return BM_SD_OK;
}

int bm_sd_reinit(void)
{
    g_inited     = 0;
    g_blockCount = 0;
    g_rca        = 0;
    return bm_sd_init();
}

/*--------------------------------------------------------------------------
 * 读扇区
 *------------------------------------------------------------------------*/
int bm_sd_read_blocks(u32 lba, u32 count, void *buf)
{
    BM1684X_MMC_CMD  cmd;
    BM1684X_MMC_DATA data;
    int ret;

    if (!g_inited)
    {
        ret = bm_sd_init();
        if (ret != BM_SD_OK)
            return ret;
    }
    if ((count == 0U) || (buf == NULL))
        return BM_SD_EPARAM;

    memset(&data, 0, sizeof(data));
    data.buf = buf; data.blkSize = BM_SD_BLOCK_SIZE; data.blkCount = count;
    data.flags = BM1684X_DATA_READ;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx   = (count > 1U) ? SD_READ_MULTIPLE_BLOCK : SD_READ_SINGLE_BLOCK;
    cmd.cmdArg   = lbaToArg(lba);
    cmd.respType = BM1684X_RESP_R1;

    /* 改走 sdSendCmdDbg 包装：之前这里直接调引擎层，读失败时啥也不打印，
     * 排不出是命令阶段还是数据阶段卡住的；现在和命令类排障走同一套日志。*/
    ret = sdSendCmdDbg("READ", &cmd, &data);
    if (ret != 0)
        return BM_SD_EIO;

    /* DMA 完成后先 dsb 排序，再 invalidate 接收缓冲区，强制 CPU 重新从内存
     * 读取 DMA 刚写入的数据，避免读到缓存里的旧值（与 eMMC 版本相同处理）。*/
    BM_DSB();
    ACoreOs_cache_invalidate(buf, count * BM_SD_BLOCK_SIZE);
    return BM_SD_OK;
}

/*--------------------------------------------------------------------------
 * 写扇区
 *------------------------------------------------------------------------*/
int bm_sd_write_blocks(u32 lba, u32 count, const void *buf)
{
    BM1684X_MMC_CMD  cmd;
    BM1684X_MMC_DATA data;
    int ret;

    if (!g_inited)
    {
        ret = bm_sd_init();
        if (ret != BM_SD_OK)
            return ret;
    }
    if ((count == 0U) || (buf == NULL))
        return BM_SD_EPARAM;

    /* 写卡前 flush 发送缓冲区，把 CPU 缓存里的最新数据刷到内存，DMA 才能
     * 读到正确内容；flush 后补一道 dsb，保证这件事排在引擎层启动 DMA 的
     * 寄存器写之前完成（与 eMMC 版本相同处理）。*/
    ACoreOs_cache_flush((void *)buf, count * BM_SD_BLOCK_SIZE);
    BM_DSB();

    memset(&data, 0, sizeof(data));
    data.buf = (void *)buf; data.blkSize = BM_SD_BLOCK_SIZE; data.blkCount = count;
    data.flags = BM1684X_DATA_WRITE;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx   = (count > 1U) ? SD_WRITE_MULTIPLE_BLOCK : SD_WRITE_BLOCK;
    cmd.cmdArg   = lbaToArg(lba);
    cmd.respType = BM1684X_RESP_R1;

    ret = sdSendCmdDbg("WRITE", &cmd, &data);
    if (ret != 0)
        return BM_SD_EIO;

    /* 等卡内部编程完成，确保数据落盘 */
    return sdWaitReady();
}

u32 bm_sd_get_block_count(void)
{
    return g_blockCount;
}

#endif /* BM1684X_SD */
