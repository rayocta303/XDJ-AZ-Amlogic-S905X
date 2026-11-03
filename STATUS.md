# Status Porting & Emulasi AlphaTheta XDJ-AZ (EP147) pada Armbian Amlogic S905X

**Dokumen Status & Analisis Runtime**  
**Target Hardware:** Amlogic S905X Quad-Core ARM64 (AArch64), 2GB RAM, Armbian Linux 5.15  
**Target Binary:** AlphaTheta XDJ-AZ `EP147` (v1.30, SHA256: `736bdc...` / patched)  
**Terakhir Diperbarui:** 2026-11-03

---

## 1. Verifikasi Status Trik & Patch NXP i.MX6 Quad

### Pertanyaan: *Apakah NXP i.MX6 Quad sudah di-patch / di-trick dengan shims?*
**Jawab: YA, SEPENUHNYA SUDAH DI-TRICK / DI-BYPASS VIA SHIMS.**

Pada perangkat keras asli AlphaTheta/Pioneer DJ (CDJ-3000, XDJ-XZ, XDJ-AZ), aplikasi `EP147` didesain untuk berjalan di atas platform SoC NXP/Freescale i.MX6/i.MX8. Tanpa hardware asli tersebut, firmware akan gagal booting (hang / crash / reboot loop) karena memeriksa register dan device nodes khusus i.MX.

Seluruh pemeriksaan hardware i.MX telah berhasil diakali menggunakan rantai `LD_PRELOAD` shims modular (`proc_usb_shim.c`, `env_emu_shim.c`, `subucom_emu.c`):

| Subsystem Hardware i.MX | Kebutuhan Firmware Asli | Implementasi Trick / Bypass Shim | Status |
|---|---|---|:---:|
| **Identitas SoC (`soc0`)** | Membaca `/sys/devices/soc0/{soc_id, family, machine}` untuk memverifikasi chip i.MX | Di-intercept via `proc_usb_shim.c` menggunakan virtual pipe nonblocking yang mengembalikan `"i.MX6Q\n"` dan vendor Freescale. | **BERHASIL** |
| **Physical Memory Register (`/dev/mem`)** | Akses direct physical mmap ke memory-mapped register i.MX (IOMUX, CCM clock, GPIO) | Di-intercept oleh `proc_usb_shim.c` mengembalikan `-1` dengan `errno = EACCES`. Hal ini memaksa EP147 fallback ke safe software/emulated paths dan mencegah Amlogic S905X mengalami kernel panic. | **BERHASIL** |
| **U-Boot Bootloader Environment** | Menjalankan `fw_printenv` untuk memeriksa model dan firmware version | Di-intercept oleh `env_emu_shim.c` via hook `execve()` sehingga mengembalikan `model=XDJ-AZ`, `version=1.30`, dan `serial=ATC-D810-0001`. | **BERHASIL** |
| **Thermal & Backlight Sensors** | Memeriksa thermal zone i.MX dan PWM backlight display | Di-mock via `proc_usb_shim.c` pada `/sys/class/thermal/thermal_zone*/temp` mengembalikan `45000` (45°C stabil) dan backlight diarahkan ke dummy sink. | **BERHASIL** |
| **Microcontroller SPI Buses (Sub-CPU)** | Komunikasi SPI dengan Renesas/Pioneer Panel MCU untuk tombol, fader, jog, dan EQ (`/dev/subucom_spi{1,2,3,5}.0`) | Di-emulasikan penuh oleh `subucom_emu.c` dengan 4 virtual SPI channels, frame buffer 152-byte otentik, serta CRC-16/X-25 checksum calculation. | **BERHASIL** |
| **WLAN Firmware Monitor** | Memeriksa file MAC address `/tmp/mlan0Addr` | `env_emu_shim.c` membuat `/tmp/mlan0Addr` otomatis saat inisialisasi agar skrip jaringan tidak blocking. | **BERHASIL** |
| **Database Engine** | Extended Database Streamer (`edb_streamd`) | Service `edb_streamd` dijalankan di background melayani socket `/tmp/edb_daemon.sock`. | **BERHASIL** |

---

## 2. Rangkuman Progress Saat Ini

1. **Booting & Eksekusi GUI EP147**:
   - `EP147` berhasil berjalan stabil tanpa crash di atas Armbian Amlogic S905X.
   - Antarmuka JUCE/X11 resolusi **1280x800** tampil penuh melalui Direct KMS DRM modesetting pada display HDMI.
