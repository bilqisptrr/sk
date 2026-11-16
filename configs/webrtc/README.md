# Konfigurasi kandidat untuk fork WebRTC

Enam file `.txt` di folder ini adalah **input pencarian**, bukan kode WebRTC
atau network hasil siap pakai. Pencarian masih acak dan berjalan terus tanpa
`--timeout`. Contoh:

```sh
make
./SorterHunter configs/webrtc/feedback_sort_8.txt --timeout 30
./SorterHunter configs/webrtc/rtcp_clock_median_7.txt --timeout 30
```

Titik kode di bawah dicek pada snapshot mirror WebRTC commit
[`3710345e71ad3a7c1f4ed4efef152fd4fc24869a`](https://github.com/webrtc-mirror/webrtc/tree/3710345e71ad3a7c1f4ed4efef152fd4fc24869a).
Fork atau versi pengguna bisa berbeda. Pilihan ukuran 4 dan 8 di bawah **bukan**
klaim ukuran batch default WebRTC; ukur distribusi ukuran dan hotspot di fork
sebelum membuat fast path.

| Konfigurasi | Titik kode | Yang dijamin / syarat |
|---|---|---|
| `feedback_sort_4.txt`, `feedback_sort_8.txt` | [`TransportPacketsFeedback::SortedByReceiveTime()`](https://github.com/webrtc-mirror/webrtc/blob/3710345e71ad3a7c1f4ed4efef152fd4fc24869a/api/transport/network_types.cc), dipakai GoogCC dan SCReAM | Urut penuh tepat 4 atau 8 paket **yang diterima**; fallback untuk ukuran lain. Comparator asli: receive time, send time, sequence number. Swap seluruh `PacketResult`, bukan hanya timestamp. Urut per chunk tanpa merge tidak cukup. |
| `rtcp_clock_median_7.txt` | [`RemoteNtpTimeEstimator`](https://github.com/webrtc-mirror/webrtc/blob/3710345e71ad3a7c1f4ed4efef152fd4fc24869a/modules/rtp_rtcp/source/remote_ntp_time_estimator.cc) | Median offset tujuh nilai arbitrer pada wire 3. Window maksimum aktual 7; pertahankan FIFO untuk menghapus nilai tertua. Startup berukuran 0–6 tetap perlu implementasi lain. |
| `rtcp_clock_insert_7.txt` | Titik RTCP yang sama, pendekatan alternatif | Urut penuh dari **6 nilai yang sudah terurut** setelah penghapusan nilai tertua plus 1 nilai baru. Tetap perlu FIFO terpisah, penanganan duplikat, dan fallback saat window belum penuh. Bukan untuk 7 input acak. |
| `neteq_waiting_stats_8.txt` | [`StatisticsCalculator::GetNetworkStatistics()`](https://github.com/webrtc-mirror/webrtc/blob/3710345e71ad3a7c1f4ed4efef152fd4fc24869a/modules/audio_coding/neteq/statistics_calculator.cc) | Hanya saat jumlah waiting time tepat 8: wire 0, 3, 4, 7 adalah min, dua nilai tengah, max. Hitung median `(wire[3]+wire[4])/2` dan mean sesuai kode asli. Wire lain tidak dijamin terurut; jumlah lain termasuk kosong perlu fallback. |
| `video_order_3.txt` | [VP8 `UpdateSearchOrder`](https://github.com/webrtc-mirror/webrtc/blob/3710345e71ad3a7c1f4ed4efef152fd4fc24869a/modules/video_coding/codecs/vp8/default_temporal_layers.cc) dan [simulcast allocator](https://github.com/webrtc-mirror/webrtc/blob/3710345e71ad3a7c1f4ed4efef152fd4fc24869a/modules/video_coding/utility/simulcast_rate_allocator.cc) | Full sort tepat 3 record; fallback untuk jumlah 0–2. VP8 membandingkan usia lalu ID buffer. Simulcast memakai **stable sort** berdasarkan max bitrate: gunakan `(maxBitrate, original_index)` agar tie tetap sesuai urutan semula. |

## Yang sengaja tidak dipaksakan

- `RobustThroughputEstimator` mencari dua gap terbesar secara linear dan
  mempertahankan window paket terurut secara inkremental. `TrendlineEstimator`
  juga memakai insertion pada window yang ukurannya dapat berubah. Sorting
  network full-size belum tentu lebih baik untuk keduanya.
- Filter persentil di `DecodeTimePercentileFilter` memakai window berbasis waktu
  dengan jumlah sampel variabel. `JitterEstimator` memiliki window frame size
  default 300 dan opsi percentile; itu bukan window tetap N=16. Jangan ubah
  window hanya agar cocok dengan config tanpa menerima perubahan algoritme.
- `NetEq` menampung hingga 100 waiting time; config N=8 **hanya** fast path
  ukuran 8, tidak menggantikan perhitungan untuk ukuran lainnya.
- Tidak semua layer audio/video perlu sorting. Config AGC atau filter spasial
  spekulatif bukan bukti bahwa ada call site production yang bisa diganti.

## Sebelum integrasi

1. Ambil jaringan `nw` hasil pencarian dan gunakan comparator yang benar,
   termasuk tie-break dan pemindahan payload. Input/output network dibaca dari
   wire indeks 0; hanya output yang disebut config dijamin benar.
2. Pertahankan fallback untuk semua ukuran dan kondisi yang tidak ditangani,
   termasuk startup, reset, duplikat, dan urutan data yang diperlukan caller.
   Untuk data floating-point, cek juga NaN, infinities, signed zero, dan
   semantics comparator/SIMD target; jaringan nilai biasa tidak otomatis
   membuktikan kesetaraan untuk kasus-kasus itu.
3. Uji diferensial terhadap fungsi WebRTC asli dan regresi end-to-end di fork.
   Benchmark call site lengkap—termasuk dispatch, comparator, perpindahan
   record, cache, dan frekuensi ukuran—di CPU target. Jumlah comparator/depth
   bukan bukti bahwa implementasi ini lebih cepat daripada kode asli.
4. `make test` menjalankan smoke test tiap config dan memvalidasi setiap network
   yang tercetak terhadap semua input biner (atau seluruh kelas input presorted
   untuk insertion), serta beberapa sampel bernilai negatif dan duplikat. Itu
   **bukan** pengganti uji integrasi dan benchmark WebRTC.
