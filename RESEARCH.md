# Research: Bluetooth/WiFi coexistence on Raspberry Pi and runtime firmware iovar access

This document records the complete investigation that led to the creation of
this tool. It serves as legwork evidence - the technical reasoning, dead ends,
and discoveries that informed the design.

Date: 2026-02-14
Platform: Volumio 4 (Debian Bookworm), kernel 6.12.x
Test hardware: Raspberry Pi 5 Model B Rev 1.1 (revision e04171)


## The problem

Every Raspberry Pi with built-in wireless uses a single combo WiFi/Bluetooth
chip from the Broadcom/Cypress/Infineon lineage (Broadcom sold the wireless
IoT division to Cypress in 2016, Infineon acquired Cypress in 2020). The chip
connects to the SoC via SDIO for WiFi and UART for Bluetooth. Both WiFi and
Bluetooth share a single PCB antenna with no external antenna connector on
consumer SBC models.

This architecture creates a fundamental constraint: WiFi and Bluetooth share
both the antenna and the 2.4 GHz radio band. The chip uses Time Division
Multiplexing (TDM) coexistence to arbitrate access. This is not a bug - it is
a hardware design limitation inherent to every generation.

The firmware file controlling coexistence behaviour lives in
/usr/lib/firmware/brcm/ and contains btc_mode and btc_params entries that
determine priority allocation between WiFi and Bluetooth.

For audio streaming applications (A2DP source or sink), this is the worst-case
scenario: sustained bidirectional radio demand on a shared antenna with TDM
arbitration. The device simultaneously pulls audio data over WiFi and pushes a
continuous outbound Bluetooth audio stream.


## Hardware breakdown by generation


### Generation 1 wireless: Pi 3B, Pi 3A+, Pi Zero W, Pi Zero 2 W

Chip: BCM43438 (also known as CYW43438)
WiFi: 802.11n, 2.4 GHz only, single band
Bluetooth: 4.1 Classic + BLE (Pi 3B, Zero W) / 4.2 (later revisions)
Antenna: Ceramic chip antenna (Pi 3B, 3A+), PCB trace "Proant InSide" antenna (Zero W, Zero 2 W)
Driver: brcmfmac (FullMAC)
Firmware: brcmfmac43430-sdio.txt

Interference profile - worst of all generations:

- 2.4 GHz only WiFi means WiFi and Bluetooth always compete on the same band.
  There is no escape to 5 GHz.