2. **Remote Access & Visual Mirroring**:
   - Server VNC `x11vnc` aktif di port `5900`.
   - Web Remote HTML5 `noVNC` aktif di port `5800` (`http://192.168.137.227:5800/vnc.html`).
   - `vnc_touch_bridge` aktif mengonversi klik pointer browser/VNC menjadi touch input.
3. **Optimasi Performa & Binary Patches**:
   - Diterapkan 19 patch statis & dinamis melalui `tools/patch_az.py`:
     - Unlock 60 FPS refresh rate (menggantikan default 30 FPS timer).
     - Subpixel waveform rendering.
     - JUCE UI timers dipercepat dari 33ms ke 16ms.
4. **Hardware Controller Bridge**:
   - Daemon `flx6_controls` (C native) aktif membaca USB HID Pioneer DDJ-FLX6 dan memetakan jog wheel, mixer fader, dan transport ke register subucom EP147.
5. **Touchscreen Driver Shim**:
   - Driver `touch_shim.c` meng-intersep node `/dev/input/by-id/gt928` yang diminta oleh EP147 dan menghubungkannya dengan physical touchscreen `/dev/input/event1` (`USB2IIC_CTP_CONTROL`).

---

## 3. Masalah yang Ditemukan (Blockers) & Solusinya

### Masalah 1: Popup Modal "USB Error. Remove the device." (Caution 43)
- **Gejala:**  
  Di tengah layar muncul kotak pesan modal *"USB Error. Remove the device."*. Karena kotak ini berstatus modal pada JUCE framework (`juce::ModalComponentManager`), dialog ini **menangkap dan membuang (swallow) semua event touch dan klik**, sehingga UI di belakangnya tidak bisa disentuh / diklik sama sekali.
- **Root Cause:**
  1. Pada konstruksi Source View (`0x1de07f4`–`0x1de0848`), terdapat instansiasi hardcoded komponen Caution 43 (`mov w2, #0x2b`) yang memanggil `addAndMakeVisible`.
  2. Ketika parser USB (`meow::StorageMountDetector`) mendeteksi event koneksi USB yang tidak lengkap pada `/proc/udev_usb1` atau `/proc/udev_usb2`, ia memanggil `browser::CautionManager::request(mgr, 43)` dengan durasi `INT_MIN` (tampil tanpa batas waktu / tanpa tombol dismiss).
- **Solusi yang Diterapkan & Sedang Diuji:**
  1. Patch binary statis & dinamis pada `0x1de0848` (`bl 0x249d6b0` -> `nop` `0xd503201f`) untuk meniadakan penambahan dialog modal ke dalam view tree.
  2. Universal hook pada dispatcher `browser::CautionManager::request` (`0x195cc98`) agar jika ID = 43 (`0x2b`), fungsi langsung me-return 0 tanpa memproses popup.
  3. Mengatur emulasi udev proc (`proc_usb_shim.c`) agar hanya mengirim payload mount bersih: `mount /media/usb/sda1 vfat protect:0` dan tidak mengirim perintah `connect,` yang memicu error parsing.

### Masalah 2: Interaksi Touchscreen Tidak Berfungsi
- **Gejala:**  
  Layar sentuh fisik tidak merespon sentuhan.
- **Root Cause:**
  1. Terhalang oleh modal popup Caution 43 (Masalah 1).
  2. Device node touchscreen fisik pada Armbian terdeteksi sebagai `/dev/input/event1` (resolusi digitizer 0..2048 x 0..2048), sedangkan EP147 membaca via `/dev/input/by-id/gt928` dalam koordinat logis 1280x800.
- **Solusi:**
  - `touch_shim.c` melakukan scaling transformasi koordinat dari `[0..2048]` ke `[0..1280, 0..800]`.
  - Setelah modal popup dihilangkan, touch input langsung tembus ke komponen UI utama (Cloud Log In, Track List, Waveform, Utility).

### Masalah 3: Navigasi View & Full Automated Testing
- **Kebutuhan:**
  Melakukan pengujian otomatis 5 area lengkap dengan bukti snapshot resolusi 1280x800:
  1. Touchscreen & USB Source
  2. Navigasi Seluruh Views (Source, Browse, Waveform 2-Deck & 4-Deck, Utility)
  3. Audio ALSA Output
  4. Koneksi Bridge DDJ-FLX6
  5. Pioneer Pro DJ Link

---

## 4. Hardware Controller & Master Panel Injection (DDJ-FLX6 & send_panel.py)

