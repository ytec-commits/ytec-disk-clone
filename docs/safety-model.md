# Phase 1安全モデル

## 守る対象

最優先はコピー元、ホストOS、接続中の実ディスク、利用者データです。利便性やコピー継続より、誤認時の停止と検証可能性を優先します。

## 現在許可する操作

- アクセス権`0`の物理ディスク列挙と照会専用IOCTL
- ボリュームを`GENERIC_READ`で開く`FSCTL_GET_VOLUME_BITMAP`照会
- 合成メモリディスクへのGPT/パーティション書込みと読戻し試験
- 既定OFFのVM専用ハーネスから、再識別済み追加仮想ディスクをoffline化し、コピー元を読取り専用、コピー先をwrite-throughで開く物理I/O
- 信頼済みSystem32内で埋込み署名またはWindowsカタログ署名を検証したMicrosoft製BCDBootを、固定引数で起動する境界とモック/統合試験
- ローカルADK/WinPE Add-on候補の固定パス、通常ファイル、reparse、署名、`/bootex`を読み取り専用で診断する処理
- 診断ゲート通過後、リポジトリ外のADK/WIM複製だけをマウントし、監査済み自作ファイル3点を追加して検証ISOを生成する明示的な管理者操作
- 製品WinPEAppから、再列挙したコピー元/コピー先の選択条件と対象固有確認トークンを表示する読み取り専用プリフライト
- 製品WinPEAppの復元ジョブだけで、ジョブ/イメージ/対象/必須安全条件/
  二段階確認を同じ呼出し内で再検証し、書込み/削除共有なしの同一
  読取り専用dcimgハンドルを完全検証した後に限って、再識別済みの
  復元先だけをoffline化して復元する対象専用境界
- 縮小復元ジョブだけで、置換不能にロックした`.dcmig`ディレクトリ、正規
  `manifest.dcmig`、全WIMの長さ/SHA-256を検証し、使用量＋安全余白が収まる
  空RAWコピー先だけへGPT/MBR、NTFS、内容を新規作成する境界
- 製品WinPEAppのクローン予約ジョブだけで、ジョブ/コピー元/コピー先/
  対応区画/既知BusType/512バイト論理セクター/空RAWコピー先/
  二段階確認を同じ呼出し内で再検証し、コピー元の同じ読取り専用
  ハンドルを保持した後に限ってコピー先だけをoffline化する対象専用境界
- 縮小クローンでは、コピー元/コピー先と別の第三物理ディスクへ`.dcmig`作業束を
  完全検証・確定してからだけ、同じロック済み束を使ってコピー先へ進む境界
- 既定OFFの破壊的VMビルドだけで、製品と同じ`--clone-execute`境界へ固定プロファイルの実行サービスを明示注入する試験
- メモリ上の`.dcimg` v1作成に加え、読取り専用コピー元を各チャンク1回だけ読み、Zstandard level 3または非圧縮fallbackで抽象ステージング先へ書き、全索引/展開後チャンク/全体SHA-256再読込み後だけcommitし、失敗時は未完了abortするモック境界
- 保存先ディスクを開始時/確定前に再識別し、コピー元/指定コピー先との分離、空き容量、reparse不使用、非上書きを確認して、保護DACL付き新規`.partial`だけを書くWindowsファイルBackend
- オンライン縮小作成で、コピー元の読取り専用ハンドルとVSS Snapshot Volume
  だけを使用し、新規一時`.dcmig`へWIM/manifestを保存して、全件検証、
  `BackupComplete`、Snapshot set削除後だけ完成名へ移動する境界。保存先と作業
  フォルダーは原本と異なる同一の単一物理ディスクへ限定し、書込み直前と確定前にも再確認
- 通常製品から分離したMBR解析、新ディスク署名、Active区画クローンと固定Legacy BIOS VMハーネス
- Microsoft署名済み`System32\mbr2gpt.exe`を`/validate`後の再識別成功時だけ`/convert`で呼ぶモック済み境界と、既定OFFの固定VMハーネス
- 実VSSを呼ばない注入バックエンドで、VSS固定手順、Writer異常停止、Snapshot削除を確認するPhase 5ワークフローモック
- 製品入口へ未接続のWindows SDK VSS具体バックエンドをビルドし、有限timeout/キャンセル、COM/Writer監査、Snapshot Identity、厳密Cleanup、Snapshot専用コピー型をモック、静的解析、専用VMで確認する
- 既定OFFの破壊的VM構成だけで、VirtualBox/管理者/固定許可語/固定C: NTFS/固定合成Sentinelを再検証し、ライブVSSのSnapshot作成・読取り・`BackupComplete`・削除を実行する
- 既定OFFのVM構成だけで、固定128MiB RAW識別ディスクと固定512MiB NTFS保存先VDIを再検証し、合成ReaderからWindows実ファイルBackendへ`.dcimg`を作成・再読込み・非上書き確定する

