# ouro

**一個用 C 寫成、能編譯自己的 C11 編譯器，原生產生 arm64 macOS（Apple Silicon）程式。**

ouro 能編譯自己的原始碼，而它編出來的編譯器再編譯一次同一份原始碼，會得到逐位元組完全相同的執行檔。它有自己的前置處理器、語法分析器與型別檢查器，把程式轉成 SSA 形式的中間表示，再做最佳化（mem2reg、稀疏條件常數傳播 SCCP、共同子運算式消除 CSE、迴圈不變量外提 LICM、多餘載入消除、死碼消除 DCE、控制流程圖整理），選擇 arm64 指令、用線性掃描做暫存器配置，最後輸出遵守 Apple arm64 呼叫慣例的組合語言，並自己呼叫系統的組譯器與連結器。

[English](README.md) · [設計文件](docs/DESIGN.md) · [導讀（給初學者）](docs/導讀.zh-TW.md)

```
$ make && make bootstrap
stage2 and stage3 binaries are identical
stage1 and stage2 emit identical assembly for all 20 source files
stage2 build: 1.12 s, stage3 build: 1.51 s
7c82f7fa553f1d6f03783cdf637112f31af146b2779fe404dd7bf57f204648e5  build/stage2/ouro
7c82f7fa553f1d6f03783cdf637112f31af146b2779fe404dd7bf57f204648e5  build/stage3/ouro
```

第 1 階段是用 clang 編出的 ouro，第 2 階段是用第 1 階段編出的 ouro，第 3 階段是用第 2 階段編出的 ouro。以下每個說法都附上可以驗證它的指令。

## 能編譯什麼

