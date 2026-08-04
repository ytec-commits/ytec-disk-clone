# Y-TEC Tsumugi Drive

**Y-TEC Tsumugi Drive（ワイテック・ツムギ・ドライブ）** は、Windowsディスクのクローン、イメージバックアップ、復元、起動修復を安全側に実行するための開発中ツールです。リポジトリ名、名前空間、実行ファイル名、SBOM Package IDなどの内部識別子は、互換性維持のため当面`ytec-disk-clone`を継続します。

Windows 10 / 11 x64向けディスク移行・復旧ツールです。現在は **Phase 1のGPT/UEFI、Phase 3のMBR/Legacy BIOS、Phase 4のMBR2GPT/UEFI、Phase 5のVSSイメージ作成・別ディスク復元を製品経路の最終VM回帰まで確認済み** です。Phase 0の読み取り専用ディスク診断に加え、GPT/UEFIコピー先単独のSecure Boot起動、MBR/BIOSコピー先単独起動、Windows 10 x64 MBR媒体のMicrosoft標準MBR2GPT変換後のSecure Boot起動、VSS/Zstandardイメージからの復元後起動を確認しています。通常モードに加え、Windows/データ専用ディスクを使用量＋安全余白が収まる小容量ディスクへ再構成する`縮小移行モード`を実装しました。4 GiBの合成データ専用原本から2 GiBの合成コピー先へVSSイメージ作成・縮小復元し、原本不変とファイル一致を確認する最終VM回帰も合格しています。

## 現在できること

- 物理ディスクとパーティションをWindows公開APIで読み取り専用列挙する
- ディスク番号、モデル、容量、セクターサイズ、BusType、シリアル末尾、GPT/MBR/RAW、パーティション一覧をテキストまたはJSONで表示する
- Windows GUIの実行ごとにアプリ横の`logs`へUTF-8診断ログを新規保存し、権限、OS版、読取り専用ディスク要約、VSS/媒体/復元の工程とエラーコードを記録する。ファイル名一覧、ユーザープロファイルの絶対パス、デバイスパス、完全シリアル、回復キー等は記録しない
- オンラインイメージ作成の確認直前に、公式Windows Update Agentの
  読取り専用`RebootRequired`を確認し、再起動保留あり、または判定不能なら
  「問題なし」とせず目立つ警告を表示する
- 保護MBR、主/副GPTヘッダー、パーティション配列とCRCを厳密に解析する
- 合成ディスク上で、新しいDisk GUID/Partition GUIDを持つGPTクローンを計画・実行・読戻し検証する
- GPT/MBRクローンと合成dcimg復元で、工程、パーティション、
  読取り/書込み/読戻し検証済み容量を別々に単調通知し、
  パーティション表最終確定前だけ安全にキャンセルする
- Windows起動ディスクとデータ専用ディスクを、通常モードでは構造を維持した
  クローン/`.dcimg`イメージ作成・復元として扱う。データ専用ディスクでは
  起動構成を作らず、GPT/MBR基本ディスクの内容だけを保全する
- `縮小移行モード`では、コピー元を縮小・変更せず、基本NTFSの実使用量と
  安全余白からコピー先の必要最小容量を計算する。直接クローンは別の物理ディスクへ
  検証済み`.dcmig`作業イメージを確定してから、小容量のコピー先だけを再構成する
- オンライン縮小イメージ作成ではVSS Snapshotから各NTFS内容をMicrosoft WIMへ
  取得し、全WIMの長さ/SHA-256、正規`manifest.dcmig`、Snapshot削除を確認後だけ
  `.dcmig`ディレクトリを確定する。復元では同じ束を読取り専用ロックして完全検証し、
  GPT/MBR形式を維持して展開する。WindowsディスクだけBCDBootで新規BCDを作る
- WinPE表示モデルで検証済み容量から進捗率、速度、経過時間、
  安定後の残り時間、中断可否を日本語表示へ変換する
