# iPhone 0.7.10.1 — iOS 音量條 / Google App 打不了字（進度筆記）

分支 `iphone-0710`（worktree `PhoneMirror-iphone0710`，從 master b519d29 開出）。
建置目錄 `build-iphone0710`（被 .gitignore 的 build-*/ 擋掉）。

## 現象（owner，0.7.10）
- 投影中打開 Google App 就跳 iOS 音量條，有時打不了字。
- Facebook App 打字完全正常。只有 Google App 會這樣。
- iPhone「鍵盤按鍵聲」本來就是關的，所以排除「按鍵音觸發音訊路由」這個假設。

## 證據（owner log %LOCALAPPDATA%\PhoneMirror\phonemirror.log）
- 14:00:12 mDNS 同時廣播 `_airplay` 和 `_raop`，features=0x4005A7FFEE6（bit 9 Audio 開），
  `[audio-advert] airplay_advertise_audio=1`。
- 14:01:13 連線：鏡像 SETUP，之後音訊 SETUP（ct=8 AAC-ELD、usingScreen=1），14:01:17 音訊 TEARDOWN。
- 14:01:17 到 14:07:02 之間：沒有任何音訊 SETUP、SET_PARAMETER、GET /info，
  只有影像和每分鐘的 /feedback。所以 14:02–14:05 的音量條是 iOS 自己彈的，沒有請求送到我們這邊。
- 14:07:02 才又有一段約 4 秒的音訊串流（SETUP→TEARDOWN）。
- owner 從來沒試過 airplay_advertise_audio=0/2（整份 log 只出現過 =1）。

## 根因判斷（推論，真機未驗證）
Google App 的搜尋框一聚焦，就會先把語音輸入準備好（麥克風按鈕、助理熱詞、Discover
自動播放），也就是啟用 AVAudioSession 的 playAndRecord 類別。playAndRecord 預設不能
走 AirPlay 輸出（除非 App 自己加 allowAirPlay），所以 iOS 會把音訊路由從「自在投影
（AirPlay 鏡像音訊）」切回 iPhone 本機，並彈出音量 HUD 顯示新路由的音量。路由一換，
Google App 會收到 interruption / routeChange 通知，它的語音 UI 一反應，搜尋框就可能失焦
（鍵盤收起或打不了字）。
Facebook 的文字框不會預先啟動錄音工作階段，路由不變，所以不會彈。
這一切發生在 iPhone 內部，所以 log 上看不到任何請求。

能從接收端下手的只有「我們怎麼自我宣告」：
1. 是不是一個音訊輸出路由：features bit 9、`_raop`、statusFlags bit 2。
2. 音量能不能遙控：GET /info 的 `volumeControlType`（UxPlay 沒送這個鍵，shairport-sync 送 3）。

## 開關（settings.ini，下次啟動生效；預設＝與 0.7.10 完全相同）
| 鍵 | 值 | 改了什麼 | 聲音 |
|---|---|---|---|
| airplay_volume_control | -1（預設） | /info 不送 volumeControlType | 不變 |
| | 0 | 送 0（猜：固定音量，音量鍵留在手機） | 應該還在；手機音量鍵可能調不到 PC 音量（PC 滑桿照常） |
| | 1 | 送 1（只能上下調，像 Apple TV 接 HDMI-CEC） | 應該還在 |
| | 3 | 送 3（滑桿，shairport-sync 的值） | 不變（對照組） |
| airplay_audio_status | 1（預設） | statusFlags 68、TXT flags=0x4、sf=0x4 | 不變 |
| | 0 | 拿掉 bit 2：64、0x0、0x0 | 可能沒聲音 |
| airplay_advertise_audio（0.7.6 就有） | 0 | bit 9 關、不註冊 `_raop` | 可能沒聲音（從沒在真機試過） |
| | 2 | bit 9 開、不註冊 `_raop` | 應該還在 |

## 給 owner 的建議測試順序
每次只改一個值：先關掉自在投影、改 settings.ini、重開，再從 iPhone 重新開始鏡像。
接著打開 Google App 點搜尋框打字，看三件事：音量條有沒有出現、能不能打字、PC 有沒有聲音
（例如播一段 YouTube）。
1. airplay_volume_control=0
2. airplay_volume_control=1
3. airplay_advertise_audio=2
4. airplay_advertise_audio=0
5. airplay_audio_status=0
每次測完都改回 -1 / 1 / 1。log 裡搜尋 `[volume-hud]` 和 `[audio-advert]`，可以確認當下用的是哪一組。

## 進度
- [x] 查證：log、/info（raop_handlers.h）、TXT（dnssd_mdnsd.c）
- [x] core：pm_volume_hud.h + /info + TXT + Options，pm_robust_test 加 test_volume_hud（commit 8d10bf4）
- [x] app：settings.ini 讀寫、傳給 Options（commit 722bc61）
- [x] 全量建置（build-iphone0710 Release，ALL_BUILD /m:2，0 error）
- [x] 測試全綠（Idle＋CPU 16-23，一次只跑一個）：pm_robust_test（含新的 test_volume_hud）、pm_buffer_test、
      pm_ntp_test、pm_clock_test、pm_reqlog_test、pm_volume_test、pm_audio_test（--session-test、
      --wav ELD、--wav ALAC、--bench，都不出聲）、pm_recorder_test、pm_video_test（ios_like.h264/.h265 --tap）
- 未驗證：真機（iOS 怎麼解讀 volumeControlType 0/1、拿掉 bit 2／bit 9 後鏡像還有沒有聲音）；
  settings.ini 讀寫只有編譯過，沒有自動化測試
