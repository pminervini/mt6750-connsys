/*! \file
    \brief  Declaration of library functions

    Any definitions in this file will be shared among GLUE Layer and internal Driver Stack.
*/

/*******************************************************************************
*                         C O M P I L E R   F L A G S
********************************************************************************
*/

/*******************************************************************************
*                                 M A C R O S
********************************************************************************
*/

#ifdef DFT_TAG
#undef DFT_TAG
#endif
#define DFT_TAG "[WMT-CONSYS-HW]"

/*******************************************************************************
*                    E X T E R N A L   R E F E R E N C E S
********************************************************************************
*/
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/memblock.h>
#include "osal_typedef.h"
#include "mtk_wcn_consys_hw.h"
#include <linux/regmap.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#if CONSYS_EMI_MPU_SETTING
#endif

#include <linux/regulator/consumer.h>
#ifdef CONFIG_MTK_HIBERNATION
#endif

#include <linux/of_reserved_mem.h>

#if CONSYS_CLOCK_BUF_CTRL
#endif

#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/iopoll.h>

/* MT6353 PMIC access, serialised with the charger: drivers/soc/mediatek/mt6755-pmic.c */
extern int mt6755_pmic_rmw(u16 adr, u16 mask, int shift, u16 val);
extern int mt6755_pmic_read(u16 adr, u16 *val);

/* MT6353 LDO fields (mt6353_hw.h) */
#define MT6353_VCN18_CON0	0x0A4C	/* b1 EN, b3 EN_CTRL (0 = SW), b15 QI_EN */
#define MT6353_VCN28_CON0	0x0A48	/* b1 EN, b3 EN_CTRL */
#define MT6353_VCN33_CON0_BT	0x0A0E	/* b1 EN_BT, b3 EN_CTRL_BT (1 = HW) */
#define MT6353_VCN33_CON1_WIFI	0x0A10	/* b1 EN_WIFI, b3 EN_CTRL_WIFI */
#define MT6353_VCN28_VOSEL	0x0A9C	/* [9:8]: 3 = 2.8 V */
#define MT6353_VCN33_VOSEL	0x0AAE	/* [10:8]: 0 = 3.3 V */

/*******************************************************************************
*                              C O N S T A N T S
********************************************************************************
*/

/*******************************************************************************
*                             D A T A   T Y P E S
********************************************************************************
*/

/*******************************************************************************
*                  F U N C T I O N   D E C L A R A T I O N S
********************************************************************************
*/
static INT32 mtk_wmt_probe(struct platform_device *pdev);
static void mtk_wmt_remove(struct platform_device *pdev);


/*******************************************************************************
*                            P U B L I C   D A T A
********************************************************************************
*/

struct CONSYS_BASE_ADDRESS conn_reg;
static phys_addr_t gConEmiPhyBase;
static UINT8 __iomem *pEmibaseaddr;
static struct clk *clk_infra_conn_main;	/*ctrl infra_connmcu_bus clk */
static struct platform_device *my_pdev;
static struct reset_control *rstc;
static struct regulator *reg_VCN18;
static struct regulator *reg_VCN28;
static struct regulator *reg_VCN33_BT;
static struct regulator *reg_VCN33_WIFI;
static struct pinctrl *consys_pinctrl;
static struct pinctrl *mt6625_spi_pinctrl;
static struct pinctrl_state *mt6625_spi_default;
static struct regmap *pmic_regmap;
#define DYNAMIC_DUMP_GROUP_NUM 5

static const struct of_device_id apwmt_of_ids[] = {
	{.compatible = "mediatek,mt6755-consys",},
	{}
};
MODULE_DEVICE_TABLE(of, apwmt_of_ids);

static struct platform_driver mtk_wmt_dev_drv = {
	.probe = mtk_wmt_probe,
	.remove = mtk_wmt_remove,
	.driver = {
		   .name = "mt6755consys",
		   .owner = THIS_MODULE,
		   .of_match_table = of_match_ptr(apwmt_of_ids),
		   },
};

static INT32 mtk_wmt_probe(struct platform_device *pdev)
{
	/*
	 * MT6755: power domain, reset and LDOs are driven directly below
	 * (SPM CONN_PWR_CON, TOPRGU, MT6353 via mt6755_pmic_*); CONN pins are
	 * dedicated balls. Nothing to look up here.
	 */
	my_pdev = pdev;
	consys_pinctrl = NULL;
	return 0;
}

static void mtk_wmt_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);
}