- EFI FAT32と回復NTFSは全領域、基本NTFSは使用クラスタ範囲、MSRは領域のみ再作成する
- 安定ディスク識別と二段階確認（対象詳細＋確認語`OK`）で、対象変更時は書き込み前に停止する
- 実行中Windowsのシステムディスクをコピー元/コピー先から除外し、再識別済みのコピー先だけを一時的にoffline化して生ディスクI/Oを開く
- コピー元は`GENERIC_READ`だけ、コピー先の書込み/flush/読戻しは監査対象の1実装だけに隔離する
- 埋込み署名またはWindowsカタログ署名とMicrosoft署名者を検証した、現在のWindows/WinPEの`System32\bcdboot.exe`だけを固定引数で実行する。新しいクローン/復元/MBR→GPTコピー先は`/c`で新規BCDを構築し、単独起動修復は正当なマルチブートを守るため既存ストアを保持する
- 製品WinPEAppで、クローンを伴わないUEFI/BIOS単独起動修復の読み取り専用プリフライトと二段階確認付き実行を受け付ける
- Windows/システム領域が同じ非システム物理ディスク上のNTFS＋ESP FAT32または一意なActive MBR領域であること、Windows x64、安定識別、BCDBoot後のBCDストアを検証する
- ローカルADK/WinPE Add-onの固定候補、必要ファイル、reparse、Microsoft署名、`/bootex`対応を読み取り専用で診断する
- Windows GUIの4段階レスキューメディアウィザードで、診断合格後にISO/USB、Secure Boot 2011/2023 CA、出力先を選び、書込み前の作成内容まで確認する。不足時はMicrosoft公式のADK/WinPE導入ページと必須更新ページを既定ブラウザーで開き、導入後に再検査できる。ISOはローカル絶対`.iso`新規パス、USBは安定識別可能な非システム・USB接続・取り外し可能・オンライン・書込み可能ディスクだけを候補にし、物理ディスク番号とローカルドライブ文字の単一ボリューム範囲をゼロアクセスで一意照合する。管理者として手動起動した配布版では、同梱した自作ファイルとSystem32の署名済みWindows PowerShellを再検証し、完成ISO/manifestを非上書きで確定する製品サービスへ接続済み
- 静的MSVCランタイムでビルドした製品`ytec-winpe-app.exe`をWinPE上で実行し、物理ディスクをテキスト/JSONで読み取り専用診断する
- 静的MSVCランタイムの製品`ytec-winpe-gui.exe`で、Tsumugiの糸を表す配色と4段階ステップ表示を使い、「予約ジョブ」「起動修復のみ」「ディスク診断」を1280x720の1画面から操作する。固定名ジョブは有界自動検出して読取り専用プリフライトへ進む。通常ジョブと旧v2は対象消去確認/確認語を要求し、Windows側で既定OFFの一回限り自動実行を明示したv3ジョブだけは、プリフライト合格後にジョブSHA-256へ結び付いた開始記録を新規保存・読戻ししてから実行する。同じジョブは自動で再実行しない。クローン/復元ジョブは改ざん、確認後の差替え、対象再識別、安全条件を検証し、復元時はイメージも完全検証して、承認後だけ種別ごとの製品サービスへ渡す。実行試行後はジョブ横へ結果ログを新規保存・読戻しする。起動修復も読取り専用確認と対象固有語による二段階確認後だけ既存のBCDBoot実行境界へ渡す
- Windows版とWinPE版のGUIは、承認済みのLINE Seed JP `LINESeedJP_20241105` Regular/Boldを実行ファイルへ埋め込み、OSへインストールせずプロセス内限定で使用する。読込み失敗時は`Yu Gothic UI`へフォールバックする。公式配布条件はSIL Open Font License 1.1で、著作権表示、ライセンス本文、版、公式ZIPと収録ファイルのSHA-256を依存台帳・通知・SBOMへ記録する
- WinPEAppで`--clone-preflight --source N --target N`を受け付け、再列挙した2台の安定識別、容量、セクター、システムディスク除外、コピー先が空RAWまたは既知の基本GPT/MBRであることを読み取り専用で検査する
- プリフライトは短い確認語`OK`を表示し、`executionEnabled=false`を固定する
- `--clone-execute`は実行サービスを明示注入できる境界を持つが、通常製品の`main`はサービスを注入せず、ディスク列挙前に終了コード`64`で拒否する
- 許可済みADKのWIM複製へWinPE CLI/GUI、第三者通知とLINE Seed JPのOFL本文、ローカルADKの日本語フォントサポートだけを監査付きで追加し、リポジトリ外へ製品ISOを生成する境界を持つ。CLI/GUIのAMD64 PE32+、固定System DLL、SHA-256を検査し、製品媒体はGUIを自動起動する
- 配布版からMediaBuilderを実行する際は、スクリプトの長さとSHA-256をEXEへビルド時固定し、完全一致した場合だけWindows PowerShellの実行ポリシーをその子プロセス内に限定して指定する。端末全体のExecutionPolicyやUAC設定は変更しない
- 専用VirtualBox VMで2GiB GPTから3GiB RAWへの物理I/O、GUID再生成、読戻し、ファイルハッシュ照合を完走する
- UEFI64/Secure Boot有効の実WinPE VMで、製品と同じCLI境界へVM専用サービスを注入し、2GiB GPTから3GiB RAWへのクローンを完走する
- 実WinPE VMで96GiB Windows 10 GPTから110GiB RAWへクローンし、署名検証済みBCDBootを実行後、コピー元を外したコピー先単独のSecure Boot有効UEFI起動を確認する
- 作業中`.dcimg` v1の固定ヘッダー、Zstandardプロファイル1/非圧縮/ゼロチャンク索引、Windows CNG SHA-256、独立ハッシュ表、全体ハッシュを合成メモリ上で作成・検証する
- オンラインイメージ作成では論理512/4096バイトと、論理以上の2の累乗・整数倍・
  64KiB以下の物理セクターを検証し、Windows 11/NVMeが報告する16KiBも記録する
