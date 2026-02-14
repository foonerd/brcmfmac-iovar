# brcm-iovar

Runtime iovar access for Broadcom/Cypress brcmfmac WiFi firmware
via nl80211 vendor commands.


## What this does

Sends firmware commands (iovars) to the Broadcom/Cypress CYW43xx wireless
chipset at runtime, without:

- The proprietary Broadcom "wl" utility (licensing/availability issues)
- Custom kernel modules or driver patches
- Module reload or WiFi disruption
- Rebooting

The primary use case is setting btc_mode (Bluetooth coexistence mode) on
Raspberry Pi to resolve WiFi/Bluetooth interference during A2DP streaming.


## How it works

### Communication path

```
brcm-iovar (this tool)
    |
    | NL80211_CMD_VENDOR via generic netlink socket
    v
kernel: cfg80211 subsystem
    |
    | routes vendor command to driver
    v
brcmfmac: brcmf_cfg80211_vndr_cmds_dcmd_handler()
    |
    | brcmf_fil_cmd_data_set() / brcmf_fil_cmd_data_get()
    v
BCDC protocol layer
    |
    | over SDIO bus (Pi 3B+/4B/Zero 2W) or PCIe (Pi 5)
    v
CYW43xx firmware: processes iovar command
```

### Key discovery

The mainline brcmfmac driver (in every Linux kernel since ~3.10) registers
an nl80211 vendor command handler that provides GENERIC firmware command
passthrough:

```c
/* drivers/net/wireless/broadcom/brcm80211/brcmfmac/vendor.c */
const struct wiphy_vendor_command brcmf_vendor_cmds[] = {
    {
        .vendor_id = BROADCOM_OUI,          /* 0x001018 */
        .subcmd = BRCMF_VNDR_CMDS_DCMD,    /* 1 */
        .doit = brcmf_cfg80211_vndr_cmds_dcmd_handler
    }
};
```

This handler accepts a `brcmf_vndr_dcmd_hdr` structure followed by arbitrary
iovar data, and passes it directly to the firmware via
`brcmf_fil_cmd_data_set()` or `brcmf_fil_cmd_data_get()`.

This is exactly how the proprietary "wl" utility communicates with brcmfmac
on Raspberry Pi - it uses libnl to send nl80211 vendor commands, NOT private
ioctls (SIOCDEVPRIVATE).


### Why this was not obvious

Several factors obscured this mechanism:

1. The old brcmfmac driver (kernel 3.7) had `ndo_do_ioctl` registered.
   This was removed in 2013 (commit by Arend van Spriel, Broadcom) because
   it only handled SIOCETHTOOL which moved to ethtool_ops.

2. Current mainline brcmfmac registers NO ioctl handlers at all:
   ```c
   static const struct net_device_ops brcmf_netdev_ops_pri = {
       .ndo_open           = brcmf_netdev_open,
       .ndo_stop           = brcmf_netdev_stop,
       .ndo_start_xmit     = brcmf_netdev_start_xmit,
       .ndo_set_mac_address = brcmf_netdev_set_mac_address,
       .ndo_set_rx_mode     = brcmf_netdev_set_multicast_list
   };
   ```
   No ndo_do_ioctl, no ndo_siocdevprivate. This led to the incorrect
   conclusion that userspace cannot send arbitrary firmware commands.

3. The vendor command interface is in vendor.c, which is separate from
   the net_device_ops registration in core.c. It operates through
   cfg80211/nl80211, not through the network device ioctl path.

4. The Broadcom "wl" utility is closed-source, so its communication
   mechanism could not be directly inspected.

5. The wl utility's libnl dependency (visible in Infineon community
   forums) was the key clue. The Raspberry Pi compliance testing package
   includes a "wl" binary that links against libnl-3 and libnl-genl-3.


## Build

### Dependencies (build host)

```
apt-get install gcc make libnl-3-dev libnl-genl-3-dev
```

### Native build

```
make
```

### Cross-compile for Volumio 4 (Raspberry Pi armhf)

```
make CROSS_COMPILE=arm-linux-gnueabihf-
```

### Cross-compile for Raspberry Pi 64-bit

```
make CROSS_COMPILE=aarch64-linux-gnu-
```

### Strip for deployment

```
make strip
```

### Docker builds (reproducible, all Raspberry Pi targets)

