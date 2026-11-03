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

## 4. Langkah Kerja Berikutnya (Action Plan)

1. **Eliminasi Total Popup Caution 43**:
   - Terapkan bypass universal di `0x195cc98` (hook `browser::CautionManager::request`) via shim atau patch agar semua pemanggilan Caution 43 diabaikan.
   - Ambil snapshot verifikasi untuk memastikan layar 100% bersih tanpa notifikasi error.
2. **Verifikasi Touchscreen & Navigasi**:
   - Jalankan script pengujian touch pada tombol *"Cloud Log In"* dan source tabs.
   - Pindahkan view ke Browse, Waveform, dan Menu / Utility.
   - Simpan snapshot setiap view ke `tests/reports/snapshots/`.
3. **Eksekusi Full Automated QA Test Suite**:
   - Uji Touchscreen & USB
   - Uji Navigasi Views
   - Uji Audio Output ALSA
   - Uji DDJ-FLX6 HID & MIDI
   - Uji Pro DJ Link Packet Emission
4. **Finalisasi Laporan QA**:
   - Tulis laporan lengkap di `tests/reports/qa_test_report.md` beserta embedding snapshot dan log hasil eksekusi.