- 読取り専用コピー元から16/32MiB以下のチャンクをZstandard level 3で圧縮し、圧縮後が小さくならないチャンクだけ非圧縮へ戻して抽象ステージング先へ一度書く。全索引・全チャンク・全体ハッシュの再読込み成功後だけcommitし、失敗時は未完了としてabortする
- Windows実ファイル保存先を開始直前に再列挙し、コピー元/コピー先との安定識別分離、空き容量、ローカル絶対パス、reparse不使用、既存ファイル非上書きを確認後、保護DACL付き新規`.partial`だけへ書く
- `.partial`を全件再読込み・flushした後だけ、所有中ファイルハンドルから非上書きで`.dcimg`へ確定し、失敗時は所有している未完了ファイルだけを削除する
- `.dcimg`のMBR/GPTパーティション表スナップショットを検証し、合成ターゲットで全体検証後のデータ復元とパーティション表最終確定を行う。大容量イメージは全量をRAMへ載せず、Reader経由で完全検証した後、非ゼロチャンクを最大32MiBだけ保持して復元直前にもSHA-256を再確認する
- Windows GUIからローカル`.dcimg`を読取り専用で選び、全体/全チャンクSHA-256、正規マニフェスト、パーティション表、復元領域を有界完全検証する。列挙済み情報だけで復元先候補を基礎確認し、二段階確認後は復元予約ジョブだけを新規保存・読戻し検証する。復元先は開かず実行は無効
- Windows GUIの「ログ・診断」で固定/リムーバブル媒体の固定位置にあるWinPE結果ログだけを読み取り専用で検出し、正規UTF-8、詳細長/SHA-256、完全再生成、ファイル名との種別/UTC一致を厳格検証して最新結果を表示する。不正候補があれば部分表示しない
- WinPEAppの復元ジョブプリフライトで、ジョブSHA-256に加えて`.dcimg`全体を再検証し、復元先容量、論理セクター、元ディスクとの分離を確認する。成功時も`executionEnabled=false`
- 製品WinPEAppの`--job-execute`でクローン/復元ジョブを受け付け、二段階確認を同じ呼出し内で再実行する。クローンは固定・オンライン・基本GPT/MBRコピー元と、同容量以上の空RAWまたは既知の基本GPT/MBR固定コピー先だけを許し、コピー元の同一読取り専用ハンドルを保持してからコピー先だけをoffline化する。コピー先の先頭・末尾1 MiBを読戻し検証付きで無効化して既存NTFS等を置換し、GPT/MBRの全書込みを読戻し検証し、パーティション表確定後だけonlineへ戻す。製品復元サービスはコピー元物理ディスクを開かず、ロック済みの同一dcimgハンドルで完全検証してジョブSHA-256へ一致した後だけ復元先をoffline化する。どちらも失敗した部分対象はonlineへ戻さず保護する
- MBRの4プライマリエントリ、Active、範囲を厳密に解析し、合成ディスク上でNTFS使用クラスタ/回復NTFS、新しいディスク署名、読戻し、MBR最終確定まで実行する
- 署名検証済みBCDBoot境界で`/f UEFI`と`/f BIOS`を固定値として明示的に選択する
- 実WinPE VMで48GiB Windows 10 MBRから56GiB RAWへクローンし、署名検証済みBCDBoot `/f BIOS`後、コピー元を外したLegacy BIOS単独起動を確認する
- 署名検証済み`System32\mbr2gpt.exe`を固定`/validate`後の対象再識別成功時だけ固定`/convert`で呼ぶPhase 4境界をモック検証する
- Phase 4変換前にオフラインWindowsカーネルのPEヘッダーを検査し、AMD64 PE32+以外を安全側に拒否する
- Microsoft署名を確認した現在の`System32\reagentc.exe`へ固定`/info /target`だけを渡し、オフラインWindowsのWinRE登録先を対象ディスクへ照合する。登録先または固定のWindows内フォールバックにある`Winre.wim`を読取り専用・非reparse・容量上限付きで確認し、確認済み事実だけをMBR→GPT別ターゲット再構築プランへ渡す。製品WinPE CLIの`--winre-diagnostic --disk N --windows-root W:\`と「起動修復のみ」GUIの`WinREを診断`へ接続済み。GUIは登録済み回復領域、Windows内の補完候補、根拠不足による停止を分けて表示し、この操作だけでは変換や書込みを開始しない。実ディスクを使わないモック、最小幅レイアウト、MSVC静的解析で検証済み
- MBR→GPTの別ターゲット再構築を、コピー元へ書かない純粋プランとして計算する。現在のMicrosoft推奨に合わせて512/512eは200 MiB、4Knは300 MiBのESP、16 MiBのMSR、Windows、990 MiB以上かつWinREイメージ＋250 MiB以上の回復領域、任意データの順に配置する。回復領域がWindowsより前なら後ろへ移し、欠損/狭小時は確認済みWinREイメージがある場合だけ新規計画する。各Microsoft型GUID、Windows REの必須＋ドライブ文字なし属性、保護MBR、主副GPT、最後に主ヘッダーを確定するメタデータ列をメモリ上で生成し、`/validate`、再識別、空ターゲット確認、内容移行、Windows領域拡張、BCDBoot、WinRE登録、全読戻し、単独UEFI起動を順序付き実行契約として再照合する。実行アダプターは未接続
- Windows 10 x64 Legacy BIOS/MBR専用VMでMicrosoft標準MBR2GPTを完走し、GPT/ESP再列挙、署名検証済みBCDBoot、コピー元/ISOなしのUEFI64起動とSecure Boot有効起動を確認する
- VSSの固定ワークフロー、NTFS/Volume GUID検証、Writer異常停止、Snapshot必須削除をモックで検証する
- Windows SDK標準VSS COMバックエンドで有限timeout/キャンセル、Writer監査、厳密なSnapshot Identity/Cleanup、Snapshot専用の読取り/Bitmap/コピー境界を実装し、モック・静的解析・専用VMの実VSSで検証する
- 固定VirtualBox/管理者/許可語/C: NTFS/合成Sentinelだけを許すPhase 5ライブVSS検証ハーネスを製品・CTestから分離し、Snapshot内Sentinel、raw boot sector、NTFS geometry、Snapshot bitmap、Writer 10件、`BackupComplete`、Shadow Copy残留0件を専用VMで確認する
- 固定128MiB RAW識別ディスクと512MiB NTFS保存先VDIだけを許すVM専用ハーネスで、Windows実ファイルBackendによる非圧縮`.dcimg`の新規作成、全件再読込み、全ハッシュ、非上書き確定、`.partial`後始末、ホスト側SHA-256一致を確認する
- 製品VSS経路でZstandard `.dcimg`を作成し、別の合成GPTディスクへ復元して、コピー元とISOを外した対象だけでSecure Boot起動する最終回帰を確認する

## 実機受入・正式公開まで残ること

- 物理ディスクと実USBを使う代表実機での互換性、性能、残り時間、
  キャンセル応答、電源断相当の受入試験
- 単独起動修復で未割当ESP/Active領域を一時割当・解除する経路の実機受入
- MBR→GPTのメモリ上のGPTメタデータ計画を、実際の別ターゲットへの安全な
  適用、Windows内容移行、WinRE配置/登録、BCDBoot、全量再検証へ接続する処理。既存ディスクを
  その場で自動移動する方式は通常機能に含めません。現在の製品機能は
  Microsoft標準MBR2GPTが受理するWindows 10/11 x64構成を対象にします
- BitLockerボリュームのクローン（現在はシグネチャを検出して明示停止）
- 4Knへの書き込み（実機相当検証まで512バイト論理セクターに限定）
- コード署名、正式版番号、商標/法務、公開先とサポート条件の決定

物理ディスクI/Oのうち、GPT/MBRクローン、VSSイメージ作成/復元、MBR→GPTは通常製品の検証済み経路へ接続しました。2026-08-03の最終VM回帰では、BCD新規再構築トランザクションを含む最新コードと最新ISOを使い、GPT/MBRコピー先、VSS復元先、MBR→GPT変換先をコピー元とISOなしで単独起動しました。保護コピー元または保護イメージのSHA-256前後一致、NICなし、合成VDI限定、VM設定復元、稼働中VM 0台も確認しています。詳細は[実機前最終検証サマリー](docs/validation-summary-20260803.md)を参照してください。直接`--clone-execute`と破壊的VMハーネスは通常製品から分離し、既定ビルドで無効です。実機受入前のため、重要データや実運用では使用しません。

## 起動方式の方針

完成版はBIOS（Legacy）/UEFIの両方を対象にします。WinPEメディアも、利用者のPCに導入されたMicrosoft ADK/WinPE Add-onからローカル生成し、BIOS/UEFI両対応を目指します。Secure BootではMicrosoft標準の署名済みブート部品だけを使用し、無効化や回避を行いません。

2026年はWindows UEFI 2011 CAから2023 CAへの移行を考慮する必要があります。MediaBuilderではADK/WinPE Add-onと更新状態を確認し、対応ADKの`MakeWinPEMedia /bootex`で作る2023 CAメディアと従来互換メディアを区別します。実際に起動できるかは対象PCのファームウェア証明書と失効状態にも依存するため、無条件の互換性は保証せず、VMと代表実機で組合せを検証します。

Phase 1はGPT/UEFI、Phase 3はMBR/Legacy BIOS、Phase 4はx64 MBR→GPT/UEFI移行が対象です。最新製品CLI/GUIとLINE Seed JP/OFL本文を収録した2011/2023 CA ISOは、Legacy BIOS、UEFI、Secure Boot有効/無効の6条件で起動確認済みです。GPT/MBRクローン、VSS/Zstandard復元、Microsoft標準MBR2GPT後の単独起動も2026-08-03の製品VM回帰で確認済みです。実機回帰前であり、この検証だけで一般利用者向け書込み機能を実運用可とは扱いません。

## 安全境界

- コピー元は読み取り専用インターフェースからしか渡せない
- ディスク番号だけを同一性判断に使わず、モデル、容量、論理セクター、シリアル末尾、デバイスインスタンスIDなどを照合する
- 破壊的操作の直前とoffline/online変更後に全ディスクを再列挙し、未解決の列挙診断、対象差替え、システムディスク、状態不一致では停止する
- 不明なGPT種別、破損GPT、BitLocker、範囲外I/O、読戻し不一致は成功扱いにしない
- ターゲットGPTは主ヘッダーを最後に確定し、途中失敗時に完成ディスクと誤認しにくくする
- Microsoft製EXE/DLL/WIM/ISO/ADKファイルをリポジトリへコピーしない

## 必要環境

- Windows 10 / 11 x64
- CMake 3.25以上
- MSVC x64 C++ Build Tools
- Ninja
- PowerShell 7またはWindows PowerShell 5.1

外部C++ライブラリと外部テストフレームワークは使用していません。

## ビルドとテスト

通常のPowerShellから次を実行すると、MSVC x64環境を初期化し、ビルド、CTest、ライセンス/安全境界/SBOM検査、`/analyze`、AddressSanitizerを実行します。
CIはWindows PowerShell 5.1でも日本語スクリプトを誤解釈しないよう、
全`.ps1`のUTF-8 BOMを開始時に検査します。媒体manifestなど
UTF-8 BOMなしが必要な生成物は、PowerShell 7専用の
`-Encoding utf8NoBOM`を使わず.NETの明示的なUTF-8 encodingで保存します。

```powershell
./scripts/ci.ps1
```

Windows版GUIは次で起動できます。クローン画面では読み取り専用で
ディスクを選択し、コピー先情報の確認と確認語`OK`の完全一致後に、
ハッシュ付きWinPE引継ぎジョブを新規保存できます。既存ファイルは上書きせず、
保存後に全バイトとハッシュを再検証します。「イメージを作成」はWindows
起動ディスクとデータ専用GPT/MBR基本ディスクを表示し、標準権限では無効、
自動UACなしです。通常`.dcimg`と縮小`.dcmig`を選択できます。
管理者実行時は再識別済み読取り専用物理ディスク、VSS Snapshot、実ファイル
  Backendを通り、全件読戻し、`BackupComplete`、Snapshot削除後だけ新規
  `.dcimg`ファイルまたは`.dcmig`ディレクトリへ確定します。通常`.dcimg`経路は
  固定VMで容量不足、キャンセル、正常完了を確認済みです。縮小`.dcmig`経路は
  対策後の最終VM再実行を残しています。
「レスキューメディア」は標準権限でもADK/WinPE、Microsoft署名、
検証済みバージョン、必須更新、`/bootex`を読み取り専用診断できます。
不足時はMicrosoft公式の導入ページと必須更新ページを既定ブラウザーで開き、
導入後に同じ画面から再検査できます。合格後はISO/USB、2011/2023 CA、出力先を順番に選択し、
ISOの新規ローカルパスまたは安全条件を満たすUSBだけを作成内容確認へ
進められます。標準権限では診断と確認だけで安全に停止し、自動UACは
要求しません。利用者が配布版を管理者として手動起動した場合だけ、
ADK/出力先/同梱自作ファイルを再検証してISOまたはUSBを作成し、進捗、
経過時間、目安残り時間、最終SHA-256を表示する製品サービスへ進みます。
USBは取り外し可能、USB Bus、非システム、書込み可能、オンライン、
既知の基本GPT/MBR単一区画、または区画のないRAW/GPT/MBRを要求します。
区画のないUSBには未使用ドライブ文字を読み取り専用で提案します。初期化中に
旧ボリュームのアクセスパスが残った場合は、初期化後に全ボリュームを再確認して
別の未使用文字へ切り替え、実際の割当文字を同じ物理USBへ再照合します。
短い確認語`OK`の入力後も、書込み直前と作成後に安定識別情報を再列挙し、
選択USBディスク全体だけをMBR・最大30 GiBの単一FAT32へ自動初期化してから、
ローカルADKのMicrosoft公式`MakeWinPEMedia.cmd`へ渡します。完了時はUSB上の全媒体ファイルを
元の作業媒体とSHA-256で照合します。実USBでは全消去後に空のGPTとして返り、
区画照会を0件ではなく`ObjectNotFound`で返す機種も確認したため、同じUSBを
安定識別で再照合した場合だけ空として扱う回復経路を追加済みです。修正版による完成確認は未実施です。
「イメージを復元」は標準権限でローカル通常`.dcimg`を選び、全体/
全チャンクSHA-256、マニフェスト、パーティション表、復元領域を
最大4 MiB単位で完全検証します。合格しても復元実行は無効で、
列挙済み情報から復元先候補を選べます。候補は安定識別、元ディスクとの
同一性、現在のWindowsディスク、読取り専用/リムーバブル/不明状態、
パーティション形式、容量、論理セクターを安全側に確認します。
対象情報の確認と確認語`OK`の完全一致後は、ハッシュ付き復元予約ジョブだけを
既存ファイル非上書きで保存し、全バイトとSHA-256を読戻し検証します。
この段階では復元先ディスクを開いたり変更したりせず、実行前の詳細検査と
物理復元は行いません。

```powershell
./out/build/msvc-x64/src/WindowsApp/ytec-tsumugi-drive.exe
```

静的ランタイム版のポータブルZIPは、リポジトリ外の新規パスだけへ作成します。
実ZIPは日本語ファイル名を保持するUTF-8ハッシュ一覧、全ファイルSHA-256、
Microsoft媒体の不在まで検証できます。

```powershell
./scripts/New-PortablePackage.ps1 `
  -OutputRoot C:\TsumugiRelease\Y-TEC-Tsumugi-Drive-0.2.0 `
  -BuildPackage