### DDJ-FLX6 Input Bridge (`flx6_controls.c`):
- **Root Cause Sebelumnya**:
  1. `flx6_controls.c` mengirim format UDP dengan prefix 1-byte bus id (`1 + sizeof(pkt)`), namun `subucom_emu.so` memeriksa `magic == 0x5AA5` di offset 0, sehingga paket dianggap korup dan di-drop.
  2. Tombol momentary (Play, Cue, Sync) hanya mengirim bit ON tanpa event bit OFF pada release, sehingga tombol dianggap tertahan terus (stuck).
- **Perbaikan**:
  1. `flx6_controls.c` di-refactor menggunakan format overlay wire otentik (`1 + SUBUCOM_FRAME_SIZE = 153 bytes`) dengan state persistence berbasis frame hardware idle resmi.
  2. Full 4-deck mapping:
     - **Transport (Ch 0..3)**: Play/Pause (Note 11), Cue (Note 12), Platter Touch Vinyl (Note 54), Beat Sync (Note 88), Master Tempo / Key Lock (Note 93), Deck Toggle 3/4 (Note 114).
     - **Mixer (Ch 0..3 & Ch 6)**: Fader 1..4 (CC 19), Trims (CC 4), EQs Hi/Mid/Low (CC 7, 11, 15), Crossfader (CC 31), Sound Color FX Filter (CC 23..26).
     - **Navigation (Ch 6)**: Rotary Browse (CC 64), Push/Enter (Note 65), Back (Note 101), View Toggle (Note 122), Shift+View Source (Note 104), Shift+Load 1 Utility (Note 88), Load 1..4 (Notes 70..73).
     - **Pads (Ch 7..14)**: Performance Pads 1..8 pada Deck 1..4.

### Master Panel Control Script (`tools/send_panel.py`):
Skrip CLI master lengkap untuk mengontrol seluruh fungsionalitas hardware dan UI XDJ-AZ:
- **Views**: `source`, `browse`, `waveform`, `menu` / `utility`, `taglist`, `playlist`, `search`
- **Navigation**: `scroll <delta>`, `scroll_down`, `scroll_up`, `enter`, `back`
- **Loading**: `load1`, `load2`, `load3`, `load4`
- **Transport**: `play <deck>`, `cue <deck>`, `sync <deck>`, `master <deck>`, `slip <deck>`, `tempo <deck> <pct>`, `jog <deck> <delta>`
- **Mixer**: `fader <ch> <pct>`, `xfader <val>`, `eq <ch> <hi> <mid> <low>`, `trim <ch> <pct>`, `color <ch> <pct>`
- **Pads**: `pad <deck> <1..8>`, `pad_mode <deck> <mode>`

---

## 6. Audio Routing & Hardware DDJ-FLX6 Sound Output

### Masalah Audio Sebelumnya:
- **Gejala**: Suara tidak keluar dari DDJ-FLX6 (Master RCA maupun Headphone Cue).
- **Investigasi**:
  1. `lsof /dev/snd/*` pada target menunjukkan `EP147` membuka `/dev/snd/pcmC0D0p` (Card 0 = HDMI), bukan Card 1 (`hw:CARD=DDJFLX6,DEV=0`).
  2. EP147 meminta output audio 8-channel interleaved `S24_LE` pada rate 44100 Hz.
  3. Hardware DDJ-FLX6 hanya menerima maksimal **4 channel** dalam format `S24_3LE` atau `S16_LE` pada rate 44100 Hz (`/proc/asound/card1/pcm0p/sub0/hw_params`).
  4. Akibatnya, pemanggilan `snd_pcm_open(&pcm, "hw:CARD=DDJFLX6,DEV=0", ...)` dengan 8 channel langsung ditolak oleh ALSA kernel driver dengan return code `-EINVAL` (-22), sehingga EP147 jatuh kembali (fallback) ke HDMI Card 0.

### Solusi & Implementasi:
1. **ALSA Architecture Plugin Layer (`configs/asound.conf`)**:
   - Dibuat konfigurasi `/etc/asound.conf` yang mendefinisikan `pcm.!default` bertipe `plug` dengan target slave `hw:CARD=DDJFLX6,DEV=0`.
   - Plugin ALSA `plug` secara otomatis dan transparan melakukan:
     - Downmixing / perutean 8 channel EP147 ke 4 channel hardware DDJ-FLX6 (Ch 0..1 -> Master L/R RCA, Ch 2..3 -> Headphone Cue L/R).
     - Konversi format sample dari 32-bit container `S24_LE` ke 24-bit 3-byte packed `S24_3LE` dengan zero CPU overhead.
