# Testing checklist

This tool has been compiled and the kernel interface verified in mainline
source, but has NOT been tested on live hardware at the time of initial
commit.

This checklist tracks first hardware verification.


## Prerequisites

- Raspberry Pi with brcmfmac WiFi (Pi Zero W, Pi Zero 2 W, Pi 3B+, 4B, 5, CM4, CM5, or other BCM43430/CYW43455 boards)
- Root access
- brcmfmac driver loaded and wlan0 interface up
- libnl-3-200 and libnl-genl-3-200 installed (standard on Volumio 4 / Bookworm)


## Step 1: Deploy binary

Use the binary that matches your platform (from Docker or local build):

- **32-bit Volumio (Pi 0–4):** `out/armhf/brcm-iovar`
- **64-bit (Pi 3/4/5):** `out/arm64/brcm-iovar`
- **Pi Zero / Pi 1:** `out/armv6/brcm-iovar`

```
scp out/armhf/brcm-iovar root@volumio.local:/usr/local/bin/brcm-iovar
ssh root@volumio.local chmod +x /usr/local/bin/brcm-iovar
```

Verify library dependencies are satisfied:

```
ldd /usr/local/bin/brcm-iovar
```

Expected: libnl-3.so.200, libnl-genl-3.so.200 present.


## Step 2: Read btc_mode (basic functionality)

```
brcm-iovar wlan0 get_int btc_mode
```

Expected result: `btc_mode = 1` (default value from NVRAM)

- [ ] Command completes without error
- [ ] Returned value matches NVRAM default (1)
- [ ] No kernel errors in dmesg


## Step 3: Verify with dmesg

```
dmesg | tail -20
```

- [ ] No brcmfmac errors after get_int
- [ ] No vendor command rejection messages
- [ ] No firmware crash indicators


## Step 4: Write btc_mode

```
brcm-iovar wlan0 set_int btc_mode 4
```

- [ ] Command completes without error
- [ ] WiFi connection remains active (no disruption)
- [ ] SSH session (if over WiFi) stays connected


## Step 5: Read back after write

```
brcm-iovar wlan0 get_int btc_mode
```

Expected result: `btc_mode = 4`

- [ ] Value matches what was written
- [ ] Confirms firmware accepted the change


## Step 6: Verify WiFi still functional

```
ping -c 5 8.8.8.8
iw dev wlan0 link
```

- [ ] Ping succeeds
- [ ] WiFi association unchanged
- [ ] No packet loss increase


## Step 7: Verify Bluetooth still functional

If Bluetooth is active:

```
hciconfig hci0
bluetoothctl show
```

- [ ] BT adapter still up
- [ ] No connection drops


## Step 8: Revert btc_mode

```
brcm-iovar wlan0 set_int btc_mode 1
brcm-iovar wlan0 get_int btc_mode
```

- [ ] Reverts to mode 1
- [ ] WiFi and BT still functional


## Step 9: Error handling verification

Test with invalid interface:

```
brcm-iovar eth0 get_int btc_mode
```

- [ ] Returns meaningful error (not vendor command supported on non-brcmfmac)

Test with interface down:

```
ip link set wlan0 down
brcm-iovar wlan0 get_int btc_mode
ip link set wlan0 up
```

- [ ] Returns error, does not crash

Test with invalid iovar:

```
brcm-iovar wlan0 get_int nonexistent_iovar_xyz
```

- [ ] Returns firmware error code, does not crash


## Step 10: Cross-platform verification

Repeat steps 2-8 on each available board:

- [ ] Pi 3B+ (CYW43455, SDIO)
- [ ] Pi 4B (CYW43455, SDIO)
- [ ] Pi 5 (CYW43455, PCIe)
- [ ] CM4 (CYW43455, SDIO)
- [ ] Pi 3B (BCM43430, SDIO) - btc_mode may behave differently
- [ ] Pi Zero 2 W (BCM43430, SDIO)


## Known potential issues

1. Response parsing: The response_handler parses nested NLA attributes within
   NL80211_ATTR_VENDOR_DATA. If the kernel's cfg80211_vendor_cmd_reply()
   nesting differs from what the handler expects, the get_int return value
   may be incorrect or the handler may report ENODATA. Check with:
   `brcm-iovar wlan0 get_int btc_mode` and compare against the NVRAM
   default.

2. Byte order: The firmware returns values in little-endian. On ARM (all Pi
   models) this matches native byte order. If ever used on big-endian, the
   value parsing would need le32toh().

3. Permission: Requires CAP_NET_ADMIN. If running as non-root user without
   capabilities, the nl80211 command will be rejected by the kernel.

4. brcmfmac_wcc: On Pi 5, the companion module brcmfmac_wcc is loaded
   alongside brcmfmac. This should not affect vendor command operation but
   is noted for completeness.


## Reporting results

When reporting test results, include:

```
uname -a
cat /proc/device-tree/model
dmesg | grep brcmfmac | head -10
brcm-iovar wlan0 get_int btc_mode
```

If something fails, include the full dmesg output after the failed command.