Build binaries for **armv6l** (Pi Zero/1), **armhf** (Pi 2/3/4 32-bit), and **arm64** (Pi 3/4/5 64-bit) using the same pattern as other Volumio Docker-based builds:

```bash
# Single architecture
./docker/run-docker-brcmfmac_iovar.sh armv6    # Pi Zero/1
./docker/run-docker-brcmfmac_iovar.sh armhf    # Pi 2/3/4 (32-bit)
./docker/run-docker-brcmfmac_iovar.sh arm64    # Pi 3/4/5 (64-bit)

# All targets
./build-matrix.sh
```

Output: `out/armv6/brcm-iovar`, `out/armhf/brcm-iovar`, `out/arm64/brcm-iovar`. Requires Docker (with buildx for multi-platform).


## Runtime dependencies (target)

```
apt-get install libnl-3-200 libnl-genl-3-200
```

These are typically already present on Volumio 4 (Bookworm).


## Usage

```
brcm-iovar <interface> get_int <iovar_name>
brcm-iovar <interface> set_int <iovar_name> <value>
```

Requires root or CAP_NET_ADMIN capability.

### Examples

```
# Read current BT coexistence mode
brcm-iovar wlan0 get_int btc_mode

# Set BT coexistence to full TDM (recommended for A2DP)
brcm-iovar wlan0 set_int btc_mode 4

# Read BT coexistence parameters
brcm-iovar wlan0 get_int btc_params

# Read firmware country code
brcm-iovar wlan0 get_int country
```


## btc_mode values

| Value | Mode     | Description                                      |
|-------|----------|--------------------------------------------------|
| 0     | Disabled | No BT coexistence (not recommended)              |
| 1     | Default  | Basic coexistence                                |
| 2     | Serial   | SECI-based serial coexistence                    |
| 4     | Full TDM | Time-division multiplexing (best for A2DP audio) |

Full TDM (mode 4) dedicates time slots to WiFi and Bluetooth,
preventing the mutual interference that causes audio dropouts
during simultaneous WiFi data transfer and BT A2DP streaming.


## Integration with Volumio plugin

This tool enables a Volumio plugin to:

1. Set btc_mode=4 when Bluetooth A2DP stream starts (zero WiFi disruption)
2. Revert to btc_mode=1 when BT stream stops
3. Read current btc_mode for status display
4. Set btc_mode=4 at boot via NVRAM overlay as persistent default

Combined approach:
- NVRAM overlay (/usr/lib/firmware/brcm/brcmfmac43455-sdio.txt) for
  persistent boot default
- This tool for runtime switching without reboot or WiFi interruption


## Kernel source references

- `drivers/net/wireless/broadcom/brcm80211/brcmfmac/vendor.c`
  Vendor command handler implementation
- `drivers/net/wireless/broadcom/brcm80211/brcmfmac/vendor.h`
  brcmf_vndr_dcmd_hdr structure, nl attribute IDs
- `drivers/net/wireless/broadcom/brcm80211/brcmfmac/fwil.h`
  BRCMF_C_GET_VAR (262), BRCMF_C_SET_VAR (263)
- `drivers/net/wireless/broadcom/brcm80211/brcmfmac/core.c`
  net_device_ops (no ioctl handlers registered)


## Research references

- Quarkslab: Broadcom WiFi firmware reverse engineering
  (documented firmware ioctl handler and iovar protocol)
- Nexmon project (seemoo-lab): brcmfmac firmware patching
  (documented internal driver communication mechanisms)
- Infineon community: wl tool and compliance testing
  (confirmed libnl dependency and vendor command mechanism)
- Kernel commit: "brcmfmac: remove redundant ioctl handlers"
  (Arend van Spriel, Broadcom, 2013-11-29)
  Removed ndo_do_ioctl from brcmfmac net_device_ops


## Supported hardware

Any device using the brcmfmac driver:

| Raspberry Pi Model | Chip     | Bus  | Supported |
|--------------------|----------|------|-----------|
| Pi 3B              | BCM43430 | SDIO | Yes       |
| Pi 3B+             | BCM43455 | SDIO | Yes       |
| Pi 4B              | BCM43455 | SDIO | Yes       |
| Pi Zero 2W         | BCM43430 | SDIO | Yes       |
| Pi 5               | CYW43455 | PCIe | Yes       |
| CM4                | BCM43455 | SDIO | Yes       |
| CM5                | CYW43455 | PCIe | Yes       |


## License

GPL-2.0-only (matching brcmfmac kernel driver license)