`FSCTL_GET_VOLUME_BITMAP`の具体プロバイダーは読み取り専用です。製品プリフライトは`executionEnabled=false`を固定します。通常製品の`main`は直接クローン実行サービスを注入しないため、`--clone-execute`を列挙前に拒否します。VMクローンサービスとその固定プロファイルは既定ビルドに含めず、VM版もVirtualBox、固定容量、固定許可語、管理者権限、二段階確認を再検証します。製品版はハッシュ検証済み予約ジョブからだけ別のクローンサービスへ到達し、固定・オンライン・基本GPT/MBRコピー元、空RAWまたは既知の基本GPT/MBR固定コピー先、既知BusType、対応区画、512バイト論理セクターを要求します。通常モードは同容量以上、縮小移行は基本NTFSの使用量＋安全余白が収まる容量を要求します。コピー元の同じ読取り専用ハンドルを保持し、GPT/MBRとVolume対応、MBR署名衝突候補を読取り専用検査した後だけコピー先をoffline化し、先頭・末尾1 MiBを読戻し検証付きで無効化します。失敗/中止時はonlineへ戻しません。通常クローン/復元と、データ専用ディスクの縮小VSS作成・小容量先復元は製品VMで合格済みです。Phase 4 MBR2GPTは製品入口へ未接続で、ホスト実ディスクでは実行していません。

製品復元サービスはコピー元物理ディスクを一切開きません。`.dcimg`は
`GENERIC_READ`かつ書込み/削除共有なしで一度開き、その同じハンドル上で
コンテナ全体、全チャンク、マニフェスト、パーティション表、ジョブ記録済み
長さ/全体SHA-256を検証してから、復元先のoffline化へ進みます。検証済み
Readerと解析結果は単回使用の準備済み復元元として渡し、完全検証を別ハンドルで
やり直しません。非ゼロチャンクは書込み直前にもSHA-256を再確認します。
途中失敗した対象はonlineへ戻さず、明示的な診断/復旧対象として保護します。

VSS具体バックエンドのコピーCallbackは検証済み`GLOBALROOT\Device\HarddiskVolumeShadowCopyN`パスの配列だけを受け取り、元Volume GUIDパスを受け取りません。Snapshot Readerはこの形式だけを`GENERIC_READ`で開き、容量/論理セクターの再確認と有界・整列読取りを要求します。StorageAccessAlignmentPropertyの照会は、Snapshotデバイスが未サポートを明示した場合だけ`GetDiskFreeSpaceW`へfallbackし、それ以外の未知エラーを許可しません。Bitmap ProviderもSnapshot専用Binding型を使い、通常Volume GUIDとの相互混在を拒否します。専用VMでは同一SnapshotからReaderとBitmapを読み、実行前後のShadow Copy 0件を確認済みです。通常`.dcimg`は製品ライブVSS作成、別ディスク復元、単独起動まで接続・確認済みです。縮小`.dcmig`も製品入口へ接続済みで、最終データ専用VM再実行を残します。

## 強制停止条件

- コピー元とコピー先が同一、実行中システムディスクがコピー元/コピー先、安定識別の変化、確認トークン不一致
- 全ディスク再列挙の未解決診断、コピー先のoffline/read-only状態不明、状態変更後の再列挙不一致
- 破損GPT、範囲外/オーバーフロー、重複GUID、重複パーティション
- コピー先容量不足、セクターサイズ不一致、512以外の書込み実行
- 製品クローンのコピー元がoffline/removable/system、コピー先が不明・動的・Storage Spaces、
  BusType不明、Storage Spaces/LDM、未対応GPT/MBRパーティション
- 不明なGPTパーティション種別、無効なFAT32/NTFS、BitLockerシグネチャ
- 読取り、書込み、flush、読戻し比較の失敗
- BCDBootのパス/署名者/実行/終了コード検証失敗
- `.dcimg`の未知版/フラグ/圧縮、範囲オーバーフロー、重複、切詰め、SHA-256不一致
- `.dcimg`のコンテナ/パーティション表寸法不一致、チャンク/パーティション表重複、復元先差替え、読戻し不一致
- `.dcimg`のジョブ記録済み長さ/全体SHA-256不一致、完全検証前後の
  読取り元差替え、準備済み復元元の再利用
- 製品復元先の再識別不能、システム/removable/読取り専用/offline/
  セクター状態不明、offline化後の状態不一致、物理ハンドル寸法不一致
- `.dcimg`ステージングのbegin/write/read/flush/hash/footer/commit/abort失敗、全件再読込み不一致
- `.dcmig`の未知版/非正規manifest、宣言外項目、reparse、WIM名/長さ/SHA-256
  不一致、既存完成/一時出力、VSS cleanup前の完成名確定
- 縮小移行の非NTFS内容、BitLocker未復号、動的/Storage Spaces、不明役割、
  MBR/GPT同時変換、必要最小容量不足、第三ディスクがコピー元/コピー先と同一
