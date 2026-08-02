# 正式リリース直前チェック

更新日: 2026-08-03

この文書は「実機試験以外が完了した正式リリース直前」を判定するための
作業台帳です。現在の版は`0.2.0-dev`であり、まだ一般公開しません。

## 2026-08-03 実機前ゲート

- [x] BCD新規再構築トランザクションの実装と単体29/29
- [x] GPT/MBR製品クローン後のコピー元/ISOなし単独起動
- [x] VSS/Zstandardイメージの別ディスク復元後単独起動
- [x] Microsoft標準MBR2GPTとBCD再構築後の単独Secure Boot起動
- [x] 最新2011/2023 CA製品ISOのBIOS/UEFI/Secure Boot 6条件
- [x] 通常/静的CRT/ASan各40/40、静的解析、ライセンス、SBOM、安全/媒体境界
- [x] 物理ディスク/USB不使用、NICなし、保護元SHA-256不変、VM設定復元

このゲートの詳細は
[`validation-summary-20260803.md`](validation-summary-20260803.md)に記録しています。

最終監査ZIP:
`C:\Users\Lightning\AppData\Local\YTEC\ytec-disk-clone\portable-audit\Y-TEC-Tsumugi-Drive-0.2.0-dev-sector-fix-20260803-084748.zip`

- サイズ: 12,972,257バイト
- SHA-256: `0399708E540F4CB9D00876F4C8B0F83847AD01C58491F265E5416F9F677487C0`
- Windows EXE SHA-256: `1A10B5CBF393E7C62D5C96A6C66E03C5D1EFEB2EA46D19F81EC38CAEC47598E0`
- ファイル数: 15（`SHA256SUMS.txt`自身を除く14ファイルを記録）
- 元フォルダー/ZIP/新規再展開フォルダーの全ハッシュ一致
- Microsoft媒体0件、外部ランタイムDLL 0件、reparse point 0件
- 08:29の実機初回試行で判明したNVMe 16KiB物理セクター対応修正を収録。
  07:04版ZIPはこの試行には使用しない

## 旧監査パッケージ（現在差分ではない）

- リポジトリ外ZIP: `Y-TEC-Tsumugi-Drive-package-20260801-152105.zip`
- サイズ: 12,002,312バイト
- SHA-256: `A05C5BC2555175D026FBAD72209311C19742211CDD33F491439BDCF22F53666E`
- ファイル数: 15（SHA256SUMS自身を除く14ファイルを記録）
- Microsoft EXE/DLL/WIM/ISO/CAB/ADK: 0件
- 外部ランタイムDLL依存: 0件
- 再配布する第三者UI資産: LINE Seed JP Regular/Bold 1件（OFL-1.1）
- 性格: 2026-08-01時点の開発用監査成果物。Zstandard接続後の現在差分ではなく、
  正式な配布許諾を示さない

現在のソース依存はZstandard v1.5.7（BSD-3-Clause）とLINE Seed JP
（OFL-1.1）の2件です。次の配布候補は最新ISO/VM回帰後に新規作成し、
この旧ZIPを上書きまたは実機試験へ流用しません。

2026-08-02の現在差分については、Gドライブへ置かない開発監査用として
`C:\Users\Lightning\AppData\Local\YTEC\ytec-disk-clone\portable-audit\Y-TEC-Tsumugi-Drive-0.2.0-dev-20260802-154741.zip`
をBCD用途別方針の実装後に新規作成しました。12,955,755バイト、SHA-256
`26B0956246A013A7AED5FC1A22F4AC06B2F7BE335C63BF48FC498A32A1182E13`、
15ファイル、SHA256SUMS自身を除く14ファイルのハッシュ一致、Microsoft媒体0件を
再展開監査済みです。最新ISO、Zstandard版VSS復元、正式アイコン、実機試験前の
ため、これも利用者向け配布候補ではありません。

## 非昇格で完了した確認