#define SPM(o)	(conn_reg.spm_base + (o))
#define INFRA(o)	(conn_reg.topckgen_base + (o))
#define CONN_PWR_ON		(0x1 << 2)
#define CONN_PWR_ON_2ND		(0x1 << 3)
#define CONN_CLK_DIS		(0x1 << 4)
#define CONN_ISO		(0x1 << 1)
#define CONN_RST_B		(0x1 << 0)

static int conn_pwr_sta(void)
{
	return (CONSYS_REG_READ(SPM(CONSYS_PWR_STATUS_OFFSET)) & CONSYS_PWR_STA_CONN_BIT) &&
	       (CONSYS_REG_READ(SPM(CONSYS_PWR_STATUS_2ND_OFFSET)) & CONSYS_PWR_STA_CONN_BIT);
}

/* stock clk-mt6755-pg.c spm_mtcmos_ctrl_connsys(STA_POWER_ON) */
VOID mtk_wcn_consys_power_on(VOID)
{
	UINT32 i;

	CONSYS_REG_WRITE(SPM(CONSYS_PWRON_CONFG_EN_OFFSET), CONSYS_PWRON_CONFG_EN_VALUE);
	CONSYS_SET_BIT(SPM(CONSYS_CONN_PWR_CON_OFFSET), CONN_PWR_ON);
	CONSYS_SET_BIT(SPM(CONSYS_CONN_PWR_CON_OFFSET), CONN_PWR_ON_2ND);
	for (i = 0; i < 1000 && !conn_pwr_sta(); i++)
		udelay(10);
	if (!conn_pwr_sta())
		WMT_PLAT_ERR_FUNC("CONN power ack timeout (pwr_con 0x%08x sta 0x%08x/0x%08x)\n",
				  CONSYS_REG_READ(SPM(CONSYS_CONN_PWR_CON_OFFSET)),
				  CONSYS_REG_READ(SPM(CONSYS_PWR_STATUS_OFFSET)),
				  CONSYS_REG_READ(SPM(CONSYS_PWR_STATUS_2ND_OFFSET)));
	CONSYS_CLR_BIT(SPM(CONSYS_CONN_PWR_CON_OFFSET), CONN_CLK_DIS);
	CONSYS_CLR_BIT(SPM(CONSYS_CONN_PWR_CON_OFFSET), CONN_ISO);
	CONSYS_SET_BIT(SPM(CONSYS_CONN_PWR_CON_OFFSET), CONN_RST_B);
	/* release the INFRA bus protection for CONN */
	CONSYS_CLR_BIT(INFRA(CONSYS_TOPAXI_PROT_EN_OFFSET), CONSYS_TOPAXI_PROT_CONN_BITS);
	for (i = 0; i < 4000 &&
	     (CONSYS_REG_READ(INFRA(CONSYS_TOPAXI_PROT_STA1_OFFSET)) & CONSYS_TOPAXI_PROT_CONN_BITS); i++)
		udelay(1);
	WMT_PLAT_INFO_FUNC("CONN MTCMOS on: pwr_con 0x%08x prot_sta 0x%08x\n",
			   CONSYS_REG_READ(SPM(CONSYS_CONN_PWR_CON_OFFSET)),
			   CONSYS_REG_READ(INFRA(CONSYS_TOPAXI_PROT_STA1_OFFSET)));
}

VOID mtk_wcn_consys_power_off(VOID)
{
	UINT32 i;

	CONSYS_SET_BIT(INFRA(CONSYS_TOPAXI_PROT_EN_OFFSET), CONSYS_TOPAXI_PROT_CONN_BITS);
	for (i = 0; i < 4000 &&
	     (CONSYS_REG_READ(INFRA(CONSYS_TOPAXI_PROT_STA1_OFFSET)) & CONSYS_TOPAXI_PROT_CONN_BITS) !=
	     CONSYS_TOPAXI_PROT_CONN_BITS; i++)
		udelay(1);
	CONSYS_REG_WRITE(SPM(CONSYS_PWRON_CONFG_EN_OFFSET), CONSYS_PWRON_CONFG_EN_VALUE);
	CONSYS_SET_BIT(SPM(CONSYS_CONN_PWR_CON_OFFSET), CONN_ISO);
	CONSYS_SET_BIT(SPM(CONSYS_CONN_PWR_CON_OFFSET), CONN_CLK_DIS);
	CONSYS_CLR_BIT(SPM(CONSYS_CONN_PWR_CON_OFFSET), CONN_RST_B);
	CONSYS_CLR_BIT(SPM(CONSYS_CONN_PWR_CON_OFFSET), CONN_PWR_ON);
	CONSYS_CLR_BIT(SPM(CONSYS_CONN_PWR_CON_OFFSET), CONN_PWR_ON_2ND);
	for (i = 0; i < 1000 && conn_pwr_sta(); i++)
		udelay(10);
	WMT_PLAT_INFO_FUNC("CONN MTCMOS off: pwr_con 0x%08x\n",
			   CONSYS_REG_READ(SPM(CONSYS_CONN_PWR_CON_OFFSET)));
}

