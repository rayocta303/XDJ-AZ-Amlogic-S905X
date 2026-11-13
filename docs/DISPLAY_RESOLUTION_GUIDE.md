# AlphaTheta XDJ-AZ Display & Screen Resolution Deployment Guide
**Target Architecture:** Amlogic S905X (Meson GXL) · Armbian aarch64 Linux 5.15+  
**Target Applications:** AlphaTheta XDJ-AZ (`ep147`) & Pioneer DJ XDJ-RX3 (`rbp`)

---

## 1. Native Display Specifications

The standalone DJ engines are architected for a native **1280×800 (16:10 aspect ratio, 32bpp)** logical layout:
* **XDJ-AZ (EP147)**: Native UI viewport renders at `1280x800` via Xorg DRM KMS modesetting (`/dev/dri/card0`).
* **XDJ-RX3 (`rbp`)**: Native UI layer renders at `1280x800` via DirectFB (`/dev/fb0`).

---

## 2. Amlogic S905X Bootloader Contract (U-Boot vs Linux Kernel)

On Amlogic S905X TV Boxes running mainline U-Boot (`boot.cmd` / `boot.ini`), the kernel command line is built **strictly from the `APPEND` variable in `/boot/uEnv.txt`**.

> [!WARNING]
> Setting `extraargs=...` in `/boot/armbianEnv.txt` is **SILENTLY IGNORED** by mainline U-Boot on Amlogic platforms!
> You **MUST** append the video resolution parameters directly to the `APPEND=` line in `/boot/uEnv.txt`.

### Correct Configuration (`/boot/uEnv.txt`)
Ensure the `APPEND=` line includes:
```text
APPEND=root=... video=HDMI-A-1:1280x800@60 vt.global_cursor_default=0
```

* `video=HDMI-A-1:1280x800@60`: Forces the `meson-drm` driver to output native 1280×800 @ 60Hz timing over HDMI, bypassing generic fallback modes.
* `vt.global_cursor_default=0`: Suppresses the Linux virtual console blinking cursor timer (`fbcon`), preventing cursor flickering in the bottom-left corner of the DJ interface.

---

## 3. The Generic LCD Controller EDID Trap & Double Scaling

Many popular 7-inch and 10.1-inch IPS HDMI display panels (e.g. `JRP7813S`, generic Waveshare clones, or portable monitor boards) have factory EDID firmware that advertises `1024x600` as their primary Detailed Timing Descriptor (DTD).

If `/boot/uEnv.txt` lacks `video=HDMI-A-1:1280x800@60`:
1. The kernel DRM driver falls back to EDID's default: `1024x600`.
2. A catastrophic **double scaling** occurs:
   - **Step 1 (Software)**: The DJ engine (1280×800) is downscaled by the presenter to 1024×600.
   - **Step 2 (Hardware)**: The HDMI LCD controller board upscales the 1024×600 signal back to the panel's physical matrix.
3. **Symptoms**:
   - Small fonts (BPM, Key, Track Title, Elapsed Time) lose single-pixel strokes, appearing blurry, smeared, or jagged.
   - High software scaling overhead consumes CPU cycles, causing micro-stutters during 60 FPS waveform scrolling.

---

## 4. Root Cause of Beatgrid & Waveform Scrolling Flicker

When an image is downscaled from 1280 to 1024 using standard nearest-neighbor sampling:
$$\text{Column}_{\text{src}} = \left\lfloor x_{\text{out}} \times \frac{1280}{1024} \right\rfloor = \left\lfloor x_{\text{out}} \times 1.25 \right\rfloor$$

* $x_{\text{out}} = 0 \rightarrow \text{src} = 0$
* $x_{\text{out}} = 1 \rightarrow \text{src} = 1$
* $x_{\text{out}} = 2 \rightarrow \text{src} = 2$
* $x_{\text{out}} = 3 \rightarrow \text{src} = 3$
* $x_{\text{out}} = 4 \rightarrow \text{src} = 5$ $\rightarrow$ **Column 4 is skipped/discarded!**

### Why Only Scrolling Elements Flicker:
* **Static UI Elements**: Their pixel coordinates never change. A button or header line at column $X=2$ is always sampled by $x_{\text{out}}=2$ on every frame, so it never flickers.
* **Moving Waveform & Beatgrid Lines**:
  - A beatgrid line is a vertical hairline of **1-pixel width**.
  - As music plays, the waveform scrolls horizontally across the screen at 60 FPS:
    - At $X = 0, 1, 2, 3$: The hairline is rendered.
    - At $X = 4$: The line lands on the discarded column 4 $\rightarrow$ **the line vanishes completely for 1 frame**.
    - At $X = 5$: The line reappears.
  - This pattern repeats every 5 frames, generating a **12 Hz strobe ($60\text{ FPS} / 5$)** that human vision perceives as intense beatgrid flickering.

### The Fix:
Forcing native **1280×800** (or standard 720p **1280×720**) creates a **1:1 horizontal mapping ($1280 \rightarrow 1280$)**. Exactly 0 columns are discarded, completely eliminating waveform and beatgrid flickering.

---

## 5. Deployment Verification Checklist

Run these commands on the target board to verify the display pipeline:

1. **Check Effective Kernel Command Line**:
   ```bash
   cat /proc/cmdline
   ```
   *Verify that `video=HDMI-A-1:1280x800@60` is present.*

2. **Verify Framebuffer Geometry**:
   ```bash
   fbset -fb /dev/fb0
   ```
   *Expected output:*
   ```text
   mode "1280x800"
       geometry 1280 800 1280 800 32
   ```

3. **Check Xorg Modesetting (For XDJ-AZ)**:
   ```bash
   xrandr --display :0
   ```
   *Ensure the primary mode is connected at `1280x800`.*

4. **Verify DirectFB Log (For XDJ-RX3)**:
   ```bash
   cat /tmp/dfbdig9.log
   ```
   *Should show mode match without resampling overhead:*
   ```text
   PRESENT: mode=scale ... real_fb=1280x800 ... scale=1280x800 dx=0 dy=0 fit=stretch skip=1
   ```

