#!/usr/bin/env python3
"""Derive conn_soc/mt6755/mtk_wcn_consys_hw.c from the mt6572 one (run from conn_soc/mt6755)."""
p = 'mtk_wcn_consys_hw.c'
s = open('../mt6572/mtk_wcn_consys_hw.c').read()

def cut(s, a, b, new):
    i = s.index(a); j = s.index(b, i + len(a))
    return s[:i] + new + s[j:]

s = s.replace('''#include <linux/pm_runtime.h>
#include <linux/reset.h>
''', '''#include <linux/pm_runtime.h>
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
''', 1)
s = s.replace('{.compatible = "mediatek,mt6572-consys",},', '{.compatible = "mediatek,mt6755-consys",},')
s = s.replace('.name = "mt6572consys",', '.name = "mt6755consys",')

s = cut(s, 'static INT32 mtk_wmt_probe(struct platform_device *pdev)\n{',
        'static void mtk_wmt_remove(struct platform_device *pdev)\n{', '''static INT32 mtk_wmt_probe(struct platform_device *pdev)
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

''')

s = cut(s, 'VOID mtk_wcn_consys_power_on(VOID)', 'INT32 mtk_wcn_consys_hw_gpio_ctrl(UINT32 on)', open('../../tools/port/mt6755_power.c.in').read())
s = cut(s, '#if CONSYS_BT_WIFI_SHARE_V33\nINT32 mtk_wcn_consys_hw_bt_paldo_ctrl', 'INT32 mtk_wcn_consys_hw_state_show(VOID)', open('../../tools/port/mt6755_paldo.c.in').read())
s = cut(s, 'INT32 mtk_wcn_consys_hw_restore(struct device *device)', 'INT32 mtk_wcn_consys_hw_deinit(void)', open('../../tools/port/mt6755_init.c.in').read())
open(p, 'w').write(s)
print('ok')