./scripts/Test-PortablePackageArtifact.ps1 `
  -PackageRoot C:\TsumugiRelease\Y-TEC-Tsumugi-Drive-0.2.0 `
  -ZipPath C:\TsumugiRelease\Y-TEC-Tsumugi-Drive-0.2.0.zip
```

個別に実行する場合:

```powershell
cmake --preset msvc-x64
cmake --build --preset msvc-x64
ctest --preset msvc-x64
```

Phase 1の破壊的VM試験は既定ビルドへ含めません。専用VirtualBox
VDI、固定容量、VM識別、二段階確認、個別の許可語を満たす場合だけ、
`msvc-x64-vm-destructive`構成のハーネスを利用できます。起動可能クローン試験は
`scripts/Invoke-Phase1BootVmTest.ps1`が専用Workerとコピー元不在の
Boot-Check VMを直列制御し、UACの「はい」だけを人間が手動承認します。
ホスト物理ディスクを試験対象にはしません。

製品WinPE ISOの起動マトリクスは、HDD/NICなし専用VMだけを使用し、
各試験後に元のUEFI/Secure Boot/ISOへ復元します。画面証跡は自動で
成功扱いにせず、目視確認を別記録に残します。

```powershell
./scripts/Invoke-WinPEProductBootMatrixVmTest.ps1 `
  -Iso2011Ca C:\path\YDC-WinPEApp-amd64-2011CA.iso `
  -Iso2023Ca C:\path\YDC-WinPEApp-amd64-2023CA.iso