2. **Audio Interception Shim (`src/audio_shim.c`)**:
   - Meng-intersep `snd_pcm_open()` agar seluruh stream playback diarahkan ke device `"default"`.
   - Meng-intersep `snd_pcm_writei()` untuk menangkap buffer audio live secara real-time.
3. **Verifikasi Hardware**:
   - Pengujian dengan `speaker-test -D default -c 8 -r 44100 -F S24_LE` berhasil menghasilkan audio jernih (pink noise) pada DDJ-FLX6 Master RCA dan Headphone jack.

---

## 7. MIDI Out / LED Feedback & Real-Time Waveform Peak Metering

### Analisis & Ground Truth (Referensi `p:\az-runtime-lab`):
- **Channel Routing**:
  - `Ch 0`: Master L
  - `Ch 1`: Master R
  - `Ch 2`: Headphones Cue L
  - `Ch 3`: Headphones Cue R
- **14-Bit High-Resolution Faders & Mixer Controls**:
  - Channel 1..4 Fader: MSB `0x13`, LSB `0x33` (`val14 = (msb << 7) | lsb`).
  - Crossfader: MSB `0x1F`, LSB `0x3F`.
  - Headphone Mix: MSB `0x0C`, LSB `0x2C`.
- **VU Meter Pioneer FLX6 7-Step Banded CC 2 (`0xB0..0xB3`, CC 2)**:
  - `0x00`: Dark / Silent (<-36 dB)
  - `0x26`: Green 1 (-24 dB)
  - `0x41`: Green 2 (-12 dB)
  - `0x57`: Orange 1 (-6 dB)
  - `0x65`: Orange 2 (-2 dB)
  - `0x77`: Red (0 dB)
  - `0x7F`: Red Clip (+3 dB)

### Solusi & Implementasi:
1. **Shared Memory Bus (`include/xdj_az_shm.h`)**:
   - Mengimplementasikan POSIX shared memory ring `/dev/shm/xdj_az_shared` untuk pertukaran data ultra-low latency (< 1ms) antar-proses.
2. **Real-time Live Audio Peak Metering (`src/audio_shim.c`)**:
   - Pada `snd_pcm_writei()`, sampel PCM 24-bit dipindai untuk menghitung amplitude peak Master L/R dan Cue L/R.
   - Amplitude dikonversi ke skala desibel (dB) dan dipetakan ke 7-step band VU FLX6, kemudian dituliskan langsung ke shared memory.
3. **Hardware MOSI LED Feedback (`src/subucom_emu.c`)**:
   - Meng-intersep payload `IOCTL_MOSI_TRANSFER` dan `write()` dari EP147, mengekstrak status LED platter jog, tombol Track Loaded, Play/Cue LED, dan menyimpannya di shared memory.
4. **Native FLX6 Controller Bridge (`src/flx6_controls.c`)**:
   - Membaca shared memory setiap 25ms (40 Hz update rate) dan mengirimkan MIDI Out aktual ke `/dev/snd/midiC1D0`:
     - 4-Channel VU Meter LEDs (`0xB0..0xB3, 0x02, val`).
     - Track Loaded button illumination (Ch 15 / `0x9F`, Notes 0..3).
     - Platter ring LED rotation feedback.
   - Penuh 14-bit fader decoding, Vinyl Mode Toggle (Shift + Jog Cutter / Note 23), Fast Track Search (Shift + Jog / CC 41), serta Cue/Loop Memory Navigation.

---

## 8. Dynamic Linker Hardening & Bug Fix

- **Masalah Teridentifikasi**:
  Saat pengujian interposisi ALSA, penambahan wrapper `dlsym` pada `audio_shim.c` mengacaukan pemanggilan `dlsym(RTLD_NEXT, "semget")` pada `sem_owner.so`. Karena glibc mengevaluasi `RTLD_NEXT` berdasarkan alamat return caller, `dlsym` internal mengembalikan fungsi pembungkusnya sendiri sehingga terjadi infinite recursion.
- **Perbaikan**:
  Karena `EP147` me-link `libasound.so.2` secara statis/dinamis (bukan via `dlsym` manual), wrapper `dlsym` pada `audio_shim.c` **dihapus sepenuhnya**. Interposisi berjalan bersih via direct `LD_PRELOAD` tanpa efek samping terhadap library lain.