- [x] C++20 / CMake / MSVC x64通常ビルド
- [x] 静的ランタイム版Windows/WinPE EXEビルド
- [x] CTest通常/静的/ASan各40件
- [x] MSVC静的解析（現在差分の警告なし）
- [x] PowerShell構文/BOM/安全境界
- [x] 禁止依存/ライセンス/SBOM検査
- [x] ポータブルZIPの全ファイルSHA-256と再展開監査
- [x] Windows版1280x720実描画
- [x] WinPEのWinRE読取り専用診断GUI、根拠不足拒否、1024px以上の操作列レイアウト
- [x] 512バイト論理セクター限定の16 MiB製品I/O方針と75%要求数削減試算
- [x] 最新静的WinPE CLI/GUIのSecure Boot有効VM起動
- [x] 使用済みGPTコピー先の読取り専用拒否
- [x] 製品予約ジョブの2GiB GPT→3GiB RAW実WinPE完走と再実行拒否
- [x] ISO/USB対象固定、二段階確認、差替え拒否のモック
- [x] USB全媒体ファイルSHA-256照合の非書込み境界
- [x] 操作ガイド、安全上の注意、通信/プライバシー、報告文書の同梱

## UAC付き専用VMの確認

- [x] 全最新差分、日本語フォント、2011/2023 CAを含む完成候補ISO/manifest発行
- [x] 完成ISOのBIOS、UEFI、Secure Boot 2011 CA/2023 CA自動起動
- [x] 製品VSS→実ファイル`.dcimg`の成功/中止/容量不足/残留Snapshot 0件
- [x] 製品予約ジョブのMBRクローンと、GPT/MBR中止/offline/online回帰
- [x] 製品予約ジョブのGPT/MBR起動ディスクを複製し、コピー元/ISOなしで
  Secure Boot有効UEFIまたはLegacy BIOSから起動
- [x] 製品予約ジョブの`.dcimg`復元成功/破損拒否/中止/offline/online
- [x] 製品VSS/Zstandard `.dcimg`の別合成ディスク復元と単独Secure Boot起動
- [x] Legacy BIOS x64単独起動修復の破損前後起動
- [ ] 対象限定USB作成の専用VM実測と全媒体ハッシュ
- [ ] 速度、残り時間、キャンセル応答のVM実測

製品`.dcimg`復元の成功系は2026-07-31に完了しました。新規8MiB RAW VDIへ
正規MBRイメージを復元し、完全検証、2チャンク読戻し、MBR確定、online復帰、
独立読取り照合までPASSです。2026-08-01に破損拒否、中止、offline維持も
専用VMで完了しました。

2011/2023 CA完成ISOとmanifest、6条件の起動マトリクスは2026-08-01午前の
製品差分で完了しています。その後にLINE Seed JP埋込みとOFL本文同梱を追加したため、
正式候補としては新しいISOの再生成と同じ起動マトリクスを残します。

2026-08-02の製品起動ディスク試験は、GPTで
`.validation/evidence/product-gpt-clone-boot-vm/20260802-135917`、MBRで
`.validation/evidence/product-mbr-clone-boot-vm/20260802-143059`へ記録しました。
いずれも保護したコピー元VDIのSHA-256前後一致、NIC無効、実ディスク/USB不使用、
Worker設定復元を確認しています。検証ISO内の旧ランナーはWinPEに存在しない
`findstr.exe`による後処理だけ失敗しましたが、製品プロセスはPASSで終了し、
複製先単独のWindows側検証を最終判定に使用しました。ランナーの修正は済んでおり、
次回ISO再生成へ反映します。

UACは手動承認だけを使います。UAC無効化、自動承認、ExecutionPolicyの
永続変更、Secure Boot/署名回避は行いません。

## 実装・仕様として残る項目

- [x] Zstandard圧縮。公式v1.5.7をBSD-3-Clause条件で承認・実装・製品接続済み
  （圧縮版の実VSS VM生成/復元/起動回帰も完了）
