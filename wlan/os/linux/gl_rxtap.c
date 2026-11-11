// SPDX-License-Identifier: GPL-2.0
/*
 * rxtap: monitor mode for the MT6755 CONSYS Wi-Fi (wlan gen2), plus RX debugging.
 *
 * The Wi-Fi firmware (WIFI_RAM_CODE_6755) has a sniffer that the gen2 driver never used: command
 * 0xfe (hemCmdSetMonitor in the firmware log strings), the same 16-byte layout as the later
 * gen3 drivers' CMD_ID_SET_MONITOR:
 *   enable, band (1 = 2.4 GHz, 2 = 5 GHz), primary channel, SCO (1 above, 3 below), channel
 *   width (0 = 20/40, 1 = 80 MHz), centre channel 1, centre channel 2, 9 reserved.
 * While it is on, the firmware hands every frame it hears on that channel to the host as a DATA
 * packet with the 802.11-header flag (HIF_RX_HDR_80211_HEADER_FORMAT) set, followed by a trailer
 * (rxtap_trailer bytes, the first of which is the legacy rate in 500 kb/s units); the station's
 * own traffic keeps arriving as usual, converted to 802.3. The FCS is not included.
 *
 * Monitor interface (cfg80211):
 *   iw dev wlan0 interface add mon0 type monitor; ip link set mon0 up
 * Up = sniffer on, down = off. While wlan0 is associated, mon0 listens on the AP's channel
 * (cfg80211 refuses channel changes while other interfaces are up); with wlan0 down, `iw dev mon0
 * set channel <n> [HT40+|HT40-|80MHz]` / airodump-ng channel hopping work. Frames get a radiotap
 * header (flags, rate if legacy, channel, signal). No injection: transmitted frames are dropped.
 * Sniffed frames never reach the normal RX path (the driver cannot parse 802.11-format data and
 * would take frames from our AP to other stations as ours), with or without mon0.
 * While the sniffer is on, the firmware stops its normal RX processing (no decryption, no 802.3
 * conversion): the station link carries no incoming traffic until mon0 goes down again.
 * Firmware limitation: every sniffer command (on, off, retune) keeps one HIF TX1 (TC4, command)
 * buffer for good - the pool holds 4 and only a Wi-Fi function restart refills it; at 0 the
 * driver resets the whole chip. So on/retune need >= RXTAP_TC4_ON free buffers and off needs
 * >= RXTAP_TC4_OFF (the station needs 2 for EAPOL); otherwise -EBUSY. tablet-monitor restarts
 * the Wi-Fi function to refill the pool (on stop and before every channel change).
 * mon0 survives Wi-Fi function off/on and chip resets (no carrier until it is brought down and up
 * again); it is unregistered only when the module unloads. Lock order: the driver's halt lock is
 * held around wlanRemove() -> rxtapExit() while cfg80211 calls hold the RTNL and then wait for the
 * halt lock in kalIoctl(), so nothing here may take the RTNL from the Wi-Fi off/reset path.
 *
 * /proc/net/wlan/rxtap (debugging)
 *   read                 census of every frame the firmware hands to the host, taken before any
 *                        driver filtering (class-error / STA-record checks)
 *   write "filter <hex>" send CMD_ID_SET_RX_FILTER with this raw value (not stored as the OS filter)
 *   write "os"           resend the driver's own filter (u4OsPacketFilter): undo "filter"
 *   write "cmd <cid> <hex bytes>"  send a raw firmware command
 *   write "chan <ch> [HT40+|HT40-|80MHz]"  channel for the next "mon0 up" (cfg80211 only takes
 *                        monitor channels while mon0 is up; this saves a retune, i.e. a buffer)
 *   write "dump <n>"     hex-dump the next n frames (HIF header + first 48 bytes) to the kernel log
 *   write "reset"        zero the counters
 */
