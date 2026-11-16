# SorterHunter-DSP — Pencarian Jaringan Spesifik Domain DSP WebRTC

Fork dari [SorterHunter](https://github.com/bertdobbelaere/SorterHunter) (Bert Dobbelaere, public domain) yang dirombak dari pencarian **sorter generik** menjadi pencarian **topologi jaringan khusus pipeline DSP WebRTC**. Seluruh perombakan mengikuti blueprint berikut:

| Modul DSP WebRTC | Kebutuhan Statistik | Topologi yang Diperebutkan | Mode |
|---|---|---|---|
| Jitter Buffer & GCC (delay/packet loss estimation) | Persentil ke-95/99, trimmed mean | **Selection/Quantile Network** — hanya indeks `[k_low..k_high]` yang dijamin benar | `SearchMode=1` |
| Audio denoising / click removal (impulse noise) | Median sliding window | **Sliding-Window Insertion Network** — N-1 sampel presorted + 1 sampel baru | `SearchMode=2` |
| AEC / VAD / AGC (envelope & noise floor) | Min/max tracking, kuantil bawah | **Asymmetric Partial Network** — isolasi K nilai terkecil/terbesar | `SearchMode=3` |
| Video spatial filter (3×3/5×5 denoising) | Median 2D + morfologi | **Multi-Output Shared-Prefix Network** — beberapa order statistic sekaligus + laporan kedalaman prefix yang bisa dibagi | `SearchMode=4` |

Mode klasik (full sorter, `SearchMode=0`) tetap utuh 100% — semua config lama tetap jalan.

---

## 1. Perombakan A: Penyempitan Zero-One Principle

### 1a. Kelas input terbatas untuk sliding window (`SorterHunter.cpp` + `prefix_processor.cpp`)

Kelas input sliding window adalah semua pola yang **monoton naik pada wire 0..P-1** (P = `PresortedCount`) dan **bebas pada wire `NewSampleWire`**. Fungsi baru `prepareTestVectorsSliding()` menghasilkan tepat `2·(P+1)` pola mentah → **±2N vektor uji** setelah dua fixpoint terurut dibuang, vs 2^N untuk sorter generik. Untuk N=9: 16 vektor vs 512.

Vektor uji dihitung dengan `computeRestrictedOutputs()` (`prefix_processor.cpp`): prefix (opsional) diterapkan langsung ke pola-pola kelas terbatas, bukan dienumerasi dari 2^N input. *Greedy prefix otomatis dinonaktifkan di mode ini* (greedy A meminimalkan pola sisa atas seluruh 2^N input — tidak relevan untuk kelas terbatas).

### 1b. Verifikasi kuantil per-indeks (`checkOutputGroup()` di `SorterHunter.cpp`)

Cek validitas digeneralisasi. Semula: scan transisi terlarang `1→0` di semua wire (sorter penuh). Kini:

- **Mode klasik/sliding tanpa target**: scan `1→0` penuh (perilaku asli).
- **Mode kuantil** (`useQuantileCheck=true`): untuk setiap wire target `k` dan setiap vektor uji, bit output diharuskan `1` iff `popcount(input) ≥ N-k` — persis kondisi ranked selection pada indeks `k`. Diterapkan bit-paralel: per grup 64 vektor uji cukup `data[k] XOR expectedMask[k] == 0`. *Expected mask* dihitung sekali saat persiapan vektor (`convertToBitParallelDSP()`), biaya verifikasi jadi O(T) per grup (T = jumlah target).

**Dasar teori (generalisasi zero-one principle untuk order statistic).** Untuk setiap indeks target `k`, argumen threshold menunjukkan: jika jaringan CE lolos untuk semua input 0-1 pada wire `k`, maka wire `k` memuat nilai peringkat ke-(k+1) untuk **semua** input arbitrer. Bukti sketsa: misalkan untuk input `x` wire `k` bernilai `v > s` (s = nilai peringkat ke-(k+1)); threshold `f(y)=1 iff y>s` monoton, komut dengan setiap CE, memberi input 0-1 dengan ≤ N-k-1 satu-an namun wire `k` = 1 → lolos uji 0-1, kontradiksi. Kasus `v < s` simetris dengan threshold `y ≥ s`. Konjungsi atas beberapa `k` masing-masing sound.

Untuk **kelas input terbatas** (sliding window), jaminan berlaku pada kelas tersebut — persis invariant yang dijamin sliding window DSP.

**Catatan soundness simetri (penting):** reduksi vektor cermin (`hasSmallerMirror`) hanya valid bila himpunan target **simetri-cermin** (k ∈ targets ⇔ N-1-k ∈ targets, mis. `{0,4,8}` atau median ganjil `{4}`). Untuk target asimetris (mis. min-K `{0,1,2,3}`), simetri dimatikan otomatis dengan peringatan — kalau tidak, hasil pencarian bisa salah.

## 2. Perombakan B: Cost Function SIMD-Aware

### 2a. Penalti lane crossing (`computeSimdCost()` di `hutils.cpp`)

```
w(i,j) = SimdWeightAligned   jika |i-j| ∈ {1,2,4,8}  (stride pangkat dua, satu instruksi shuffle/vperm/tbl)
         atau i+j == N-1            (pasangan simetri-cermin)
         SimdWeightCrossing  selain itu (lane crossing, memecah vektorisasi)
```

Default: aligned = 1, crossing = 3 (bisa diatur 3–4 via config). Ketika `SimdAwareCost=1`, **orthogonal convex hull berpindah dari (size, depth) ke (simdCost, depth)** — evolusi mengejar jaringan yang paling murah dieksekusi di `@Vector` Zig, bukan yang paling sedikit komparator. Field output baru `'C'` = cost terpakai, `'L'` = ukuran fisik.

### 2b. Batasan register pressure (`computeMaxLayerWires()` di `hutils.cpp`)

Parameter baru `MaxActiveWires` = **jumlah wire aktif maksimum per layer (stage)** — setiap wire aktif menempati satu register vektor (16 × YMM @ AVX2 → `MaxActiveWires=16`; 32 × V @ ARM64 NEON → `MaxActiveWires=32`). Kandidat yang layer terlebarnya melebihi batas **ditolak sebelum pengujian vektor** (gate di loop evolusi dan pencarian solusi awal). Postfix yang melanggar batas ditolak saat startup (mencegah livelock). Field output `'MW'` = wire aktif maksimum jaringan hasil.

---

## 3. Fitur pendukung mode 4 (shared-prefix / early output)

`ReportEarlyOutput=1` (default pada mode 4) menghitung **settling layer per wire target**: jumlah layer minimum agar nilai wire target final untuk semua vektor uji — diukur dengan sweep layer-per-layer atas seluruh himpunan vektor (`computeSettlingDepths()`). Karena diukur pada semua vektor 0-1, jaminan early-output-nya berlaku untuk semua input (argumen threshold yang sama). Output field `'SD'` (maksimum) dan `'SDt'` (per target). Semua layer sebelum SD adalah **prefix bersama** yang bisa direuse antar blok piksel bertetangga / dikonsumsi lebih awal oleh pipeline.

## 4. Key config baru (ConfigParser.h/.cpp)

| Key | Default | Arti |
|---|---|---|
| `SearchMode` | 0 | 0=sorter penuh, 1=kuantil, 2=sliding window, 3=partial min/max-K, 4=multi-output |
| `QuantileIndices` | — | List indeks output yang diverifikasi, koma terpisah (mis. `0,4,8` atau `13,14`) — wajib utk mode 1/4, opsional di mode 2 |
| `IsolateSide` | 0 | Mode 3: 0 = K terkecil (wire bawah), 1 = K terbesar (wire atas) |
| `IsolateCount` | 1 | Mode 3: K |
| `PresortedCount` | N-1 | Mode 2: jumlah wire yang sudah terurut (wire 0..P-1) |
| `NewSampleWire` | PresortedCount | Mode 2: indeks wire sampel baru (harus di luar blok presorted) |
| `SimdAwareCost` | 0 | 1 = hull memakai cost SIMD-aware |
| `SimdWeightAligned` | 1 | Bobot CE yang ramah vektorisasi |
| `SimdWeightCrossing` | 3 | Bobot CE lane-crossing (sarankan 3–4) |
| `MaxActiveWires` | 0 (off) | Register pressure: max wire aktif per layer |
| `ReportEarlyOutput` | mode 4: 1 | Lapor `SD`/`SDt` (settling layer per target) |

`ConfigParser` ditambah tipe nilai ketiga: list integer (`getIntList()`), selain int dan network.

## 5. Contoh pemakaian

```bash
make                                    # build (g++ / clang++, flag PGO lama tetap ada)
./SorterHunter sample_config_dsp_sliding.txt    # audio median 3-tap, N=9
./SorterHunter sample_config_dsp_quantile.txt   # persentil GCC, N=16
./SorterHunter sample_config_dsp_mink.txt       # noise floor VAD, N=16
./SorterHunter sample_config_dsp_video.txt      # 3x3 min+median+max, N=9
```

Jaringan hasil dicetak per baris dalam format JSON-ish (kompatibel tooling lama), plus field baru `'C','MW','Mode','SD','SDt'`. Contoh hasil yang sudah tervalidasi ada di `Networks/DSP/`:

| File | Mode | Ukuran | Depth | Catatan |
|---|---|---|---|---|
| `DSP_SlidingInsertion_9_8_4.json` | 2 | **8 CE** | 4 | vs sorter generik N=9 = 25 CE |
| `DSP_Quantile_16_33_11.json` | 1 | 33 CE | 11 | p90+trim target {13,14}; vs sorter N=16 ≈ 158 CE |
| `DSP_MinK_16_42_14.json` | 3 | 42 CE | 14 | 4 nilai terkecil terisolasi (ranked) |
| `DSP_MultiOutput_9_22_9.json` | 4 | 22 CE | 9 | min+median+max, SDt=[5,8,6] |

## 6. Verifikasi yang sudah dilakukan

1. **Regresi mode klasik**: full sorter N=9 tetap dicari, hull (size,depth) berjalan normal.
2. **Validasi brute-force independen (Python)** untuk semua jaringan di atas:
   - mode 2: seluruh 512 pola dicek; 18 vektor kelas sliding lolos 100%, dan jaringan **bukan** sorter generik (spesialisasi bekerja);
   - mode 1/3: **ekshaustif 2^16 = 65.536 pola**, 0 pelanggaran;
   - mode 4: ekshaustif 2^9, plus settling depth dihitung ulang di Python = persis `SDt` C++;
   - `MW` semua jaringan ≤ limit config; cost `C` cocok dengan hitungan ulang Python.
3. **Uji silang checker kuantil** dengan jaringan median terpercaya repo (`Median_9_19_7.json`): versi utuh langsung dinyatakan complete (`Network complete`, L=19/D=7 persis metadata); versi yang satu CE-nya dihapus (42 pelanggaran median terverifikasi Python) **tidak pernah** diterima.

Dua bug ditemukan & diperbaiki berkat validasi brute-force ini:
- `bumpVectorPosition` (optimasi urutan vektor) kini memutasi `qmasks` identik dengan vektor data — tanpa itu, mask kuantil lepas sejajar setelah kegagalan kandidat pertama;
- metrik settling kini mengambil **maksimum** antar grup vektor (semula nilai grup terakhir menimpa).

## 7. Batasan & catatan

- Verifikasi kuantil menjamin **ranked selection** (wire ke-k = nilai peringkat ke-k+1, tersortir di antara dirinya) pada indeks target; wire lain bebas ("tanpa menyentuh sisanya"). Varian "K terkecil sebagai set tanpa urutan internal" **tidak bisa** diverifikasi zero-one principle secara sound — makanya min/max-K diimplementasi sebagai kuantil rentang `[0..K-1]` / `[N-K..N-1]` (bonus: K nilai terisolasi sekaligus terurut).
- Untuk N=32/64 (window jitter buffer besar), enumerasi 2^N tidak mungkin — gunakan prefix seperti mode klasik: `PrefixType=2` + `GreedyPrefixSize` yang cukup (mis. 16–24) sehingga vektor uji = output prefix (ribuan, bukan miliaran). Validasi Python untuk N > 16 tidak lagi ekshaustif; gunakan sampling acak + semua pola dengan popcount di sekitar N-k (batas keputusan kuantil).
- `MaxActiveWires` ekstrem (mis. 2–4) membuat konstruksi solusi awal lambat karena jaringan acak punya layer lebar; batas realistis = jumlah register target.
- Simetri otomatis nonaktif untuk kelas input/target yang tidak tertutup cermin; untuk target simetri-cermin tetap aktif.
