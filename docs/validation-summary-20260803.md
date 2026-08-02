# 実機前最終検証サマリー（2026-08-03）

## 判定

引継ぎで定義した実機前の完成基準はPASSです。試験対象は固定VirtualBox VMと
合成VDIだけで、物理ディスク、実USB、ネットワークは使用していません。

## BCD新規再構築

クローン、復元、MBR→GPTのコピー先では、Microsoft署名済みBCDBootの事前確認、
固定BCDパスとreparse不使用の確認、既存BCDの非上書き退避、署名再確認、
`BCDBoot /c`、新規BCD確認、成功時の退避削除を1トランザクションにしました。
失敗時は部分BCDを除去して旧BCDを復元します。単独起動修復の既存ストア保持方針は
変更していません。単体テストは29/29 PASSです。

## 製品VM回帰

| 経路 | 結果 | 証跡 |
|---|---|---|
| GPT→GPTクローン、コピー先単独UEFI/Secure Boot起動 | PASS | `.validation/evidence/product-gpt-clone-boot-vm/20260803-011915` |
| MBR→MBRクローン、コピー先単独Legacy BIOS起動 | PASS | `.validation/evidence/product-mbr-clone-boot-vm/20260803-023613` |
| VSS/Zstandardイメージ作成物の別GPTディスク復元、単独Secure Boot起動 | PASS | `.validation/evidence/product-vss-restore-vm/20260803-033054` |
| Microsoft標準MBR2GPT、BCD新規再構築、単独UEFI/Secure Boot起動 | PASS | `.validation/evidence/product-mbr2gpt-vm/20260803-062217` |

GPT/MBR試験では保護コピー元VDIのSHA-256が試験前後で一致しました。VSS試験でも
保護イメージのSHA-256が前後一致しています。全経路で製品処理後にコピー元とISOを
切り離し、対象だけでWindowsが起動することを確認しました。BCDの重複メニューは
再現していません。MBR2GPTの最終判定は、製品処理前にターゲットWindowsへ一回限りの
自己削除型VMプローブを安全に配置し、GuestControlやキーボード入力に依存しない
UART証跡で取得しました。このプローブはVM専用で通常製品には含まれません。

## 最新製品媒体

生成ルート:
`C:\Users\Lightning\AppData\Local\YTEC\ytec-disk-clone\final-media-candidate\20260803-063736`

| 媒体 | バイト | SHA-256 |
|---|---:|---|
| 2011 CA製品ISO | 435,118,080 | `EE9D2A6D4F011CEEC57C2E89F5A76A61242F78B25E23B5DE85163ABA37D638BF` |
| 2023 CA製品ISO | 435,103,744 | `DDEE9A786D5D804EE5F6A84EEFB519161BC53B073DAACD07232B00B149184910` |
| MBR2GPT 2023 CA VM専用ISO | 438,394,880 | `726B48FECA4D8C918E65F1B38B097445CF4D0C823580CCBC0C00CDA0D3CF6EA9` |

2011/2023 CA製品ISOはBIOS、UEFI/Secure Boot無効、UEFI/Secure Boot有効の
6条件すべてで日本語GUIを表示しました。目視で文字切れ、重なり、起動エラーなしを
確認しています。証跡は
`.validation/evidence/winpe-product-boot-matrix/20260803-064210`です。
manifestとISO、WIM追加ファイルのSHA-256は一致し、WIMマウント残留と
リポジトリ内Microsoft媒体はいずれも0件です。

## 自動検証

- 通常CTest: 40/40 PASS
- 静的CRT CTest: 40/40 PASS
- ASan CTest: 40/40 PASS
- MSVC `/analyze`: PASS
- PowerShell UTF-8 BOM、ライセンス、SBOM、安全境界、WinPE媒体境界、
  起動マトリクス境界、ポータブル配布境界: PASS

## 最終ポータブルZIP

- パス: `C:\Users\Lightning\AppData\Local\YTEC\ytec-disk-clone\portable-audit\Y-TEC-Tsumugi-Drive-0.2.0-dev-20260803-070454.zip`
- サイズ: 12,971,684バイト
- SHA-256: `DB87454787DB4D487F6E50A8C958B7EEE5247DA7F7D9682A8592CE383CAE786F`
- 15ファイル、`SHA256SUMS.txt`の14件一致、再展開後の全ファイル一致
- Microsoft媒体0件、外部ランタイムDLL 0件、reparse point 0件
- 指定先`G:\マイドライブ\TBDV-0156\アプリ\_zip\Y-TEC-Tsumugi-Drive-0.2.0-dev-20260803-070454.zip`へ
  非上書きで複製し、コピー後SHA-256一致を確認

## 実機へ残す項目

- 実SSD/HDD/NVMeでのGPT/MBRクローン、MBR→GPT、VSS復元、起動修復
- 実USBの作成とBIOS/UEFI/Secure Boot起動
- 接続方式と容量の代表組合せでの速度、残り時間、キャンセル応答
- 電源断相当を含む復旧手順と、正式公開前の法務・署名・版番号判断

これらが終わるまで、本成果物は`0.2.0-dev`の実機受入前候補です。