#include "precomp.h"
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/hex.h>
#include <linux/rtnetlink.h>
#include <linux/etherdevice.h>
#include <linux/moduleparam.h>
#include <net/cfg80211.h>
#include <net/ieee80211_radiotap.h>

#define RXTAP_NAME		"rxtap"
#define RXTAP_CID_SNIFFER	0xfe
#define RXTAP_TC4_ON		3	/* free command buffers needed to turn on / retune */
#define RXTAP_TC4_OFF		2	/* ... to turn off */

static P_GLUE_INFO_T g_prRxtapGlue;
static atomic_t rxtap_dump_left = ATOMIC_INIT(0);
static u32 rxtap_last_filter;
static int rxtap_last_status = -1;

static int rxtap_trailer = 9;	/* bytes the firmware appends to each sniffed frame */
module_param(rxtap_trailer, int, 0644);
MODULE_PARM_DESC(rxtap_trailer, "length of the firmware's trailer after each sniffed frame");

static struct net_device __rcu *g_mon;	/* the monitor interface, while it exists */
static bool g_sniffing;			/* firmware sniffer on */
static struct { u8 band, ch, sco, width, s1; } g_monch = { BAND_2G4, 1, 0, 0, 0 };
static u8 g_cur_ch;			/* channel of the last sniffer-on command */

static struct {
	u32 pkt[4];			/* by HIF packet type: data, event, loopback, mgmt */
	u32 fmt80211, fmt8023;		/* data/mgmt frames with/without the 802.11-header flag */
	u32 sub[3][16];			/* 802.11 frames by type (mgmt/ctrl/data) and subtype */
	u32 to_me, group, not_me;	/* by A1 (802.11) or DA (802.3) */
	u32 other_bss;			/* 802.11 mgmt/data whose BSSID is not ours */
	u32 starec_invalid;		/* data with an STA record index the driver does not know */
	u32 max_len;
	u32 sniffed, delivered;		/* 802.11-format data packets consumed / passed to mon0 */
} rs;

static inline bool rxtap_is_group(const u8 *a) { return a[0] & 1; }

static const u8 rxtap_legacy_rates[] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };

/* one sniffed frame -> radiotap + 802.11 frame on mon0 */
static void rxtap_deliver(P_HIF_RX_HEADER_T h, const u8 *p, u32 plen)
{
	struct net_device *dev;
	struct sk_buff *skb;
	int flen = (int)plen - rxtap_trailer, i;
	u8 *rt, rate = 0, ch = h->ucHwChannelNum;
	bool g5 = ch > 14;
	u16 chflags;

	rcu_read_lock();
	dev = rcu_dereference(g_mon);
	if (!dev || !netif_running(dev) || flen < 10)
		goto out;
	if (rxtap_trailer > 0)
		for (i = 0; i < ARRAY_SIZE(rxtap_legacy_rates); i++)
			if (p[flen] == rxtap_legacy_rates[i])
				rate = p[flen];
	skb = dev_alloc_skb(16 + flen);
	if (!skb) {
		DEV_STATS_INC(dev, rx_dropped);
		goto out;
	}
	/* radiotap: version 0, len 15; flags @8, rate @9 (padding if absent), channel @10, dBm @14 */
	rt = skb_put_zero(skb, 15);
	rt[2] = 15;
	put_unaligned_le32(BIT(IEEE80211_RADIOTAP_FLAGS) | (rate ? BIT(IEEE80211_RADIOTAP_RATE) : 0) |
			   BIT(IEEE80211_RADIOTAP_CHANNEL) | BIT(IEEE80211_RADIOTAP_DBM_ANTSIGNAL), rt + 4);
	rt[9] = rate;
	put_unaligned_le16(ieee80211_channel_to_frequency(ch, g5 ? NL80211_BAND_5GHZ : NL80211_BAND_2GHZ),
			   rt + 10);
	/* radiotap channel flags: 0x0020 CCK, 0x0040 OFDM, 0x0080 2 GHz, 0x0100 5 GHz */
	chflags = g5 ? 0x0140 : 0x0080 | (rate == 2 || rate == 4 || rate == 11 || rate == 22 ? 0x0020 : 0x0040);
	put_unaligned_le16(chflags, rt + 12);
	rt[14] = (u8)(s8)((int)h->ucRcpi / 2 - 110);	/* RCPI -> dBm */
	skb_put_data(skb, p, flen);
	skb->dev = dev;
	skb_reset_mac_header(skb);
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb->pkt_type = PACKET_OTHERHOST;
	skb->protocol = htons(ETH_P_802_2);
	DEV_STATS_INC(dev, rx_packets);
	DEV_STATS_ADD(dev, rx_bytes, flen);
	rs.delivered++;
	netif_rx(skb);
out:
	rcu_read_unlock();
}

