# NZXT Signal 4K30 color bug firmware patch

This is a firmware fix for a bug I observed on my NZXT Signal 4K30 HDMI capture device. The bug manifests only in certain situations. When it does, the captured video shows up with completely incorrect colors. There is a pink/green tint instead of the proper colors. In my case, the 4K30 worked fine with most source devices like my iPad or laptop, so I knew the capture card itself was working okay and simply had an incompatibility bug with some devices. I had also seen other people on Reddit ([1](https://www.reddit.com/r/NZXT/comments/17csgqa/picture_is_green_and_pink_from_signal_4k30/), [2](https://www.reddit.com/r/obs/comments/16l8efg/pink_and_green_screen/)) complaining about the exact same symptom, with sample images of the problem in action. Here is my own example:

![Macaws with an incorrect green/pink tint](sample_images/yuv422.jpg)

This is kodim23.png from the [Kodak Lossless True Color Image Suite](https://r0k.us/graphics/kodak/), displayed from a source device exhibiting this symptom.

I asked Claude Opus 5 to analyze NZXT's latest firmware update for this device, and also pointed it to [ITE's IT6805 driver source code](https://github.com/Qiuzixing/IP5000-A302/tree/b7f61f612672301979b6f062129f8ffd0163c45d/modules/ast_modules/it6805), in hopes of locating the problem. I also showed it the example images of the problem in action from the Reddit threads, and told it that I assumed the problem involved some sort of RGB/YUV mismatch because that has typically been the cause of this kind of issue when I've worked with video decoder/encoder devices in the past.

## The bug

Claude almost immediately came up with some theories, and after some testing and guidance from me, it found the problem. It's plainly visible in ITE's [IT6805_SYS.c](https://github.com/Qiuzixing/IP5000-A302/blob/b7f61f612672301979b6f062129f8ffd0163c45d/modules/ast_modules/it6805/iTE6805_SYS.c) source file. Here are the relevant lines, with other stuff filtered out:

```c
// REG6B[5:4]: Reg_ColMod_Set Input color mode set 00: RGB mode - 01: YUV422 mode, 10: YUV444 mode, 11: YUV420 mode
chgbank(0);
if (iTE6805_Check_HDMI_OR_DVI_Mode(iTE6805_DATA.CurrentPort) == MODE_HDMI)
{
    HDMIRX_DEBUG_PRINT(("---- CSC HDMI mode ----\n"));
    ...
    hdmirxset(0x6B, 0x30, iTE6805_DATA.AVIInfoFrame_Input_ColorFormat << 4);// seting input format by info frame ??? do not need ???
    ...
}
else
{
    ...
    HDMIRX_DEBUG_PRINT(("---- CSC DVI mode ----\n"));
    hdmirxset(0x6B, 0x30, 0x10);                        // seting input format to RGB
    ...
}
```

The comment at the top says that register 0x6B's bits 5:4 mean the following about the input color format:

- `00` = RGB
- `01` = YUV 4:2:2
- `10` = YUV 4:4:4
- `11` = YUV 4:2:0

If the input is detected as an HDMI input, that setting is lifted directly out of the AVI InfoFrame and passed onto the chip. That logic is fine.

On the other hand, if the input is detected as a DVI input, which doesn't have any of the special in-band data like the InfoFrame, the incoming data is supposed to be interpreted as RGB. The comment `seting input format to RGB` even hints at that, but it writes 0x10 to the register, which means bits 5:4 are `01`, incorrectly configuring the chip for YUV 4:2:2 mode even though the data is going to be in RGB mode.

This is the bug. It's not even NZXT's bug. It's in ITE's driver that they dropped into their code. The comment is correct, but the code doesn't do what the comment says it should do. My guess is the original ITE developer looked at the comment at the top and accidentally saw `RGB mode - 01` instead of `00: RGB mode`.

I believe DVI sources are pretty rare these days. Most modern sources are HDMI, not DVI, but it's still possible that an HDMI source device might fall back to DVI signaling depending on what kind of display it's plugged into. The capture card copies the EDID of the monitor attached to its passthrough port, so the attached passthrough monitor could be a factor in which signaling is chosen by the source device. This could explain what happened in the Reddit reports with the Nintendo Switch and PlayStation 5, but I don't know that for a fact. I only know that it fixed my particular issue with DVI source signaling.

## The fix

The fix is very simple: we just have to change the code to say `hdmirxset(0x6B, 0x30, 0x00)` instead, which changes the configured colors to RGB instead of YUV422. This is a one-byte patch in NZXT's firmware: change the instruction at offset 0xB8D4 from `movs r2, #16` (`0x10 0x22`) to `movs r2, #0` (`0x00 0x22`). So in summary, we just need to change the byte at 0xB8D4 from `0x10` to `0x00` in the `NZXT4K30_MM32_APP_v0E.07.80.0202_20220615.bin` file.

Luckily, the firmware update mechanism doesn't seem to implement any kind of a checksum on the MCU firmware, so that's the only change required to fix this problem. With the fix applied, here is what the capture looks like now:

![Macaws with the correct colors](sample_images/rgb.jpg)

I used Claude Opus 5 to develop a simple command-line flasher utility that patches the firmware and uploads it to the device. This was necessary because NZXT's firmware updater refuses to install the patched firmware, claiming the device is already up to date.

## How to install the fix

**I am not affiliated with NZXT or ITE in any way. Do this at your own risk. It worked for me, but you are updating the firmware of your card and there is a chance you could brick it if the update process goes wrong. This is only for the 4K30, and not any other capture devices by NZXT or any other manufacturer. Do not unplug the card while it's flashing.**

1. Download and unzip NZXT's latest firmware update for the 4K30. As of this writing, the link is here: https://support.nzxt.com/hc/en-us/articles/35642755429019-Signal-4K30-Downloads. You should end up with an extracted folder called `1734332551-nzxt-capture-card-updater`.

2. Download `mcu_flash.exe` from the [Releases](https://github.com/dougg3/nzxt-signal-4k30-color-bug-firmware-patch/releases) page on GitHub, and save it directly inside of the `1734332551-nzxt-capture-card-updater` directory next to all of the other NZXT files, including `CTITSDKDeviceTool.dll` and `setting.cfg`.

3. Open up a Windows command prompt in the `1734332551-nzxt-capture-card-updater` folder. I closed all video capture programs and unplugged the HDMI cables from the device so only the USB-C cable was plugged in, just to be safe.

4. Run this command in the Windows command prompt: `mcu_flash.exe --patch-dvi-rgb NZXT4K30_MM32_APP_v0E.07.80.0202_20220615.bin`

5. When prompted, type `YES` in all caps and hit enter to confirm you want to install the update.

6. After the update is complete, unplug and replug the capture device. Now the bug should be fixed!

If you ever want to restore the device back to the factory firmware that has the bug, run the same command, but remove `--patch-dvi-rgb`.

## License

This project is licensed under the MIT license. See the [LICENSE](LICENSE) file for more details.

## Acknowledgements

- ITE's IT6805 SDK, which is available in a few random GitHub repositories out there, made it easy to find the actual underlying bug.
- Claude Opus 5 helped find the bug and wrote the `mcu_flash` utility. I wrote the documentation by hand.
- Claude used [Ghidra](https://github.com/nationalsecurityagency/ghidra) for disassembling the firmware.
- I used kodim23 from the [Kodak Lossless True Color Image Suite](https://r0k.us/graphics/kodak/) to demonstrate the color issue.