static void consys_cpu_reset(int assert)
{
	UINT32 v = CONSYS_REG_READ(conn_reg.ap_rgu_base + CONSYS_CPU_SW_RST_OFFSET);

	v = assert ? (v | (0x1 << 12)) : (v & ~(0x1 << 12));
	CONSYS_REG_WRITE(conn_reg.ap_rgu_base + CONSYS_CPU_SW_RST_OFFSET, (v & 0x00FFFFFF) | 0x88000000);
}

static unsigned int vcn28_settle_us = 2000;
module_param(vcn28_settle_us, uint, 0644);

static void vcn28_set(int on)
{
	mt6755_pmic_rmw(MT6353_VCN28_CON0, 1, 3, 0);		/* SW mode */
	if (on)
		mt6755_pmic_rmw(MT6353_VCN28_VOSEL, 0x3, 8, 3);	/* 2.8 V */
	mt6755_pmic_rmw(MT6353_VCN28_CON0, 1, 1, on ? 1 : 0);
	if (on)
		usleep_range(vcn28_settle_us, vcn28_settle_us + 100);	/* stock: regulator ramp delay */
}

/* stock common_main/platform/mt6755.c + mtk_wcn_consys_hw.c, co-clock (WMT_SOC.cfg co_clock_flag=1) */
INT32 mtk_wcn_consys_hw_reg_ctrl(UINT32 on, UINT32 co_clock_type)
{
	UINT32 retry = 10;
	UINT32 consysHwChipId = 0;
	u16 v18 = 0;

	WMT_PLAT_INFO_FUNC("CONSYS-HW-REG-CTRL(%d), co_clock %d\n", on, co_clock_type);
	if (on) {
		/* 1. VCN18 (MT6625L digital IO) on, SW mode */
		mt6755_pmic_rmw(MT6353_VCN18_CON0, 1, 3, 0);
		mt6755_pmic_rmw(MT6353_VCN18_CON0, 1, 1, 1);
		udelay(150);
		/* 2. VCN28: also needed in co-clock mode (efuse); XO buffer is PMIC-wrapper driven */
		vcn28_set(1);
		mt6755_pmic_read(MT6353_VCN18_CON0, &v18);
		WMT_PLAT_INFO_FUNC("VCN18 con0 0x%04x\n", v18);
		/* 3. assert CONNSYS CPU reset (TOPRGU 0x18 bit 12, key 0x88) */
		consys_cpu_reset(1);
		/* 4. CONN MTCMOS on + bus protection off */
		mtk_wcn_consys_power_on();
		udelay(10);
		/* 5. chip id */
		while (retry-- > 0) {
			consysHwChipId = CONSYS_REG_READ(conn_reg.mcu_base + CONSYS_CHIP_ID_OFFSET);
			if (consysHwChipId == CONSYS_CHIP_ID_MT6755)
				break;
			msleep(20);
		}
		WMT_PLAT_INFO_FUNC("consys chipId 0x%08x (%s)\n", consysHwChipId,
				   consysHwChipId == CONSYS_CHIP_ID_MT6755 ? "ok" : "UNEXPECTED");
		/* 6. MBIST at real speed */
		CONSYS_SET_BIT(conn_reg.mcu_base + CONSYS_MCU_CFG_ACR_OFFSET, CONSYS_MCU_CFG_ACR_MBIST_BIT);
		/* 7. release CONNSYS CPU */
		consys_cpu_reset(0);
		msleep(20);
	} else {
		mtk_wcn_consys_power_off();
		vcn28_set(0);
		mt6755_pmic_rmw(MT6353_VCN18_CON0, 1, 1, 0);
	}
	WMT_PLAT_DBG_FUNC("CONSYS-HW-REG-CTRL(0x%08x),finish\n", on);
	return 0;
}