/* Called for every RX packet before the driver's own processing. TRUE: consumed (caller frees it). */
bool rxtapInspect(P_ADAPTER_T prAdapter, P_SW_RFB_T prSwRfb)
{
	P_HIF_RX_HEADER_T h = prSwRfb->prHifRxHdr;
	P_BSS_INFO_T bss;
	const u8 *p, *a1, *bssid = NULL;
	u32 off, hlen, plen, t = prSwRfb->ucPacketType;
	bool f80211;
	int n;

	if (!h)
		return false;
	if (t < 4)
		rs.pkt[t]++;
	if (t != HIF_RX_PKT_TYPE_DATA && t != HIF_RX_PKT_TYPE_MANAGEMENT)
		return false;

	off = h->ucHerderLenOffset & HIF_RX_HDR_HEADER_OFFSET_MASK;
	hlen = (h->ucHerderLenOffset & HIF_RX_HDR_HEADER_LEN) >> HIF_RX_HDR_HEADER_LEN_OFFSET;
	if (h->u2PacketLen < HIF_RX_HDR_SIZE + off)
		return false;
	plen = h->u2PacketLen - HIF_RX_HDR_SIZE - off;
	p = (const u8 *)h + HIF_RX_HDR_SIZE + off;
	if (plen > rs.max_len)
		rs.max_len = plen;
	bss = &prAdapter->rWifiVar.arBssInfo[NETWORK_TYPE_AIS_INDEX];

	/* management frames always carry an 802.11 header; data only with the flag */
	f80211 = t == HIF_RX_PKT_TYPE_MANAGEMENT || (h->uc80211_Reorder_PAL_TCL & HIF_RX_HDR_80211_HEADER_FORMAT);
	if (f80211 && plen >= 24) {
		u16 fc = p[0] | (p[1] << 8);
		u8 type = (fc >> 2) & 3, subtype = (fc >> 4) & 15;
		bool tods = fc & BIT(8), fromds = fc & BIT(9);

		rs.fmt80211++;
		if (type < 3)
			rs.sub[type][subtype]++;
		a1 = p + 4;
		if (type == 0)
			bssid = p + 16;
		else if (type == 2)
			bssid = tods && !fromds ? p + 4 : !tods && fromds ? p + 10 : !tods ? p + 16 : NULL;
		if (bssid && !rxtap_is_group(bssid) && !EQUAL_MAC_ADDR(bssid, bss->aucBSSID))
			rs.other_bss++;
	} else if (!f80211 && plen >= 14) {
		rs.fmt8023++;
		a1 = p;
	} else {
		a1 = NULL;
	}
	if (a1) {
		if (rxtap_is_group(a1))
			rs.group++;
		else if (EQUAL_MAC_ADDR(a1, bss->aucOwnMacAddr))
			rs.to_me++;
		else
			rs.not_me++;
	}
	if (t == HIF_RX_PKT_TYPE_DATA && !cnmGetStaRecByIndex(prAdapter, h->ucStaRecIdx))
		rs.starec_invalid++;

	if (atomic_read(&rxtap_dump_left) > 0) {
		atomic_dec(&rxtap_dump_left);
		n = min_t(u32, plen, 48);
		pr_info("rxtap: type=%u len=%u hlen=%u fl=%02x seq=%04x sta=%u rcpi=%u ch=%u | %*ph\n",
			t, plen, hlen, h->uc80211_Reorder_PAL_TCL, h->u2SeqNoTid, h->ucStaRecIdx,
			h->ucRcpi, h->ucHwChannelNum, n, p);
	}

	if (t == HIF_RX_PKT_TYPE_DATA && (h->uc80211_Reorder_PAL_TCL & HIF_RX_HDR_80211_HEADER_FORMAT)) {
		rs.sniffed++;
		rxtap_deliver(h, p, plen);
		return true;
	}
	return false;
}