```

## 読み取り専用CLI

```powershell
./out/build/msvc-x64/src/CliTools/ytec-disk-inventory.exe --text
./out/build/msvc-x64/src/CliTools/ytec-disk-inventory.exe --json
```

終了コードは`0`が全項目取得、`2`が一部未取得を含む診断、`1`が列挙開始失敗、`64`が引数誤りです。管理者権限がない場合も権限昇格や書き込みを試みません。

WinPE環境も読み取り専用で診断できます。

```powershell
./out/build/msvc-x64/src/MediaBuilder/ytec-winpe-environment.exe --text
./out/build/msvc-x64/src/MediaBuilder/ytec-winpe-environment.exe --json
```

製品WinPEAppの検証用バイナリはVCランタイムDLLを必要としない静的構成でビルドします。

```powershell
cmake --preset msvc-x64-vm
cmake --build --preset msvc-x64-vm --target ytec-winpe-app
./out/build/msvc-x64-vm/src/WinPEApp/ytec-winpe-app.exe --help
./out/build/msvc-x64-vm/src/WinPEApp/ytec-winpe-app.exe --clone-preflight --source 0 --target 1 --text
./out/build/msvc-x64-vm/src/WinPEApp/ytec-winpe-app.exe --job-preflight --job-path E:\Tsumugi\job.json --text
./out/build/msvc-x64-vm/src/WinPEApp/ytec-winpe-app.exe --boot-repair-preflight --disk 0 --windows-root W:\ --system-root S:\ --firmware uefi --text
```

WinPEAppのプリフライトはディスク列挙と選択検査を行い、復元ジョブでは
ディスク列挙前に`.dcimg`全体、全チャンク、メタデータ、復元領域を
読み取り専用で再検証します。確認トークンや合格結果を出しても書込みへは
進みません。通常製品の`--clone-execute`は実行サービス未注入のため、
列挙開始前に終了コード`64`で拒否されます。VM専用ビルドだけが同じCLI境界へ、
VirtualBox、固定容量、固定許可語、管理者権限、二段階確認を再検証する
クローンサービスを注入します。通常製品の`--job-execute`は、ハッシュ検証済み
クローン/復元予約ジョブだけを受け付けます。クローンはコピー元/コピー先の
安定識別、空RAW、固定ディスク、対応GPT/MBR、容量/セクター、二段階確認を、
復元はジョブ/dcimg/復元先/必須安全条件/二段階確認を再検証した後だけ
種別ごとの製品サービスへ進みます。合成VDIだけを対象に、GPT/MBRクローン、
小容量dcimg復元、クローン/復元キャンセル、破損dcimg、改ざんジョブの
製品WinPE経路をVMで確認済みです。実機検証は全機能実装後まで行いません。

キャンセルと失敗経路の専用VM再検証は、VMラボが未使用で全VM停止中のときだけ
次のスクリプトから実行します。いずれも新規合成VDIだけを書込み対象にし、
既存媒体、物理ディスク、USBは対象にしません。

```powershell
./scripts/Invoke-ProductJobCancellationVmTest.ps1 -Profile Clone
./scripts/Invoke-ProductJobCancellationVmTest.ps1 -Profile Restore
./scripts/Invoke-ProductRestoreFailureVmTest.ps1 -Scenario CorruptImage
./scripts/Invoke-ProductRestoreFailureVmTest.ps1 -Scenario TamperedJob
```

WinPE引継ぎジョブは正規UTF-8 JSONとpayload SHA-256を使用します。
`--job-preflight`はジョブを読み取り専用で検証し、安定識別情報から現在の
ディスク番号を再解決します。復元ジョブではジョブ内の`.dcimg`または`.dcmig`を
共通検証器で完全再検証し、容量、論理セクター、元ディスクとの分離も確認します。
WindowsとWinPEでドライブ文字が変わった場合は`--image-path`で再選択でき、
ジョブv4（旧v2/v3読込み互換）に記録したイメージ長とSHA-256の一致後だけ進みます。
`--search-image`ではC:～Z:（WinPEのX:を除く）の固定/リムーバブル媒体から、
ジョブと同じ相対パスだけを最大23候補として調べます。再帰検索は行わず、
候補ごとの完全検証と指紋一致後だけ自動再解決します。`--job-preflight`は
常に読取り専用で、実行には別の`--job-execute`、消去同意、確認語`OK`が
必要です。
製品GUIはドライブ直下または`Tsumugi`直下の既定ジョブ名だけを有界検出し、
一意な正規候補では自動プリフライトまで進みます。複数/重複/改ざん候補は
自動選択せず、実行時も確認済みジョブSHA-256を再要求します。実行試行後は
時刻付き結果ログをジョブ横へ新規作成し、全バイト読戻し後だけ保存済みとします。
Windows版を再び起動して「ログ・診断」を更新すると、固定位置の結果ログを
読み取り専用で厳格検証し、最新の種別、成否、完了UTC、ジョブハッシュ先頭を
表示します。改ざん、追記、重複、名前と内容の不一致は結果として取り込みません。
Windows側の二段階確認画面には、既定OFFの「WinPEの安全確認に合格したら、
このジョブを一度だけ自動実行する」があります。選択したv3ジョブだけが対象で、
WinPEはジョブSHA-256を完全名と内容に持つ`.claim`をジョブ横へ`CREATE_NEW`保存し、
flush/全バイト読戻し後に実行します。既存記録や書込み不能媒体では対象を変更せず、
手動確認へ戻ります。ジョブ保存後は、Windows 8以降かつ管理者として手動起動済みの
場合だけ、既定「いいえ」でMicrosoftの詳細な起動オプションへの再起動を提案します。
標準権限では自動UACを行わず手順だけを表示し、他アプリ/別ユーザーを強制終了しません。
USBの最終選択は機種非依存に自動化できないため利用者が行います。
実行直前検査は合格／危険／不明を区別し、必須項目の不明を安全扱いしません。
検査合格時も
`executionEnabled=false`です。形式詳細は
[WinPEジョブ引き継ぎ形式](docs/job-handoff-v2.md)を参照してください。

単独起動修復は通常製品WinPEAppに接続されています。最初に
`--boot-repair-preflight`で表示された対象固有の確認語を確認し、同じ引数へ
`--boot-repair-execute --acknowledge-boot-files-change --confirmation <確認語>`
を追加した場合だけ実行します。実行直前にディスクとパーティションを再識別し、
クローン、初期化、パーティション移動は行わず、Microsoft署名済みBCDBootで
選択したシステム領域の起動ファイルとBCDだけを再構築します。システム領域は
割当済みルートの明示指定、または対象ディスク内で一意に確認できた未割当ESP/
Active領域の自動一時割当を選べます。自動モードは実行時だけ割り当て、解除成功まで
完了扱いにしません。Windows 10/11 x64だけを対象にします。

2026-07-30の承認後、ホストへADK `10.1.26100.2454`、対応WinPE Add-on、KB5101684をローカル導入しました。診断は固定構成、Microsoft署名、`/bootex`、Windows Installer製品バージョン、適用済みOscdimg/DISMパッチ、署名済みDISM `10.0.26100.8972`を読取り専用で確認し、不一致時はメディア作成を許可しません。Microsoft配布物はリポジトリへ同梱しません。

診断ゲート通過後は、`scripts/New-WinPEBootValidationMedia.ps1 -OutputRoot <リポジトリ外の新規フォルダー>`で2011 CA/2023 CAの起動検証用ISOをローカル生成できます。HDD/NICなし専用VMでLegacy BIOS、UEFI、Secure Boot有効2011 CA/2023 CAがWinPEプロンプトまで起動することを確認済みです。

続けて、2023 CA WinPE起動ISOと自作`ytec-winpe-app.exe`だけを収録した補助ISOを分離して専用VMへ接続しました。Secure Boot有効、NICなし、immutableの合成2GiB GPTディスクで、WinPEAppが`VBOX HARDDISK`、512バイト論理/物理セクター、4パーティションをテキスト/JSONの両方で列挙し、`issues: []`を返すことを確認済みです。これはWIM統合やクローン機能を含む製品媒体の完成を意味しません。

さらに、`scripts/New-WinPEAppValidationMedia.ps1`は既定で非昇格の事前診断だけを行い、`-BuildMedia`と管理者権限を明示した場合だけ、リポジトリ外の新規フォルダーへADK媒体を複製します。WIM追加前後のSHA-256、自作EXEのAMD64 PE32+形式と依存DLL、追加した3ファイル、生成ISOをmanifestへ記録します。2026-07-30 19:20生成の製品App媒体は、ISO SHA-256 `1A25DA7D226FA8EC7128C0115FFF59ABE6DB9BA814BB87F8BDC53B69401325E8`で、専用`YDC-WinPE-App-Integrated` VMのUEFI64/Secure Boot有効/NICなし構成から自動起動し、合成GPTディスクを読み取り専用列挙しました。VM専用サービスを注入した同じCLI境界では2GiB→3GiBと96GiB→110GiBの実WinPEクローンを完走し、後者は署名検証済みBCDBoot後、コピー元不在かつSecure Boot有効でWindows 10が起動しました。この既存ISOは今回追加した単独起動修復CLIをまだ収録していません。利用者方針により、実機試験とUSB用コピーは一通りの機能がVMで揃うまで延期します。

Phase 4では新規Windows 10 Pro x64 Legacy BIOS/MBR VMを固定56GiB、NICなしで作成し、VM限定WinPEからMicrosoft標準MBR2GPTのvalidate/convert、GPT/ESP再列挙を完走しました。Legacy BIOS WinPEの`mountvol /S`失敗を安全停止として保存後、ESPを固定disk 0 / partition 2へ限定する再開媒体で署名検証済みBCDBootとドライブ文字解除を完走しました。ISOと全補助媒体を外し、対象VDI 1台だけでUEFI64、続けてNVRAMの`PK`/`KEK`/`db`/`SecureBootEnable`を持つSecure Boot有効構成からWindows 10 `10.0.19045`が起動しました。詳細は[Phase 4進捗](docs/phase4-progress.md)に記録しています。

Phase 5ではWindows 10 x64専用VMでWindows SDK標準VSS COMバックエンドを実行し、Snapshot内照合、raw boot sector、容量/論理セクター、使用区間、Writer監査、`BackupComplete`、Shadow Copy残留0件を確認しました。製品経路の容量不足、コピー中止、正常完了に加え、2026-08-03にはZstandard `.dcimg`を別の合成ディスクへ復元し、元媒体とISOを外した復元先だけでSecure Boot起動しました。確定証跡は`.validation/evidence/product-vss-restore-vm/20260803-033054/`です。

設計と制約は[アーキテクチャ](docs/architecture.md)、[安全モデル](docs/safety-model.md)、[テスト計画](docs/test-plan.md)、[実装トレーサビリティ](docs/requirements-traceability.md)、[縮小移行モード](docs/shrink-migration-mode.md)、[Windows版GUI進捗](docs/windows-app-progress.md)、[WinPEネイティブGUI進捗](docs/winpe-gui-progress.md)、[WinPEジョブ引き継ぎ形式](docs/job-handoff-v2.md)、[Phase 1進捗](docs/phase1-progress.md)、[Phase 2進捗](docs/phase2-progress.md)、[`.dcimg` v1形式基盤](docs/image-format-v1.md)、[Phase 3進捗](docs/phase3-progress.md)、[Phase 4進捗](docs/phase4-progress.md)、[Phase 5進捗](docs/phase5-progress.md)、[WinPE環境検出と導入ゲート](docs/winpe-environment.md)を参照してください。