INT32 mtk_wcn_consys_hw_gpio_ctrl(UINT32 on)
{
	INT32 iRet = 0;

	WMT_PLAT_DBG_FUNC("CONSYS-HW-GPIO-CTRL(0x%08x), start\n", on);

	if (on) {

		/* TODO: [FixMe][GeorgeKuo] double check if BGF_INT is implemented ok */
		/* iRet += wmt_plat_gpio_ctrl(PIN_BGF_EINT, PIN_STA_MUX); */
		iRet += wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_INIT);
		iRet += wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_EINT_DIS);
		WMT_PLAT_DBG_FUNC("CONSYS-HW, BGF IRQ registered and disabled\n");

	} else {

		/* set bgf eint/all eint to deinit state, namely input low state */
		iRet += wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_EINT_DIS);
		iRet += wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_DEINIT);
		WMT_PLAT_DBG_FUNC("CONSYS-HW, BGF IRQ unregistered and disabled\n");
		/* iRet += wmt_plat_gpio_ctrl(PIN_BGF_EINT, PIN_STA_DEINIT); */
	}
	WMT_PLAT_DBG_FUNC("CONSYS-HW-GPIO-CTRL(0x%08x), finish\n", on);
	return iRet;

}

INT32 mtk_wcn_consys_hw_pwr_on(UINT32 co_clock_type)
{
	INT32 iRet = 0;

	WMT_PLAT_INFO_FUNC("CONSYS-HW-PWR-ON, start\n");

	iRet += mtk_wcn_consys_hw_reg_ctrl(1, co_clock_type);
	iRet += mtk_wcn_consys_hw_gpio_ctrl(1);

	WMT_PLAT_INFO_FUNC("CONSYS-HW-PWR-ON, finish(%d)\n", iRet);
	return iRet;
}

INT32 mtk_wcn_consys_hw_pwr_off(VOID)
{
	INT32 iRet = 0;

	WMT_PLAT_INFO_FUNC("CONSYS-HW-PWR-OFF, start\n");

	iRet += mtk_wcn_consys_hw_reg_ctrl(0, 0);
	iRet += mtk_wcn_consys_hw_gpio_ctrl(0);

	WMT_PLAT_INFO_FUNC("CONSYS-HW-PWR-OFF, finish(%d)\n", iRet);
	return iRet;
}

INT32 mtk_wcn_consys_hw_rst(UINT32 co_clock_type)
{
	INT32 iRet = 0;

	WMT_PLAT_INFO_FUNC("CONSYS-HW, hw_rst start, eirq should be disabled before this step\n");

	/*1. do whole hw power off flow */
	iRet += mtk_wcn_consys_hw_reg_ctrl(0, co_clock_type);

	/*2. do whole hw power on flow */
	iRet += mtk_wcn_consys_hw_reg_ctrl(1, co_clock_type);

	WMT_PLAT_INFO_FUNC("CONSYS-HW, hw_rst finish, eirq should be enabled after this step\n");
	return iRet;
}

/* MT6353 VCN33: enable in SW, then hand over to CONSYS (HW mode), as stock mt6755.c */
static void vcn33_ctrl(u16 reg, UINT32 enable)
{
	if (enable) {
		mt6755_pmic_rmw(MT6353_VCN33_VOSEL, 0x7, 8, 0);	/* 3.3 V */
		mt6755_pmic_rmw(reg, 1, 1, 1);
		mt6755_pmic_rmw(reg, 1, 3, 1);
	} else {
		mt6755_pmic_rmw(reg, 1, 3, 0);
		mt6755_pmic_rmw(reg, 1, 1, 0);
	}
}

INT32 mtk_wcn_consys_hw_bt_paldo_ctrl(UINT32 enable)
{
	vcn33_ctrl(MT6353_VCN33_CON0_BT, enable);
	WMT_PLAT_INFO_FUNC("WMT do BT PMIC %s\n", enable ? "on" : "off");
	return 0;
}

INT32 mtk_wcn_consys_hw_wifi_paldo_ctrl(UINT32 enable)
{
	vcn33_ctrl(MT6353_VCN33_CON1_WIFI, enable);
	WMT_PLAT_INFO_FUNC("WMT do WIFI PMIC %s\n", enable ? "on" : "off");
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_wifi_paldo_ctrl);

INT32 mtk_wcn_consys_hw_vcn28_ctrl(UINT32 enable)
{
	vcn28_set(enable);
	WMT_PLAT_INFO_FUNC("vcn28 %s (fm/gps, co-clock)\n", enable ? "on" : "off");
	return 0;
}

INT32 mtk_wcn_consys_hw_state_show(VOID)
{
	return 0;
}

INT32 mtk_wcn_consys_hw_restore(struct device *device)
{
	return 0;
}

/*
 * EMI: 1 MB "mediatek,consys-reserve-memory" region (memory-region phandle of
 * the consys node). CONSYS sees it at 0xF0000000 once the INFRA remap register
 * points at it; WMT uses the upper half (+0x80000) for control/coredump.
 */