static WLAN_STATUS rxtapSetFilter(IN P_ADAPTER_T prAdapter, IN PVOID pvSetBuffer, IN UINT_32 u4SetBufferLen,
				  OUT PUINT_32 pu4SetInfoLen)
{
	*pu4SetInfoLen = sizeof(UINT_32);
	return wlanSendSetQueryCmd(prAdapter, CMD_ID_SET_RX_FILTER, TRUE, FALSE, TRUE,
				   nicCmdEventSetCommon, nicOidCmdTimeoutCommon,
				   sizeof(UINT_32), (PUINT_8) pvSetBuffer, pvSetBuffer, u4SetBufferLen);
}

struct rxtap_cmd { u8 cid, len, resp, buf[64]; };

static WLAN_STATUS rxtapSendCmd(IN P_ADAPTER_T prAdapter, IN PVOID pvSetBuffer, IN UINT_32 u4SetBufferLen,
				OUT PUINT_32 pu4SetInfoLen)
{
	struct rxtap_cmd *c = pvSetBuffer;

	*pu4SetInfoLen = sizeof(*c);
	return wlanSendSetQueryCmd(prAdapter, c->cid, TRUE, c->resp, TRUE,
				   nicCmdEventSetCommon, nicOidCmdTimeoutCommon,
				   c->len, c->buf, pvSetBuffer, u4SetBufferLen);
}

/* firmware sniffer on/off; while associated, on the AP's channel so the link survives */
static int rxtap_sniffer(bool on)
{
	P_GLUE_INFO_T g = g_prRxtapGlue;
	struct rxtap_cmd c = { .cid = RXTAP_CID_SNIFFER, .len = 16 };
	P_BSS_INFO_T bss;
	u32 len, tc4;
	int st;

	if (!g)
		return -ENODEV;
	tc4 = g->prAdapter->rTxCtrl.rTc.aucFreeBufferCount[TC4_INDEX];
	if (tc4 < (on ? RXTAP_TC4_ON : RXTAP_TC4_OFF)) {
		pr_warn("rxtap: sniffer %s refused: %u free command buffers (the firmware keeps one per sniffer command; restart the Wi-Fi function to refill)\n",
			on ? "on" : "off", tc4);
		return -EBUSY;
	}
	bss = &g->prAdapter->rWifiVar.arBssInfo[NETWORK_TYPE_AIS_INDEX];
	c.buf[0] = on;
	if (bss->eConnectionState == PARAM_MEDIA_STATE_CONNECTED) {
		c.buf[1] = bss->eBand;
		c.buf[2] = bss->ucPrimaryChannel;
	} else {
		c.buf[1] = g_monch.band;
		c.buf[2] = g_monch.ch;
		c.buf[3] = g_monch.sco;
		c.buf[4] = g_monch.width;
		c.buf[5] = g_monch.s1;
	}
	if (on)
		g_sniffing = true;
	st = kalIoctl(g, rxtapSendCmd, &c, sizeof(c), FALSE, FALSE, TRUE, FALSE, &len);
	if (!on || st != WLAN_STATUS_SUCCESS)
		g_sniffing = false;
	else
		g_cur_ch = c.buf[2];
	pr_info("rxtap: sniffer %s band %u ch %u sco %u width %u%s (%u command buffers left) -> 0x%x\n",
		on ? "on" : "off", c.buf[1], c.buf[2], c.buf[3], c.buf[4],
		bss->eConnectionState == PARAM_MEDIA_STATE_CONNECTED ? " (AP channel)" : "", tc4 - 1, st);
	return st == WLAN_STATUS_SUCCESS ? 0 : -EIO;
}

