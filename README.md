# AMB82-MINI 專注偵測系統

這個專案包含三個部分：

1. `firmware/FocusMonitor/FocusMonitor.ino`：在 AMB82-MINI 上執行 YOLOv4-Tiny，辨識 `person`（學生）與 `cell phone`（手機），畫出 Bounding Box（邊界框），並執行 3 秒連續判斷與防重複計數。
2. `gateway/server.py`：在電腦上讀取 AMB82-MINI 的 RTSP 影像與 JSON 狀態，轉成瀏覽器可顯示的即時影像。
3. `dist/`：監控網頁，顯示學生、手機、重疊與滑手機秒數，以及分心次數。

## 一、燒錄 AMB82-MINI

1. 安裝 Arduino IDE 與 Realtek AmebaPro2 開發板套件。
2. 在 Arduino IDE 開啟 `firmware/FocusMonitor/FocusMonitor.ino`。
3. 將 `YOUR_WIFI_SSID`、`YOUR_WIFI_PASSWORD` 改成自己的 Wi-Fi。
4. 選擇 AMB82-MINI 對應的開發板與連接埠，編譯並上傳。
5. 開啟序列監控視窗（115200 baud），記下顯示的 `AMB82 IP`。

## 二、啟動網頁

電腦與 AMB82-MINI 必須使用同一個 Wi-Fi。電腦需要 Python 3 與 ffmpeg。

macOS 可先安裝 ffmpeg：

```sh
brew install ffmpeg
```

在專案資料夾中啟動：

```sh
python3 gateway/server.py --board-ip 192.168.1.82
```

請把範例 IP 改成序列監控視窗顯示的 IP，再開啟：

```text
http://127.0.0.1:8000
```

也可以不加 `--board-ip`，啟動後按網頁右上角齒輪輸入 IP。

## 三、系統如何避免誤判

- YOLO 信心值必須至少 60%。
- 人框必須覆蓋至少 20% 的手機框，才視為重疊。
- 重疊必須連續 3 秒，才顯示「專心一下！」並將分心次數加一。
- 低信心結果只會中斷計時，不會觸發提醒。
- 同一次手機使用只計一次。計數後，必須在學生仍清楚可見時，連續 3 秒沒有手機，系統才重新允許下一次計數。
- 系統不儲存影像，僅在區域網路中顯示即時串流。

## 常用英文對照

- Object Detection：物件偵測
- Bounding Box：邊界框（俗稱辨識框）
- Confidence Score：信心分數
- Overlap Ratio：重疊比例
- Continuous Detection：連續偵測
- Cooldown / Re-arm：冷卻時間／重新允許計數
- RTSP Stream：即時串流協定

## 參考資料

- Realtek Ameba 官方 Object Detection 說明：<https://github.com/Ameba-AIoT/ameba-arduino-doc/blob/main/source/ameba_pro2/amb82-mini/Example_Guides/Neural%20Network/Object%20Detection.rst>
- Realtek AmebaPro2 Arduino SDK：<https://github.com/Ameba-AIoT/ameba-arduino-pro2>