- Coexistence on the BCM43438 is notably poor. Multiple GitHub issues document
  this (raspberrypi/linux #1552, #1444, #5293).
- Bluetooth audio stuttering when WiFi is active is a well-documented and
  essentially permanent problem on this chipset.
- The Pi Zero W and Zero 2 W have an even smaller PCB trace antenna than the
  Pi 3B ceramic antenna, further reducing effective range.
- Pi Zero 2 W uses the same BCM43438 chip despite shipping years later (2021).
  The coexistence problem was carried forward unchanged.

Documented symptoms:

- BT keyboard/mouse disconnects and reconnects randomly when WiFi is active
- BT audio drops/stutters when streaming over WiFi simultaneously
- WiFi throughput degrades significantly during BT scanning or data transfer
- BLE advertising reception range drops substantially with WiFi enabled

Mitigations available:

- Use ethernet (disables WiFi, frees the radio for BT exclusively)
- Use a USB WiFi dongle on 5 GHz and disable onboard WiFi
- Use a USB Bluetooth dongle and disable onboard BT
- There are no effective firmware-level fixes for this generation


### Generation 2 wireless: Pi 3B+, Pi 4B, CM4, Pi 5, CM5, Pi 500

Chip: CYW43455 (Infineon, formerly Cypress, formerly Broadcom)
Exception: Pi 400 uses BCM43456 (Synaptics), a sister chip with separate firmware but similar capabilities
WiFi: 802.11ac, dual-band 2.4 GHz + 5 GHz
Bluetooth: Marketed as 5.0 (Pi 4B onwards), but the chip only implements mandatory BT5 features
  - No 2 Mbps PHY (optional in BT5 spec)
  - No LE Coded PHY (optional in BT5 spec)
  - The same CYW43455 was previously certified as BT 4.2; the "upgrade" to 5.0
    is a recertification with identical feature set
Antenna: PCB trace antenna (all models), U.FL connector available on CM4 and CM5 only
Driver: brcmfmac (FullMAC)
Firmware: brcmfmac43455-sdio.txt

Interference profile - improved but still significant:

The key improvement is dual-band WiFi. Moving WiFi to 5 GHz eliminates the
direct band conflict with Bluetooth (which operates exclusively at 2.4 GHz).
However:

- When WiFi operates on 2.4 GHz, the same TDM coexistence problems occur as
  on the BCM43438, though the firmware coexistence algorithms are somewhat
  better.
- The PCB antenna is practically identical across Pi 3B+ through Pi 5. Argenox
  analysis confirms the antenna design has barely changed since Pi 3B+.
- The antenna is surrounded by the GPIO header pins, which impacts radiation
  pattern and effective gain.
- No external antenna option exists on consumer SBC models (Pi 3B+, 4B, 5).
  Only CM4 and CM5 provide a U.FL connector with software-selectable
  internal/external antenna.

Pi 4B specific additional interference source:

The Pi 4 introduced USB 3.0 ports. USB 3.0 signalling generates broadband RF
noise centred around 2.4 GHz (documented by Intel white paper). On the Pi 4B,
the USB 3.0 ports and HDMI ports are in close physical proximity to the
WiFi/BT antenna area. This means:

- Any active USB 3.0 device can degrade both 2.4 GHz WiFi and Bluetooth
  range/reliability
- Poorly shielded USB 3.0 cables, SATA adapters, and SSDs amplify the problem
- This is not Pi-specific - it affects all USB 3.0 implementations - but the
  Pi 4B's compact layout makes it particularly severe

Pi 5 specific notes:

- Uses the same CYW43455 as Pi 3B+ and Pi 4B. The Raspberry Pi Foundation
  blog post for Pi 5 explicitly states the chip is unchanged.
- The SDIO interface to the SoC was upgraded to DDR50 mode and the chip has a
  dedicated switched power rail, but neither change affects the RF
  characteristics.
- The Pi 5 moved USB 3.0 to the RP1 southbridge with a different physical
  layout, but the fundamental proximity issue remains.
- BLE range complaints persist on Pi 5, with users reporting significantly
  worse range than smartphones for the same BLE peripherals.
- Metal or aluminium cases significantly attenuate the already weak PCB antenna
  signal.

CM4 and CM5 notes:

- Both provide a U.FL connector for an external antenna, software-selectable
  via config.txt
- This is the only supported way to meaningfully improve wireless range on any
  Pi hardware
- The CM4 uses the same CYW43455 (despite some confusion about it using the
  BCM43456 - the FCC filing confirms CYW43455)
- The CM5 datasheet confirms CYW43455
- External antenna selection is configured at boot and cannot be changed at
  runtime


### Summary table

```
Pi Zero W       | BCM43438 | BT 4.1   | 2.4 GHz only | PCB trace  | Worst
Pi Zero 2 W     | BCM43438 | BT 4.2   | 2.4 GHz only | PCB trace  | Worst
Pi 3B           | BCM43438 | BT 4.1   | 2.4 GHz only | Ceramic    | Worst
Pi 3A+          | BCM43438 | BT 4.2   | 2.4 GHz only | Ceramic    | Worst (*)
Pi 3B+          | CYW43455 | BT 4.2   | Dual-band    | PCB trace  | Moderate
Pi 4B           | CYW43455 | BT 5.0** | Dual-band    | PCB trace  | Moderate + USB3 noise
Pi 400          | BCM43456 | BT 5.0** | Dual-band    | Internal   | Moderate (better shielding)
CM4             | CYW43455 | BT 5.0** | Dual-band    | PCB + U.FL | Best (with ext antenna)
Pi 5            | CYW43455 | BT 5.0** | Dual-band    | PCB trace  | Moderate
Pi 500          | CYW43455 | BT 5.0** | Dual-band    | Internal   | Moderate (better shielding)
CM5             | CYW43455 | BT 5.0** | Dual-band    | PCB + U.FL | Best (with ext antenna)
```

(*) Pi 3A+ uses CYW43455 per some sources - needs verification. Some
documentation lists it as BCM43438. Conflicting data.

(**) BT 5.0 marketing only - mandatory features only, no 2 Mbps PHY, no LE
Coded PHY.


## Firmware coexistence parameters (CYW43455)

The NVRAM file /usr/lib/firmware/brcm/brcmfmac43455-sdio.txt contains
coexistence parameters:

```
btc_mode=1          (default - Full TDM, one side gets full priority)
btc_params8=0x4e20
btc_params1=0x7530
btc_params50=0x972c
```

Known btc_mode values:

- Mode 0: Coexistence disabled - confirmed to crash firmware. Never use.
- Mode 1: Full TDM (default) - WiFi priority, one side gets full control
- Mode 4: BT always allowed, WiFi keeps normal activity
- Mode 5: Some simultaneous activities allowed (balanced)

Community reports confirm improved BT audio quality after changing btc_mode
to 4 and commenting out the btc_params lines. This trades WiFi performance
for BT stability.

The btc_params are coexistence tuning parameters from Cypress/Infineon.
Documentation is sparse - most knowledge comes from Infineon community forum
posts. Infineon support redirects btc_params questions to module partners
(Laird, Murata) for application-specific tuning.

This modification applies to Pi 3B+, Pi 4B, Pi 5, CM4, CM5, Pi 500 (all
CYW43455 boards). It does not apply to Pi 3B, Pi Zero W, or Pi Zero 2 W
(BCM43438 boards - different firmware file).


## Live system verification (Volumio 4, Pi 5)

The following was verified on a running Volumio 4 system (Pi 5, kernel
6.12.47-v8+, firmware 7.45.265 CYW43455).


### NVRAM file chain

All board-specific .txt files for RPi boards are symlinks to the single
generic file:

```
brcmfmac43455-sdio.raspberrypi,5-model-b.txt -> brcmfmac43455-sdio.txt
```

The single file /usr/lib/firmware/brcm/brcmfmac43455-sdio.txt (2074 bytes)
serves Pi 3A+, Pi 3B+, Pi 4B, CM4, Pi 5, CM5, and Pi 500 via symlinks.

The Pi 400 uses a separate chain: brcmfmac43456-sdio.txt (different chip).


### NVRAM btc entries

The file contains btc_mode twice:

```
Line 17:  btc_mode=1        (standalone, near top, under XTAL section)
Line 95:  btc_mode=1        (with the btc_params block at bottom)
Line 96:  btc_params8=0x4e20
Line 97:  btc_params1=0x7530
Line 98:  btc_params50=0x972c
```

The duplicate on line 17 is from the original Cypress NVRAM template. NVRAM
parsing uses last-value-wins, so line 95 is the effective one.


### Overlay filesystem

```
/dev/loop0 on /static type squashfs (ro)
overlay on / type overlay (rw,lowerdir=/mnt/static,upperdir=/mnt/ext/dyn,workdir=/mnt/ext/work)
```

- Lower (read-only): /mnt/static (the squashfs image)
- Upper (read-write): /mnt/ext/dyn (on the data partition)
- Writable with root access: confirmed
- Persists across reboots: confirmed
- Persists across OTA updates: confirmed (upper dir is not wiped)


### Security environment

- No SELinux
- No AppArmor
- Root access via sudo


## Investigation of runtime change options

The goal: change btc_mode at runtime without rebooting or disrupting WiFi.
This would allow a Bluetooth audio plugin to set btc_mode=4 when a BT stream
starts and revert to btc_mode=1 when it stops.

Five options were identified and systematically investigated.


### Option 1: NVRAM overlay file replacement

Mechanism: Place a modified NVRAM file in the overlay upper dir, shadowing the
squashfs version. Requires reboot or module reload to take effect.

Status: CONFIRMED VIABLE. Ready to implement.

Limitation: Not a runtime change. Requires either reboot or module reload
(Option 2) to apply.


### Option 2: brcmfmac module reload

Mechanism: Unload and reload the brcmfmac kernel module after modifying the
NVRAM file. The driver re-reads NVRAM on load.

```
rmmod brcmfmac_wcc    # Pi 5 companion module
rmmod brcmfmac
modprobe brcmfmac
# brcmfmac_wcc auto-loads as dependency on Pi 5
```

Status: CONFIRMED VIABLE but disruptive.

Limitation: Drops all WiFi connections during reload. If the user is connected
over WiFi, they lose access temporarily. The wlan0 interface is destroyed and
recreated.


### Option 3: Custom userspace iovar tool (initial assessment)

Initial hypothesis: The brcmfmac driver exposes private ioctls that could be
used to send iovar commands (including btc_mode) from userspace.

This hypothesis was based on the knowledge that:

- The kernel source (btcoex.c) uses brcmf_fil_iovar_int_set(ifp, "btc_mode",
  value) internally
- Broadcom's proprietary "wl" utility can set btc_mode at runtime
- Private ioctls (SIOCDEVPRIVATE) are the traditional mechanism for
  vendor-specific commands

Investigation result: iwpriv wlan0 returned "no private ioctls."

Initial conclusion: ELIMINATED. No userspace iovar passthrough exists.

This conclusion was WRONG. See "The breakthrough" below.


### Option 4: Broadcom proprietary "wl" utility

The "wl" utility is distributed by Raspberry Pi Foundation in the compliance
testing package at pip.raspberrypi.com.

Status: ELIMINATED for distribution. The binary is proprietary and licensed
for compliance testing only. Cannot be redistributed in a plugin.


### Option 5: debugfs interface

Checked all debugfs entries under /sys/kernel/debug/ieee80211/phy0/:

```
console_interval
counters
forensics
fws_stats
fwcap
features
reset
revinfo
```

No btc_mode, btc_params, btcoex, or coexistence entries exist anywhere in
debugfs. Searched all of /sys/kernel/debug/ with no matches.

Status: ELIMINATED.


## The false conclusion

After eliminating Options 3, 4, and 5, the assessment was:

```
Option 1 - NVRAM overlay:        CONFIRMED - ready to implement
Option 2 - Module reload:        CONFIRMED - works as apply mechanism
Option 3 - Custom iovar tool:    ELIMINATED (no userspace interface)
Option 4 - Proprietary wl:       ELIMINATED (licensing)
Option 5 - Debugfs:              ELIMINATED (no entries)
```

The recommended implementation was Option 1 + Option 2 combined: persistent
NVRAM modification with module reload as the apply mechanism. Runtime dynamic
switching was considered impossible without kernel module development.

This was wrong because the investigation of Option 3 asked the wrong question.
The question was "does brcmfmac expose private ioctls?" The answer was
correctly "no." But the conclusion that no userspace interface exists was
incorrect - there is a completely different communication path.


## The breakthrough: nl80211 vendor commands

The question that reopened the investigation: how does the proprietary "wl"
utility actually communicate with brcmfmac if there are no private ioctls?


### What was checked and eliminated

1. Private ioctls (SIOCDEVPRIVATE):

   iwpriv wlan0 returned "no private ioctls." The mainline brcmfmac
   net_device_ops contains no ioctl handlers:

   ```c
   static const struct net_device_ops brcmfmac_netdev_ops_pri = {
       .ndo_open = brcmf_netdev_open,
       .ndo_stop = brcmf_netdev_stop,
       .ndo_start_xmit = brcmf_netdev_start_xmit,
       .ndo_set_mac_address = brcmf_netdev_set_mac_address,
       .ndo_set_rx_mode = brcmf_netdev_set_multicast_list
   };
   ```

   No ndo_do_ioctl. No ndo_siocdevprivate. Confirmed absent.

2. Historical ioctl handler:

   The old brcmfmac (kernel 3.7) DID have an ioctl handler:

   ```c
   .ndo_do_ioctl = brcmf_netdev_ioctl_entry,
   ```

   This was REMOVED in November 2013 by Arend van Spriel (Broadcom) in commit
   "brcmfmac: remove redundant ioctl handlers". The commit message states:

   "The ioctl() entry points were empty except for handling SIOC_ETHTOOL but
   that has been obsoleted in favor of struct ethtool_ops."

   The ioctl handler only ever handled SIOCETHTOOL. It NEVER supported
   SIOCDEVPRIVATE for firmware iovar passthrough. It returned -EOPNOTSUPP for
   everything else.


### What was found

The mainline brcmfmac driver registers an nl80211 vendor command handler.

File: drivers/net/wireless/broadcom/brcm80211/brcmfmac/vendor.c

```c
static const struct wiphy_vendor_command brcmf_vendor_cmds[] = {
    {
        {
            .vendor_id = BROADCOM_OUI,
            .subcmd = BRCMF_VNDR_CMDS_DCMD
        },
        .flags = WIPHY_VENDOR_CMD_NEED_WDEV |
                 WIPHY_VENDOR_CMD_NEED_NETDEV,
        .policy = VENDOR_DCMD_POLICY,
        .doit = brcmf_cfg80211_vndr_cmds_dcmd_handler
    },
};
```

Constants:

- BROADCOM_OUI = 0x001018
- BRCMF_VNDR_CMDS_DCMD = 1

The handler brcmf_cfg80211_vndr_cmds_dcmd_handler() is a GENERIC firmware
command passthrough. It accepts arbitrary firmware commands (dcmd = "dongle
command") from userspace via nl80211 vendor data, and forwards them to the
firmware through the same brcmf_fil_cmd_data_set/get() functions that the
kernel uses internally.

This means any firmware iovar - including btc_mode - can be set from userspace
through this interface.


### Communication path

```
userspace (libnl)
    |
    v
NL80211_CMD_VENDOR (generic netlink)
    |
    v
cfg80211 vendor command dispatch
    |
    v
brcmf_cfg80211_vndr_cmds_dcmd_handler() [vendor.c]
    |
    v
brcmf_fil_cmd_data_set() or brcmf_fil_cmd_data_get() [fwil.c]
    |
    v
BCDC protocol layer
    |
    v
SDIO/PCIe bus
    |
    v
CYW43455 firmware
```


### Vendor command data structure

```c
struct brcmf_vndr_dcmd_hdr {
    uint32_t cmd;      // BRCMF_C_GET_VAR (262) or BRCMF_C_SET_VAR (263)
    int32_t  len;      // expected return buffer length
    uint32_t offset;   // byte offset where payload begins
    uint32_t set;      // 0 = get, 1 = set
    uint32_t magic;    // not validated by mainline handler
};
```

For setting btc_mode=4:

- cmd = 263 (BRCMF_C_SET_VAR)
- set = 1
- payload = "btc_mode\0" + uint32_le(4)

For reading btc_mode:

- cmd = 262 (BRCMF_C_GET_VAR)
- set = 0
- payload = "btc_mode\0"


### Why this was obscured

1. The vendor command interface is in vendor.c, which is separate from the
   net_device_ops in core.c. It operates through cfg80211/nl80211, not the
   network device ioctl path. Searching for ioctl handlers will never find it.

2. The "wl" utility is closed-source. Its communication mechanism is not
   directly inspectable. The key clue came from Infineon community forums
   showing the wl utility links against libnl-3 and libnl-genl-3 - not the
   ioctl libraries you would expect if it used SIOCDEVPRIVATE.

3. The removal of the ioctl handler in 2013 created a false narrative. The
   fact that brcmfmac "has no ioctl interface" is technically true for the
   net_device path, but irrelevant because the vendor command path through
   cfg80211 was added independently.

4. iwpriv only queries wireless extensions private ioctls. It does not probe
   nl80211 vendor commands. The "no private ioctls" result was accurate but
   answered the wrong question.

5. The nl80211 vendor command mechanism has been in mainline since kernel 3.10+.
   It is not a Pi-specific patch. It works on any system running brcmfmac.


### Clue that broke it open

The Infineon community forums showed build requirements for the "wl" utility
that included libnl-3 and libnl-genl-3 as link dependencies. A tool using
SIOCDEVPRIVATE ioctls would have no reason to link against the generic netlink
libraries. This indicated the communication path was nl80211, not ioctl.

From there, searching the brcmfmac kernel source for vendor command
registration led directly to vendor.c and the BRCMF_VNDR_CMDS_DCMD handler.


## Corrected option matrix

```
Option 1 - NVRAM overlay:          CONFIRMED (boot-time default)
Option 2 - Module reload:          CONFIRMED (apply with disruption)
Option 3 - nl80211 vendor tool:    CONFIRMED VIABLE (runtime, zero disruption)
Option 4 - Proprietary wl:         ELIMINATED (licensing)
Option 5 - Debugfs:                ELIMINATED (no entries)
```

Recommended: Option 1 + Option 3 combined.

- Option 1 provides the persistent boot-time default (btc_mode=4 in NVRAM)
- Option 3 provides runtime switching when BT streams start/stop
- Zero WiFi disruption, no module reload, no reboot for runtime changes


## Supported hardware

All Raspberry Pi models using brcmfmac with CYW43455:

- Pi 3B+ (CYW43455, SDIO)
- Pi 4B (CYW43455, SDIO)
- Pi 5 (CYW43455, SDIO/PCIe)
- CM4 (CYW43455, SDIO)
- CM5 (CYW43455, SDIO/PCIe)
- Pi 500 (CYW43455)

Also applicable to BCM43438 boards (Pi 3B, Zero W, Zero 2 W) for any iovar
that the BCM43438 firmware supports, though btc_mode tuning effectiveness on
that chipset is limited.


## Runtime dependencies

- libnl-3-200, libnl-genl-3-200 (present on Debian Bookworm, Volumio 4)
- CAP_NET_ADMIN capability or root access
- brcmfmac driver loaded
- WiFi interface up (wlan0)


## Verification status

The tool compiles cleanly and the kernel interface is confirmed to exist in
mainline source. However, the tool has NOT been tested on live hardware at the
time of writing. The following needs verification:

1. Vendor command reaches firmware successfully
2. btc_mode get returns a valid value
3. btc_mode set changes coexistence behaviour
4. nl80211 vendor data attribute parsing matches kernel response format
5. Error handling for edge cases (interface down, driver not loaded)

The most likely adjustment area is the response_handler parsing of nested NLA
attributes within NL80211_ATTR_VENDOR_DATA. The exact nesting structure
depends on cfg80211_vendor_cmd_reply() implementation.


## References

Kernel source:

- drivers/net/wireless/broadcom/brcm80211/brcmfmac/vendor.c (handler)
- drivers/net/wireless/broadcom/brcm80211/brcmfmac/vendor.h (structures)
- drivers/net/wireless/broadcom/brcm80211/brcmfmac/fwil.h (command IDs)
- drivers/net/wireless/broadcom/brcm80211/brcmfmac/core.c (net_device_ops)

Kernel commits:

- "brcmfmac: remove redundant ioctl handlers" - Arend van Spriel, 2013-11-29
- "net: split out SIOCDEVPRIVATE handling from dev_ioctl" - Arnd Bergmann, 2021

Community and vendor:

- Infineon community: CYW43455 btc_mode and btc_params discussion
  https://community.infineon.com/t5/AIROC-Wi-Fi-Bluetooth-Combo/
- Raspberry Pi Forums: "Potential fix for bluetooth audio/wifi throughput
  issues" (btc_mode=4 discovery by community user)
  https://forums.raspberrypi.com/viewtopic.php?t=367947
- raspberrypi/linux GitHub issues: #1552, #1444, #5293 (coexistence reports)
- RPi-Distro/firmware-nonfree: NVRAM file with btc parameters
- Quarkslab: Broadcom WiFi firmware reverse engineering (ioctl protocol)
- Nexmon project (seemoo-lab): brcmfmac firmware patching (internal mechanisms)
- Argenox: Raspberry Pi antenna analysis