- イメージ保存先がUNC/ADS/reparse/既存ファイル、コピー元/指定コピー先と同一、容量不足、開始後の安定識別変化
- MBRの保護/拡張構成、不正Active、範囲重複、32bit LBA超過、ディスク署名衝突
- MBR2GPTの対象WindowsがAMD64 PE32+でない、不正System32、署名不明、validate非ゼロ、validate後の対象差替え/番号変更、convert非ゼロ
- Phase 4 ESP用ドライブ文字が使用中、固定VM区画の割当/解除失敗、ESPの物理対応/GPT種別/FAT32不一致、変換後BCDBoot失敗
- VSSの権限不足、非NTFS、不正/重複Volume GUID、Writer異常/不明、Snapshot対応不整合、BackupComplete/削除失敗
- VSS COMセキュリティ/例外設定失敗、Writer metadata 0件、非同期待機timeout/キャンセル/未知状態、Snapshotデバイス形式不正、コピーCallback未設定
- VSS Snapshot Reader/BitmapのGeometry不一致、範囲外/非整列/非昇順使用範囲、重複パーティションBinding、Snapshot読取り途中消失

## コピー元とコピー先の分離

コピー元APIには書込みメソッドがなく、Windowsハンドルも`GENERIC_READ`だけで開きます。対象はディスク番号だけで選ばず、モデル、容量、論理セクター、シリアル末尾、デバイスインスタンスIDなどの安定信号を組み合わせます。確認画面相当のトークンを作成した後も、実行直前とoffline/online変更後に同一性を再検証します。

## 途中失敗への備え

書込み開始時にターゲットGPTの主/副署名を無効化し、データとメタデータを読戻し検証します。主GPTヘッダーは最後に書くため、途中失敗したコピー先を完成済みとして扱いにくくします。ただし、電源断耐性を保証するものではありません。

## 外部ツールとMicrosoft配布物

Microsoft製EXE/DLL/WIM/ISO/CAB/ADK/WinPEファイルは同梱しません。BCDBootは現在のWindows/WinPEのSystem32にあるファイルをその場で参照し、埋込み署名がない場合もWindowsシステムカタログ内のメンバー署名とMicrosoft署名者を検証してから実行します。WinPEは利用者の端末に導入済みの許可済みADK/WinPE Add-onから、リポジトリ外へローカル生成します。ADK/WinPE Add-onのバージョンと更新状態、対象PCのWindows UEFI 2011/2023 CA対応と失効状態が不明な場合は、Secure Boot互換を断定しません。署名回避、Secure Boot無効化、BitLocker回避は実装しません。

配布版のMediaBuilderは、ビルド時に固定した正確なバイト長とSHA-256へ実行直前に一致した場合だけ、Microsoft署名済みWindows PowerShellへ渡します。端末の既定ExecutionPolicyがスクリプト実行禁止でも、変更済みスクリプトを許可せず、`-ExecutionPolicy Bypass`はこの検証済み子プロセスだけに限定します。レジストリ、ユーザー/マシンExecutionPolicy、UAC設定は変更しません。

ADK診断CLIは候補パスの属性・内容・署名を読むだけで、自動ダウンロード、自動インストール、WIM操作を行いません。別のメディア生成スクリプトは、バージョン/Servicing Update/署名ゲート、AMD64 PE監査、出力先境界、管理者権限をすべて通過した場合だけ起動できます。リポジトリ、ドライブルート、既存出力、reparse配下、元WIMへの変更を拒否し、失敗時はマウントを破棄します。USBは、非システム・USB接続・取り外し可能・オンライン・書込み可能・既知の基本GPT/MBR単一区画、または区画のないRAW/GPT/MBR、安定識別・二段階確認をすべて要求します。単一区画は一意なドライブ文字を物理Extentで照合し、区画なしは既存Extent不在を確認して未使用文字を提案します。書込み直前に同じUSBオブジェクトを再確認し、既存区画がある場合だけそのオブジェクトを`Clear-Disk`へ渡します。区画0件を読戻した後だけ、RAWはMBR初期化、空のGPTはMBR変換、空のMBRは維持します。旧ボリュームのアクセスパスが提案文字へ残った場合は`Get-PSDrive`/`Get-Volume`を再確認して別の未使用文字を選び、実割当文字を同じ物理USBへ再照合してから、最大30 GiBの単一FAT32へ再構成します。作成後も安定識別、実割当文字、全媒体ファイルのSHA-256が一致しなければ完了扱いにしません。

`scripts/check-safety-boundary.ps1`は`WriteFile`、`GENERIC_WRITE`、ディスク属性変更IOCTLを審査済みの物理ターゲット実装だけに制限し、`CreateProcessW`をBootRepair実装だけに制限します。自動検査はコードレビューとVM/実機検証の代替ではありません。