| 檢查項目 | 結果 | 指令 |
|---|---|---|
| [c-testsuite](https://github.com/c-testsuite/c-testsuite)（全部 220 個 single-exec 測試，測試時才下載，commit 5c72756） | -O0 與 -O1 都是 **220/220** | `make c-testsuite` |
| 從官方原始碼包建置的 [Lua 5.4.7](https://www.lua.org)，執行 Lua 自己的測試套件 `all.lua` | 27 個測試檔全部通過（"final OK"）；連同 Lua 內部斷言與 C API 測試函式庫一起編進去也通過；測試用的 C 模組也由 ouro 編成動態函式庫 | `make lua` |
| [zlib 1.3.1](https://zlib.net) | 它的 `example` 自我測試通過；`minigzip` 與系統的 `gzip` 雙向互通，而且壓縮結果和 clang 建置的版本逐位元組相同 | `make zlib` |
| 呼叫慣例 | 每個亂數種子產生 80 種函式簽名的呼叫端與被呼叫端，分別用 ouro 與 clang 編譯並交叉組合，輸出都和全用 clang 的版本相同 | `make abi` |
| 隨機程式 | 種子固定的產生器寫出定義良好的 C 程式，輸出和 clang 相同（`make test` 跑 40 個，`make fuzz` 跑 500 個） | `make fuzz` |
| 迴歸測試程式 | -O0 與 -O1 的輸出都和 clang 相同 | `make test` |

`make test` 另外也會跑單元測試、前置處理器測試（token 序列和 `clang -E` 相同）、診斷訊息測試（訊息、行號、欄號與插入符號 `^` 都要正確），以及檢查最佳化後 IR 和產生的組合語言的 FileCheck 式測試。

## 看看裡面

```c
int sum(const int *a, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += a[i];
  return s;
}
```

`ouro -O1 --dump-ir -S sum.c` 會印出最佳化後的 IR（區域變數已經變成 SSA 值與 phi），並寫出組合語言：

```
function _sum {                          _sum:
bb0:                                     LBB0_0:
  %0 = param i64 x0                          mov w9, #0
  %1 = param i32 x1                          mov w10, #0
  jmp bb2                                    b LBB0_2
bb1:  ; preds bb2                        LBB0_1:
  %3 = sext32 i64 %10                        ldr w11, [x0, w9, sxtw #2]
  %4 = shl i64 %3, 2                         add w10, w10, w11
  %5 = add i64 %0, %4                        add w9, w9, #1
  %6 = load i32 %5                       LBB0_2:
  %7 = add i32 %11, %6                       cmp w9, w1
  %8 = add i32 %10, 1                        b.lt LBB0_1
  jmp bb2                                LBB0_3:
bb2:  ; preds bb0 bb1                        mov x0, x10
  %10 = phi i32 [0, bb0], [%8, bb1]          ret
  %11 = phi i32 [0, bb0], [%7, bb1]
  %12 = slt i32 %10, %1
  br %12, bb1, bb3
bb3:  ; preds bb2
  ret %11
}
```

陣列索引的計算併進了「暫存器位移」形式的載入指令，比較併進了分支指令；而因為配置器讓每個值使用它所流入的 phi 的暫存器，迴圈回邊上的搬移指令也消失了。錯誤訊息會指向原始碼，連經過巨集展開的也一樣：

```
$ ouro -c oops.c
oops.c:3:10: error: use of undeclared identifier 'PI'
    3 |   return AREA(r);
      |          ^
oops.c:1:18: note: expanded from macro 'AREA'
    1 | #define AREA(r) (PI * (r) * (r))
      |                  ^
```

## 效能

用 `scripts/bench.py --runs 7` 在 Apple M5（10 核心）、macOS 27.0、Apple clang 17.0.0 上量測。量測時這台機器同時在跑其他工作（負載平均約 8），因此請預期幾個百分點的誤差；每個數字是 7 次執行的中位數，單位是秒。比值大於 1 表示 ouro 產生的程式比較快。同一個程式的所有建置版本必須印出相同的結果，數字才算數。

| 程式（執行時間，秒） | clang -O0 | clang -O2 | ouro -O0 | ouro -O1 | ouro -O1 對 clang -O0 | ouro -O1 對 clang -O2 |
|---|---:|---:|---:|---:|---:|---:|
| binarytrees.c | 0.797 | 0.663 | 0.720 | 0.701 | 1.14x | 0.94x |
| fannkuch.c | 0.217 | 0.167 | 0.278 | 0.133 | 1.62x | 1.25x |
| matmul.c | 0.264 | 0.030 | 0.419 | 0.064 | 4.13x | 0.46x |
| nbody.c | 1.016 | 0.315 | 1.072 | 0.383 | 2.65x | 0.82x |
| sieve.c | 1.264 | 1.175 | 2.216 | 1.133 | 1.12x | 1.04x |
| Lua 5.4.7 執行 `tests/bench/fib.lua` | 0.232 | 0.076 | 0.202 | 0.147 | 1.58x | 0.51x |

ouro -O1 在每個程式上都比 clang -O0 快；和 clang -O2 相比，從大約一半的速度（clang 會做內聯、向量化與迴圈展開的地方：矩陣乘法、Lua 直譯器）到不相上下（受記憶體配置或記憶體頻寬限制的程式）。ouro 不做內聯、向量化、迴圈展開，也不做尾端呼叫消除。

編譯速度：依序把所有檔案編成組合語言（3 次的中位數）：

| 編成組合語言（秒） | clang -O0 | clang -O2 | ouro -O0 | ouro -O1 | ouro -O1 對 clang -O0 | ouro -O1 對 clang -O2 |
|---|---:|---:|---:|---:|---:|---:|
| Lua 5.4.7（33 個檔案，23,940 行） | 1.195 | 2.233 | 0.258 | 0.299 | 4.00x | 7.48x |
| ouro 自己（20 個檔案，15,597 行） | 0.756 | 1.836 | 0.113 | 0.133 | 5.68x | 13.80x |

`--time` 會列出每個階段花的時間。建置執行檔時還要執行系統的組譯器，每個檔案大約 0.05 秒，建置時間主要花在這裡。

## 運作方式

```
原始碼 ─► 詞法分析 ─► 前置處理 ─► 語法分析/型別檢查 ─► 產生 IR ─► 最佳化 ─► 指令選擇 ─► 暫存器配置 ─► 組合語言 ─► as ─► ld
```

- **前置處理器**：一個「用拉的」token 串流，底下是一疊來源（檔案與巨集展開），用 Prosser 的 hideset 控制遞迴。它的輸出會和 `clang -E` 逐個 token 比對。
- **前端**：語法分析和型別檢查在同一趟完成。每個隱含的型別轉換都會變成樹裡一個明確的轉型節點；常數運算式在建樹時就算好，但標準規定為未定義行為的運算一律不在編譯期計算。
- **IR**：建立在控制流程圖上的 SSA。常數在每次使用的地方重新產生。比 32 位元窄的值，在 32 位元暫存器中一律保持符號延伸或零延伸的狀態，這也正是 Apple ABI 在呼叫時所要求的。
- **最佳化器**：mem2reg（Cytron 等人）、和 arm64 實際運算方式完全一致的 SCCP（Wegman 與 Zadeck）、代數化簡、沿支配樹的 CSE、區塊內的多餘載入消除（別名判斷保守）、LICM、DCE。測試時每個階段之後都會用驗證器檢查 IR。
- **後端**：以模式比對做指令選擇（比較加分支、立即數、移位運算元、暫存器位移定址、跳躍表），在「有空洞的存活區間」上做線性掃描暫存器配置，溢出的選擇依迴圈深度加權，phi 合併提示，以及平行搬移的解決。
- **ABI**：Apple 的 arm64 規則：堆疊上的引數緊密排列、可變引數一律放堆疊、窄整數引數由呼叫端延伸、`char` 有號、`long double` 是 64 位元、完全不碰 x18、HFA 放在 SIMD 暫存器。

[docs/DESIGN.md](docs/DESIGN.md) 說明每個階段、困難的地方，以及沒有採用的做法。

### 為什麼 ouro 自帶 libc 標頭檔

Apple SDK 的標頭檔用了 Clang 的 blocks、可用性與可空性屬性、符號別名，以及用編譯器內建函式寫成的 inline 函式。與其實作這些擴充語法，ouro 在 `include/` 裡附了大約 30 個小標頭檔，宣告 C 程式需要的東西，而在二進位介面有影響的地方採用 Darwin 的結構配置與常數（`__stdinp`、`errno` 是 `*__error()`、48 個 int 的 `jmp_buf`、144 位元組的 `struct stat`、`realpath` 的 `$DARWIN_EXTSN` 符號）。程式如果需要 ouro 沒有提供的標頭檔或宣告，就要先補上才能編譯。

## 使用方式

```sh
make                         # 產生 build/ouro
build/ouro -o hello hello.c  # 編譯並連結
build/ouro -S -O1 f.c        # 輸出組合語言
build/ouro -E f.c            # 只做前置處理
build/ouro --help
```

選項：`-o`、`-c`、`-S`、`-E`、`-O0`/`-O1`（預設 `-O1`；`-O2` 以上等同 `-O1`）、`-I`、`-D`、`-U`、`-w`、`-Werror`、`-l`、`-L`、`-Wl,`、`-shared`、`--dump-ir`、`--dump-ir-raw`、`--dump-mir`、`--time`。其他編譯器常見的旗標（`-g`、`-std=`、`-W...`、`-f...`）會接受並忽略。連結時會採用 `MACOSX_DEPLOYMENT_TARGET`。

## 限制

- 只支援 arm64 macOS，並需要 Xcode 命令列工具提供的 `as` 與 `ld`。
- 不支援：`_Complex`、`_Thread_local`、真正的 `_Atomic` 運算（這個修飾字會被接受但忽略）、多維的可變長度陣列以及指向它們的指標運算、內嵌組合語言、computed `goto`、GNU 的 `a ?: b`。`long double` 就是 `double`，和 Apple 平台相同。
- 診斷在第一個錯誤就停止，沒有錯誤復原。
- 最佳化器沒有內聯、迴圈展開、向量化、尾端呼叫消除，也沒有以型別為依據的別名分析。
- 只能使用 `include/` 裡的標頭檔（見上文）。
- 測試過的程式是 c-testsuite、Lua、zlib、ouro 自己，以及這個 repo 裡手寫與自動產生的測試；更大的程式碼很可能會遇到尚未支援的功能。

## 相關專案

- Rui Ueyama 的 [chibicc](https://github.com/rui314/chibicc) 是一個小巧、能自我編譯的 C11 編譯器，目標是 x86-64 Linux，能編譯真實的軟體；它直接從語法樹以堆疊方式產生程式碼。ouro 和它一樣想做一個好讀、又能建置真實軟體的編譯器，但多了 SSA 最佳化器與暫存器配置，目標是 Apple 的 arm64 ABI。
- [cproc](https://github.com/michaelforney/cproc) 加上 [QBE](https://c9x.me/compile/) 是設計上最接近的：C11 前端接一個獨立的、有暫存器配置的 SSA 後端。cproc 在 Linux 與 FreeBSD（x86_64、aarch64、riscv64）上測試，需要外部的前置處理器；QBE 列出的目標是 amd64（Linux 與 macOS）、arm64 與 riscv64。ouro 是一份自己掌握每個階段（包括前置處理器與標頭檔）的程式碼，並專門針對 Apple 的 arm64 呼叫慣例。
- [TinyCC](https://bellard.org/tcc/) 單趟編譯、速度極快，支援多種目標，最佳化很少。
- [lcc](https://github.com/drh/lcc) 是經典的可重定目標 C 編譯器，有一本書介紹它；它從語法樹產生程式碼，沒有 SSA。

據我所知，把這些全部放在一份自給自足、能自我編譯、目標為 arm64 macOS 的程式碼裡（自己的前置處理器與標頭檔、SSA 最佳化器、遵守 Apple 呼叫慣例的暫存器配置器）的編譯器並不多。ouro 的定位是一個好讀的範例，而不是 clang 的替代品。

## 開發

```sh
make test         # 單元、前置處理器、診斷、IR、組合語言、迴歸、ABI 與隨機程式測試
make bootstrap    # 三階段自我編譯檢查
make c-testsuite  # 把測試套件下載到 build/
make lua          # 把 Lua 5.4.7 與它的測試下載到 build/（會驗證雜湊值）
make zlib         # 把 zlib 1.3.1 下載到 build/（會驗證雜湊值）
make fuzz         # 500 個隨機程式；FUZZ_SEEDS=N 可以跑更多
make abi          # 呼叫慣例測試，10 個種子
make lint         # clang -Wall -Wextra -Wpedantic -Wshadow -Werror
make sanitize     # 用 UBSan 建置的 ouro 跑迴歸與 ABI 測試
python3 scripts/bench.py   # 產生上面的效能表格
```

`OURO_VERIFY=1` 會在每個階段之後開啟 IR 驗證器（測試腳本都有設定）。當 ouro 和 clang 對某個程式的結果不同時，`tests/fuzz/reduce.py` 會把它縮小，並一路用 UBSan 確認它仍然沒有未定義行為。

## 授權

[MIT](LICENSE)