- [x] `.dcimg`分割ファイルは最上位仕様12.7で初版任意のため、v1では非採用
- [x] WinPE GUIの固定名ジョブ自動検出/読取り専用プリフライトと、
  実行結果ログの新規保存/全バイト読戻し
- [x] Windows版の固定位置結果ログ読取り専用検出、厳格検証、最新結果表示
- [x] Windowsで既定OFFの一回限り自動実行を明示したv3ジョブと、
  WinPEでのSHA-256連動開始記録/二重自動実行防止（単体・ファイル試験）
- [x] ジョブ保存後のWindows詳細起動オプション再起動提案、標準権限/UACなし・
  Windows 7の手動Boot Menu案内（モックのみ、実再起動は未実施）
- [ ] 詳細起動オプションから利用者生成WinPE USBを選ぶVM/実機実測
- [ ] auto-onceジョブの更新ISO上VM実測（同一ジョブ再実行拒否を含む）
- [ ] MBR→GPT別ターゲット再構築の物理実行アダプターとWinRE登録
- [x] Windows版のWindows Update再起動保留を公式WUAの読取り専用
  `RebootRequired`で判定し、あり/判定不能を最終確認で警告
- [x] 未割当ESP/Active領域の製品GUIからの一時割当/必須解除を実装・モック検証
  （実WinPE VMでの一時割当/解除は検証項目として残る）
- [x] 新規クローン/復元/MBR→GPTコピー先はBCDBoot固定`/c`で新規BCDを構築し、
  単独起動修復は既存マルチブートを保持する用途別方針を実装・単体検証
  （既存BCD退避/失敗時復元を含むトランザクションと最新ISO VM再確認済み）
- [ ] 複数DPI、高コントラスト、スクリーンリーダー実測
- [ ] 既存の3本の糸モチーフを使う正式アプリアイコンの確定とWindows/WinPE共通適用

WinPE側では、実行中OSが復元対象Windowsではないため、オフライン対象の
再起動保留を安全と推定しません。警告項目は`unknown`のまま表示し、必須の
BitLocker/基本ディスク/Storage Spaces/ファイルシステム/電源判定とは
分離します。

## Y-TEC側で決定が必要な公開条件

- [ ] プロジェクト自身の`LICENSE`または利用条件
- [ ] Visual Studio / MSVCの組織内使用・配布資格の確認
- [ ] コード署名証明書を採用するか
- [ ] 正式な製品バージョン
- [ ] プライバシーポリシーURL、運営者表示、問い合わせ先
- [ ] セキュリティ報告先URLまたはメールアドレス、返信目安
- [ ] サポート範囲、免責、復旧不能時の扱い
- [ ] 「Y-TEC Tsumugi Drive」の商標最終確認
- [x] Zstandard v1.5.7をBSD-3-Clause条件で採用

## 最後に利用者が行う実機受入

詳細な開始ゲート、証跡、拒否試験、高リスク項目は
[`real-hardware-acceptance-checklist.md`](real-hardware-acceptance-checklist.md)を使用します。

- [ ] GPT/UEFIクローン後、コピー元を外して起動
- [ ] MBR/Legacy BIOSクローン後、コピー元を外して起動
- [ ] MBR→GPT後、UEFI/Secure Bootで起動
- [ ] VSSイメージ作成→別ディスク復元→起動
- [ ] 起動修復のみでUEFI/BIOSのBCD再構築
- [ ] ISO/USBをBIOS、UEFI、Secure Boot 2011/2023 CAで起動
- [ ] 同容量/大容量、HDD/SSD/NVMe/SATA/USBの代表組合せ
- [ ] 重要でない合成データで中止、失敗、電源断相当の復旧手順を確認

実機受入と依存/法務/署名判断がすべて終わるまで、
開発候補のハッシュを正式版ハッシュとして公開しません。