/* ---- monitor net_device + cfg80211 ops ---- */

static int rxtap_mon_open(struct net_device *dev)
{
	int err = rxtap_sniffer(true);

	if (!err)
		netif_carrier_on(dev);
	return err;
}

static int rxtap_mon_stop(struct net_device *dev)
{
	if (g_sniffing && rxtap_sniffer(false))
		g_sniffing = false;	/* refused: the firmware sniffs on until the Wi-Fi function restarts */
	netif_carrier_off(dev);
	return 0;
}

static netdev_tx_t rxtap_mon_xmit(struct sk_buff *skb, struct net_device *dev)
{
	DEV_STATS_INC(dev, tx_dropped);		/* no injection */
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops rxtap_mon_ops = {
	.ndo_open = rxtap_mon_open,
	.ndo_stop = rxtap_mon_stop,
	.ndo_start_xmit = rxtap_mon_xmit,
};

static void rxtap_mon_setup(struct net_device *dev)
{
	dev->netdev_ops = &rxtap_mon_ops;
	dev->type = ARPHRD_IEEE80211_RADIOTAP;
	dev->flags = IFF_NOARP;
	dev->addr_len = ETH_ALEN;
	dev->mtu = 2304;
	dev->needs_free_netdev = true;
}

struct wireless_dev *rxtapAddIface(struct wiphy *wiphy, const char *name, unsigned char name_assign_type,
				   enum nl80211_iftype type, struct vif_params *params)
{
	struct net_device *dev;
	struct wireless_dev *wdev;
	int err;

	if (type != NL80211_IFTYPE_MONITOR)
		return ERR_PTR(-EOPNOTSUPP);
	if (rtnl_dereference(g_mon))
		return ERR_PTR(-EBUSY);
	if (!g_prRxtapGlue)
		return ERR_PTR(-ENETDOWN);
	dev = alloc_netdev(sizeof(*wdev), name, name_assign_type, rxtap_mon_setup);
	if (!dev)
		return ERR_PTR(-ENOMEM);
	wdev = netdev_priv(dev);
	wdev->wiphy = wiphy;
	wdev->iftype = NL80211_IFTYPE_MONITOR;
	wdev->netdev = dev;
	dev->ieee80211_ptr = wdev;
	dev_net_set(dev, wiphy_net(wiphy));
	SET_NETDEV_DEV(dev, wiphy_dev(wiphy));
	eth_hw_addr_set(dev, g_prRxtapGlue->prDevHandler->dev_addr);
	err = cfg80211_register_netdevice(dev);
	if (err) {
		free_netdev(dev);
		return ERR_PTR(err);
	}
	rcu_assign_pointer(g_mon, dev);
	return wdev;
}

int rxtapDelIface(struct wiphy *wiphy, struct wireless_dev *wdev)
{
	struct net_device *dev = rtnl_dereference(g_mon);

	if (!dev || wdev != dev->ieee80211_ptr)
		return -EOPNOTSUPP;		/* wlan0 itself cannot be deleted */
	RCU_INIT_POINTER(g_mon, NULL);
	cfg80211_unregister_netdevice(dev);	/* closes it (sniffer off), waits for RCU readers */
	return 0;
}

int rxtapSetMonitorChannel(struct wiphy *wiphy, struct net_device *dev, struct cfg80211_chan_def *cd)
{
	struct ieee80211_channel *c = cd->chan;

	g_monch.band = c->band == NL80211_BAND_5GHZ ? BAND_5G : BAND_2G4;
	g_monch.ch = ieee80211_frequency_to_channel(c->center_freq);
	g_monch.sco = 0;
	g_monch.width = 0;
	g_monch.s1 = 0;
	switch (cd->width) {
	case NL80211_CHAN_WIDTH_40:
		g_monch.sco = cd->center_freq1 > c->center_freq ? CHNL_EXT_SCA : CHNL_EXT_SCB;
		break;
	case NL80211_CHAN_WIDTH_80:
		g_monch.width = CW_80MHZ;
		g_monch.s1 = ieee80211_frequency_to_channel(cd->center_freq1);
		break;
	default:
		break;
	}
	return g_sniffing ? rxtap_sniffer(true) : 0;
}

/* ---- /proc/net/wlan/rxtap ---- */

static int rxtap_show(struct seq_file *m, void *v)
{
	static const char *const tn[3] = { "mgmt", "ctrl", "data" };
	int i, j;

	seq_printf(m, "os_filter 0x%08x  last_set 0x%08x status %d\n",
		   g_prRxtapGlue ? g_prRxtapGlue->prAdapter->u4OsPacketFilter : 0,
		   rxtap_last_filter, rxtap_last_status);
	if (g_prRxtapGlue)
		seq_printf(m, "tc_free %u %u %u %u %u %u\n",
			   g_prRxtapGlue->prAdapter->rTxCtrl.rTc.aucFreeBufferCount[0],
			   g_prRxtapGlue->prAdapter->rTxCtrl.rTc.aucFreeBufferCount[1],
			   g_prRxtapGlue->prAdapter->rTxCtrl.rTc.aucFreeBufferCount[2],
			   g_prRxtapGlue->prAdapter->rTxCtrl.rTc.aucFreeBufferCount[3],
			   g_prRxtapGlue->prAdapter->rTxCtrl.rTc.aucFreeBufferCount[TC4_INDEX],
			   g_prRxtapGlue->prAdapter->rTxCtrl.rTc.aucFreeBufferCount[TC5_INDEX]);
	seq_printf(m, "sniffer %s ch %u  next ch %u  sniffed %u delivered %u\n", g_sniffing ? "on" : "off",
		   g_sniffing ? g_cur_ch : 0, g_monch.ch, rs.sniffed, rs.delivered);
	seq_printf(m, "hif data %u event %u loopback %u mgmt %u\n", rs.pkt[0], rs.pkt[1], rs.pkt[2], rs.pkt[3]);
	seq_printf(m, "fmt 802.11 %u 802.3 %u  max_len %u\n", rs.fmt80211, rs.fmt8023, rs.max_len);
	seq_printf(m, "a1 to_me %u group %u not_me %u  other_bss %u  data_starec_invalid %u\n",
		   rs.to_me, rs.group, rs.not_me, rs.other_bss, rs.starec_invalid);
	for (i = 0; i < 3; i++) {
		bool any = false;

		for (j = 0; j < 16; j++)
			if (rs.sub[i][j]) {
				if (!any)
					seq_printf(m, "%s:", tn[i]);
				seq_printf(m, " %d=%u", j, rs.sub[i][j]);
				any = true;
			}
		if (any)
			seq_puts(m, "\n");
	}
	return 0;
}

static int rxtap_open(struct inode *inode, struct file *file)
{
	return single_open(file, rxtap_show, NULL);
}

static ssize_t rxtap_write(struct file *file, const char __user *ubuf, size_t count, loff_t *pos)
{
	char buf[200], hex[160];
	struct rxtap_cmd c;
	u32 val, len;
	size_t n = min(count, sizeof(buf) - 1);

	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = 0;
	if (!g_prRxtapGlue)
		return -ENODEV;
	if (sscanf(buf, "filter %x", &val) == 1 || (!strncmp(buf, "os", 2) &&
	    (val = g_prRxtapGlue->prAdapter->u4OsPacketFilter, true))) {
		rxtap_last_filter = val;
		rxtap_last_status = kalIoctl(g_prRxtapGlue, rxtapSetFilter, &val, sizeof(val),
					     FALSE, FALSE, TRUE, FALSE, &len);
		pr_info("rxtap: rx filter 0x%08x -> status 0x%x\n", val, rxtap_last_status);
	} else if (sscanf(buf, "cmd %x %159s", &val, hex) == 2 || sscanf(buf, "cmdr %x %159s", &val, hex) == 2) {
		memset(&c, 0, sizeof(c));
		c.cid = val;
		c.resp = buf[3] == 'r';		/* "cmdr": wait for the firmware's response event */
		c.len = strlen(hex) / 2;
		if (c.len > sizeof(c.buf) || hex2bin(c.buf, hex, c.len))
			return -EINVAL;
		rxtap_last_status = kalIoctl(g_prRxtapGlue, rxtapSendCmd, &c, sizeof(c),
					     FALSE, FALSE, TRUE, FALSE, &len);
		pr_info("rxtap: cmd 0x%02x len %u %*ph -> status 0x%x\n", c.cid, c.len, min_t(int, c.len, 32),
			c.buf, rxtap_last_status);
	} else if (sscanf(buf, "chan %u", &val) == 1 && val >= 1 && val <= 196) {
		static const u8 c80[] = { 42, 58, 106, 122, 138, 155 };	/* 80 MHz centre channels */
		int i;

		g_monch.band = val > 14 ? BAND_5G : BAND_2G4;
		g_monch.ch = val;
		g_monch.sco = strstr(buf, "HT40+") ? CHNL_EXT_SCA : strstr(buf, "HT40-") ? CHNL_EXT_SCB : 0;
		g_monch.width = 0;
		g_monch.s1 = 0;
		if (strstr(buf, "80MHz"))
			for (i = 0; i < ARRAY_SIZE(c80); i++)
				if (val >= c80[i] - 6 && val <= c80[i] + 6) {
					g_monch.width = CW_80MHZ;
					g_monch.s1 = c80[i];
				}
	} else if (sscanf(buf, "dump %u", &val) == 1) {
		atomic_set(&rxtap_dump_left, val);
	} else if (!strncmp(buf, "reset", 5)) {
		memset(&rs, 0, sizeof(rs));
	} else {
		return -EINVAL;
	}
	return count;
}

static const struct proc_ops rxtap_ops = {
	.proc_open = rxtap_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = rxtap_write,
};

void rxtapInit(P_GLUE_INFO_T prGlueInfo, struct proc_dir_entry *root)
{
	g_prRxtapGlue = prGlueInfo;
	g_sniffing = false;
	if (root && !proc_create(RXTAP_NAME, 0600, root, &rxtap_ops))
		pr_warn("rxtap: cannot create /proc/net/wlan/" RXTAP_NAME "\n");
}

/* Wi-Fi function off / chip reset (wlanRemove, halt lock held): firmware state is gone */
void rxtapExit(struct proc_dir_entry *root)
{
	struct net_device *dev;

	g_prRxtapGlue = NULL;		/* no more firmware commands from here on */
	g_sniffing = false;
	if (root)
		remove_proc_entry(RXTAP_NAME, root);
	rcu_read_lock();
	dev = rcu_dereference(g_mon);
	if (dev && netif_running(dev)) {
		netif_carrier_off(dev);
		pr_info("rxtap: Wi-Fi stopped; bring %s down and up again to resume monitoring\n", dev->name);
	}
	rcu_read_unlock();
}

/* module unload, before the wiphy goes (no halt lock held here) */
void rxtapShutdown(void)
{
	struct net_device *dev;

	rtnl_lock();
	dev = rtnl_dereference(g_mon);
	if (dev) {
		struct wiphy *wiphy = dev->ieee80211_ptr->wiphy;

		wiphy_lock(wiphy);
		RCU_INIT_POINTER(g_mon, NULL);
		cfg80211_unregister_netdevice(dev);
		wiphy_unlock(wiphy);
	}
	rtnl_unlock();
}
