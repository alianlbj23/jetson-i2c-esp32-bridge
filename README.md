# jetson-i2c-esp32-bridge

Jetson 與 ESP32-S3 之間的 I2C 連線測試。ESP32 模擬成一顆標準的 I2C 感測器（slave），Jetson 當 master，只靠 Linux 內建的 `i2c-tools` 就能驗證線路、驅動與雙向讀寫是否正常，Jetson 端不需要寫任何程式。

## 目錄

- [測試原理](#測試原理)
- [硬體接線](#硬體接線)
- [ESP32 端設定與燒錄](#esp32-端設定與燒錄)
- [Jetson 端測試](#jetson-端測試)
- [暫存器對應表](#暫存器對應表)
- [通訊協定](#通訊協定)
- [ESP32 韌體運作機制](#esp32-韌體運作機制)
- [常見問題](#常見問題)

## 測試原理

I2C 匯流排上只有 master 能主動發起傳輸，slave 只能被點名後回應。市面上的 I2C 感測器幾乎都採用「暫存器對應」（register map）的介面：裝置內部有一排有編號的 byte，master 只做兩件事，一是把資料寫進某個編號的暫存器，二是從某個編號開始連續讀出 N 個 byte。

本專案讓 ESP32-S3 在記憶體裡放一個 32 byte 的陣列當暫存器表，並掛在位址 `0x28`。從 Jetson 看來，ESP32 和一顆真的感測器完全一樣，因此可以直接用 `i2cdetect`、`i2cget`、`i2cset`、`i2ctransfer` 操作。

暫存器裡事先放了已知的值，每個測試步驟都有可比對的正確答案：

| 驗證項目 | 做法 | 預期結果 |
|---|---|---|
| 位址與線路 | `i2cdetect -r` 掃描 | 在 `0x28` 看到裝置 |
| 讀取路徑 | 讀 `WHO_AM_I` 暫存器 | 固定回 `0xA5` |
| 寫入路徑 | 寫一個隨機值到 scratch 再讀回 | 讀回的值與寫入相同 |
| 多 byte 連續讀 | 從 `0x10` 讀 16 byte | 得到字串 `ESP32-S3 I2C OK` |
| 多 byte 連續寫 | 一次寫 8 byte 到 scratch 再讀回 | 8 byte 完全一致 |
| 資料是活的 | 間隔 0.5 秒讀兩次 uptime | 第二次數值大於第一次 |

## 硬體接線

ESP32-S3 與 Jetson 40-pin header 之間接三條線：

| ESP32-S3 | Jetson 40-pin | 說明 |
|---|---|---|
| GPIO8 (SDA) | pin 3 或 pin 27 | 資料線 |
| GPIO9 (SCL) | pin 5 或 pin 28 | 時脈線 |
| GND | pin 6、9、14 等任一 GND | 兩板必須共地 |

注意事項：

- 兩塊板子都是 3.3V 邏輯，不需要電平轉換。
- Jetson 40-pin 的 pin 3/5 與 pin 27/28 板上已有上拉電阻，不需要外接。ESP32 端另外開啟內部弱上拉作為保險。
- 不同 Jetson 型號，40-pin 腳位對應的 `/dev/i2c-N` 不同：

| 型號 | pin 3/5 | pin 27/28 |
|---|---|---|
| Orin Nano / Orin NX | i2c-7 | i2c-1 |
| Xavier NX | i2c-8 | i2c-1 |
| Nano（原版） | i2c-1 | i2c-0 |

不確定時在 Jetson 上執行 `i2cdetect -l` 查看。

## ESP32 端設定與燒錄

### 環境需求

- ESP-IDF v6.x（使用 `driver/i2c_slave.h` 驅動與 `esp_hal_i2c` 低階層；已在 v6.1 建置驗證）
- 目標晶片 `esp32s3`，已寫在 `sdkconfig.defaults`

### 可調整的設定

透過 `idf.py menuconfig`，在 `Jetson I2C Test Configuration` 選單中可修改：

| 設定項 | 預設值 | 說明 |
|---|---|---|
| `CONFIG_JETSON_I2C_SDA_GPIO` | 8 | 接 Jetson SDA 的 GPIO |
| `CONFIG_JETSON_I2C_SCL_GPIO` | 9 | 接 Jetson SCL 的 GPIO |
| `CONFIG_JETSON_I2C_SLAVE_ADDR` | 0x28 | 7-bit slave 位址，範圍 0x08 到 0x77 |

這些設定定義在 `main/Kconfig.projbuild`，韌體從 `sdkconfig.h` 讀取。如果改了位址，Jetson 端的測試腳本也要同步修改 `ADDR` 變數。

### 建置與燒錄

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

啟動後 serial monitor 應看到：

```
I (xxx) jetson_i2c: I2C slave ready: addr=0x28 SDA=GPIO8 SCL=GPIO9
I (xxx) jetson_i2c: On the Jetson run:  sudo i2cdetect -y -r <bus>   (expect 0x28)
```

之後每 5 秒會印一行心跳，顯示目前的收發次數與 scratch 第一個 byte：

```
I (xxx) jetson_i2c: alive | addr 0x28 | rx=3 tx=2 | scratch[0]=0x5A | SDA=1 SCL=1
```

Jetson 每次讀寫時 ESP32 也會印出對應的 log，方便對照。

## Jetson 端測試

### 自動測試腳本

```bash
sudo apt install i2c-tools        # JetPack 通常已預裝
sudo ./jetson/i2c_test.sh         # 自動掃描所有 bus 找 ESP32
sudo ./jetson/i2c_test.sh 7       # 或直接指定 bus 編號
```

一定要用 sudo，一般使用者沒有權限開 /dev/i2c-N。

腳本會依序執行上面表格的六個驗證步驟，每步印出 PASS 或 FAIL，最後顯示總結。全部通過時 exit code 為 0。

### 手動測試

```bash
BUS=7   # 依照你的 Jetson 型號替換

i2cdetect -y -r $BUS                           # 應在 28 的位置看到裝置
i2cget -y $BUS 0x28 0x00                       # 0xa5
i2cset -y $BUS 0x28 0x08 0x5a                  # 寫 scratch
i2cget -y $BUS 0x28 0x08                       # 0x5a
i2ctransfer -y $BUS w1@0x28 0x10 r16           # 16 個 byte，解碼後是 "ESP32-S3 I2C OK"
i2ctransfer -y $BUS w1@0x28 0x04 r4            # uptime，4 byte little-endian 毫秒
```

## 暫存器對應表

共 32 byte，位址 `0x00` 到 `0x1F`。讀取超過 `0x1F` 會回繞到 `0x00`。

| 位址 | 名稱 | 權限 | 內容 |
|---|---|---|---|
| 0x00 | WHO_AM_I | 唯讀 | 固定 `0xA5` |
| 0x01 | FW_VERSION | 唯讀 | 韌體版本，目前 `0x02` |
| 0x02 | RX_COUNT | 唯讀 | master 寫入次數，8-bit 自動回繞 |
| 0x03 | TX_COUNT | 唯讀 | master 讀取次數，8-bit 自動回繞 |
| 0x04 到 0x07 | UPTIME_MS | 唯讀 | 開機毫秒數，32-bit little-endian |
| 0x08 到 0x0F | SCRATCH | 可讀寫 | 8 byte 暫存區，開機時全為 0 |
| 0x10 到 0x1F | MESSAGE | 唯讀 | ASCII 字串 `ESP32-S3 I2C OK` 加結尾 NUL |

對唯讀暫存器寫入會被忽略，ESP32 會印出警告 log。

## 通訊協定

採用和一般 I2C 感測器相同的格式。

**Master 寫入**

```
[START] [0x28 W] [reg_addr] [data0] [data1] ... [STOP]
```

第一個 byte 是起始暫存器位址，後面的資料依序寫入，位址自動遞增。如果只送一個 byte，等於只設定「讀取指標」，不寫入任何資料。

**Master 讀取**

```
[START] [0x28 W] [reg_addr] [REPEATED START] [0x28 R] [byte0] [byte1] ... [STOP]
```

先用一次寫入設定起始位址，再以 repeated start 切換成讀取，ESP32 從該位址開始連續送出資料。`i2cget` 和 `i2ctransfer w1@... rN` 都是這種形式。

指標在寫入後會停留在 master 設定的位置，因此「寫完立刻讀」會從同一個位址開始。

## ESP32 韌體運作機制

程式在 `main/main.c`，分成兩層。

**1. 中斷回呼（ISR）：所有協定邏輯都在這裡**

ESP-IDF 的 i2c_slave 驅動提供兩個回呼，都在中斷環境執行，必須放在 IRAM 且不能做 log 或任何會阻塞的事：

- `on_receive`：master 寫入完成時觸發，帶著收到的資料。回呼直接把第一個 byte 當成暫存器指標，後續 byte 逐一檢查是否可寫，可寫才存入，位址遞增並在 `0x20` 回繞。做完後把原始資料丟進 queue 給 log task。
- `on_request`：master 要求讀取時觸發。回呼先刷新 uptime 與計數器，從指標位置複製 32 byte，直接用 HAL 的低階函式清空硬體 TX FIFO、填入資料、再放開 SCL。然後把指標與第一個 byte 丟進 queue 給 log task。

**2. Log task 與心跳 task**

`i2c_log_task` 從 queue 取事件後印出人類可讀的 log。`heartbeat_task` 每 5 秒印一行狀態，包含收發次數、scratch 第一個 byte，以及 SDA、SCL 腳位目前的電位（閒置時應為 1）。

**為什麼 FIFO 一定要在 ISR 裡填**

master 以讀取模式點名 ESP32 時，ESP32-S3 硬體會把 SCL 拉低（clock stretching），驅動觸發 `on_request`，並且要等到有人把資料填進 FIFO 才放開 SCL。但硬體有一個保護計時器 `stretch_protect_num`，只有 10 bit，驅動設成最大值 1023 個 I2C 時脈週期。slave 驅動不設分頻，時脈是 40 MHz，所以 ESP32 最多只能拉住 SCL 約 25 µs，之後硬體會自己放開。

如果把填 FIFO 的工作交給 task，光是 ISR 到 task 的切換加上 UART 印 log 就遠超過 25 µs。SCL 被放開時 FIFO 還是空的，master 讀到的全部是 0xFF。這就是這個專案一開始遇到的問題，ESP32 的 log 顯示資料填對了，Jetson 卻全部讀到 0xFF。

把填 FIFO 搬進 ISR 後，從 SCL 被拉低到資料就緒只需要幾微秒，不再依賴 task 排程。代價是直接呼叫 `hal/i2c_ll.h` 的低階函式，繞過驅動的 `i2c_slave_write`，因此 `main/CMakeLists.txt` 多了 `esp_hal_i2c` 依賴。

**32 byte 一次填滿**

ESP32-S3 的硬體 TX FIFO 深度是 32 byte，和暫存器表大小相同，一次填滿即可應付任何不超過 32 byte 的讀取。每次填之前先清空 FIFO，避免上一次較短的讀取留下殘餘 byte。

## 常見問題

**沒有 sudo 時自動掃描找不到裝置**

一般使用者沒有權限開 /dev/i2c-N。腳本自動掃描時把 i2cdetect 的錯誤訊息吃掉了，所以看起來像掃了但沒找到。一律用 `sudo ./jetson/i2c_test.sh`。

**`i2cdetect -y <bus>` 掃不到，但 `i2cdetect -y -r <bus>` 掃得到**

Tegra 的 I2C 控制器不支援 SMBus Quick Write，i2cdetect 預設對 0x28 這類位址用這種方式探測，會印 `Can't use SMBus Quick Write command, will skip some addresses` 然後跳過。`-r` 改用讀取方式探測，Tegra 支援。腳本已一律使用 `-r`。

**`i2cdetect -r` 看不到 0x28**

- 檢查 SDA、SCL、GND 三條線，尤其是共地。
- 看 ESP32 心跳 log 的 `SDA=` `SCL=`，閒置時應該都是 1。都是 0 代表沒共地或線沒接到 Jetson 的 pin。
- 確認 bus 編號，用 `sudo i2cdetect -l` 查。Orin 系列 pin 3/5 是 i2c-7，pin 27/28 是 i2c-1。
- 確認 ESP32 serial monitor 有印出 `I2C slave ready`。
- 若改過 GPIO 或位址，確認 menuconfig 設定與實際接線一致。
- SDA 和 SCL 接反是最常見的錯誤，整張表會是空的。

**位址掃得到，但所有讀取都回 0xFF，ESP32 的 log 卻顯示資料填對了**

這是 ESP32-S3 硬體 clock stretching 只能撐約 25 µs 造成的，詳見上面「為什麼 FIFO 一定要在 ISR 裡填」。目前的韌體已經在 ISR 裡填 FIFO，如果又出現這個症狀，確認燒錄的是最新版，心跳 log 的 `FW_VERSION` 暫存器應為 0x02。

**寫入 scratch 後讀回的值不對**

- 確認寫入位址在 `0x08` 到 `0x0F` 之間，其他位址都是唯讀。
- 看 ESP32 serial monitor 的 `RX:` log，確認收到的 byte 數與內容。

**讀到的字串尾端有多餘字元**

- `MESSAGE` 區第 16 byte 是 NUL，`i2c_test.sh` 會用 `tr -d '\0'` 去掉。手動解碼時要自行處理。

**dmesg 出現 `tegra-i2c 31b0000.i2c: I2C transfer timed out`**

`31b0000.i2c` 是 i2c-5，不是 40-pin 用的 bus。這些訊息來自腳本自動掃描所有 bus 時掃到沒接任何裝置的 bus，可以忽略。