INT32 mtk_wcn_consys_hw_init(void)
{
	INT32 iRet = -1;
	struct device_node *node, *mem;
	struct reserved_mem *rmem = NULL;
	UINT32 v;

	node = of_find_compatible_node(NULL, NULL, "mediatek,mt6755-consys");
	if (!node) {
		WMT_PLAT_ERR_FUNC("[%s] can't find CONSYS compatible node\n", __func__);
		return iRet;
	}
	conn_reg.mcu_base = (SIZE_T) of_iomap(node, 0);
	conn_reg.ap_rgu_base = (SIZE_T) of_iomap(node, 1);
	conn_reg.topckgen_base = (SIZE_T) of_iomap(node, 2);
	conn_reg.spm_base = (SIZE_T) of_iomap(node, 3);
	if (!conn_reg.mcu_base || !conn_reg.ap_rgu_base || !conn_reg.topckgen_base || !conn_reg.spm_base) {
		WMT_PLAT_ERR_FUNC("consys reg mapping failed\n");
		return iRet;
	}
	mem = of_parse_phandle(node, "memory-region", 0);
	if (mem)
		rmem = of_reserved_mem_lookup(mem);
	if (!rmem || rmem->size < SZ_1M) {
		WMT_PLAT_ERR_FUNC("no consys reserved memory (memory-region)\n");
		return iRet;
	}
	gConEmiPhyBase = rmem->base;
	v = CONSYS_REG_READ(conn_reg.topckgen_base + CONSYS_EMI_REMAP_OFFSET);
	WMT_PLAT_INFO_FUNC("EMI remap reg was 0x%08x\n", v);
	v = (v & ~0x1FFF) | ((gConEmiPhyBase & 0xFFF00000) >> 20) | (0x1 << 12);
	CONSYS_REG_WRITE(conn_reg.topckgen_base + CONSYS_EMI_REMAP_OFFSET, v);
	WMT_PLAT_INFO_FUNC("EMI base %pa (1 MB), remap reg 0x%08x\n", &gConEmiPhyBase,
			   CONSYS_REG_READ(conn_reg.topckgen_base + CONSYS_EMI_REMAP_OFFSET));

	pEmibaseaddr = ioremap(gConEmiPhyBase + CONSYS_EMI_AP_PHY_OFFSET, CONSYS_EMI_MEM_SIZE);
	if (!pEmibaseaddr) {
		WMT_PLAT_ERR_FUNC("EMI mapping fail\n");
		return iRet;
	}
	memset_io(pEmibaseaddr, 0, CONSYS_EMI_MEM_SIZE);

	iRet = platform_driver_register(&mtk_wmt_dev_drv);
	if (iRet)
		WMT_PLAT_ERR_FUNC("WMT platform driver registered failed(%d)\n", iRet);
	return iRet;
}

INT32 mtk_wcn_consys_hw_deinit(void)
{
	if (pEmibaseaddr) {
		iounmap(pEmibaseaddr);
		pEmibaseaddr = NULL;
	}
#ifdef CONFIG_MTK_HIBERNATION
	unregister_swsusp_restore_noirq_func(ID_M_CONNSYS);
#endif

	platform_driver_unregister(&mtk_wmt_dev_drv);
	return 0;
}

UINT8 *mtk_wcn_consys_emi_virt_addr_get(UINT32 ctrl_state_offset)
{
	UINT8 *p_virtual_addr = NULL;

	if (!pEmibaseaddr) {
		WMT_PLAT_ERR_FUNC("EMI base address is NULL\n");
		return NULL;
	}
	WMT_PLAT_DBG_FUNC("ctrl_state_offset(%08x)\n", ctrl_state_offset);
	p_virtual_addr = pEmibaseaddr + ctrl_state_offset;

	return p_virtual_addr;
}

UINT32 mtk_wcn_consys_soc_chipid(void)
{
	return PLATFORM_SOC_CHIP;
}

struct pinctrl *mtk_wcn_consys_get_pinctrl()
{
	return consys_pinctrl;
}
INT32 mtk_wcn_consys_set_dynamic_dump(PUINT32 str_buf)
{
	PUINT8 vir_addr = NULL;

	vir_addr = mtk_wcn_consys_emi_virt_addr_get(EXP_APMEM_CTRL_CHIP_DYNAMIC_DUMP);
	if (!vir_addr) {
		WMT_PLAT_ERR_FUNC("get vir address fail\n");
		return -2;
	}
	memcpy(vir_addr, str_buf, DYNAMIC_DUMP_GROUP_NUM*8);
	WMT_PLAT_INFO_FUNC("dynamic dump register value(0x%08x)\n", CONSYS_REG_READ(vir_addr));
	return 0;
}
