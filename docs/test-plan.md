# Phase 1および後続基盤テスト計画

## 2026-08-03 実機前最終回帰

- BCD最終回帰時点の全CI: 通常/静的CRT/ASanのCTest各40/40、MSVC
  `/analyze`、ライセンス、SBOM、安全境界、媒体境界、ポータブル配布境界がPASS
- 縮小移行追加後の現行差分は、通常/静的CRT/ASanのCTest各43/43、MSVC
  `/analyze`、ライセンス、SBOM、安全/媒体/ポータブル配布境界がPASS
- GPT製品クローン: `.validation/evidence/product-gpt-clone-boot-vm/20260803-011915`
- MBR製品クローン: `.validation/evidence/product-mbr-clone-boot-vm/20260803-023613`
- VSS/Zstandard作成物の別ディスク復元: `.validation/evidence/product-vss-restore-vm/20260803-033054`
- MBR→GPT製品回帰: `.validation/evidence/product-mbr2gpt-vm/20260803-062217`
- 最新2011/2023 CA ISO起動6条件: `.validation/evidence/winpe-product-boot-matrix/20260803-064210`
- 全VM試験でNICなし、合成VDI限定、物理ディスク/USB不使用、保護コピー元SHA-256
  前後一致、試験後のVM設定復元と稼働中VM 0台を確認
- 6画面を目視し、日本語GUI、文字切れ/重なりなし、起動エラーなしを確認

詳細な媒体ハッシュと判定根拠は
[`validation-summary-20260803.md`](validation-summary-20260803.md)を参照します。

実機初回試行ではWindows 11/NVMeが報告する物理16KiBセクターを旧VSS境界が
書込み前に拒否しました。形式、マニフェスト、VSS計画/コピー、製品入口の条件を
2の累乗・論理セクター整数倍・64KiB以下へ統一し、16KiB成功4経路と不正値拒否、
全CIを再実行済みです。実機2回目はこの条件を通過後、別のCOM利用後に
`CoInitializeSecurity`を呼んだため`RPC_E_TOO_LATE`で書込み前に停止しました。
VSS用COMセキュリティをアプリ起動直後へ移し、後続COM利用とVSS側再確認の
回帰テスト、全CIを再実行済みです。実機全量バックアップの再試行は未実施です。

縮小移行の製品VM試験は、4 GiB合成データ専用原本から2 GiB RAWコピー先へ
オンラインVSS `.dcmig`作成と縮小復元を完走しました。原本不変、復元ファイル一致、
Snapshot残留0件、データ専用時の起動処理なし、自動マウント/Shell状態復元を確認し、
証跡を`.validation/evidence/product-data-shrink-vm/20260803-224747/`へ保存しています。
固定VMの`stopping`固着はVM名・固定UUID・実行ファイル・全VirtualBoxVMプロセスを
照合した安全回復で終了し、SATA 4/5切り離し、SATA 0～3不変、NICなしを確認しました。
最終全CIも通常/静的CRT/ASan各43/43、MSVC静的解析、全境界を581.6秒でPASSしました。

## ホスト上の自動テスト

- Phase 0のJSON/テキスト、シリアルマスク、RAII、エラー、モックCLI回帰
- CRC32既知ベクトル
- GPT主/副ヘッダー、配列CRC、LBA/範囲、GUID、パーティション重複検証
- FAT32/NTFSブートセクター解析とBitLocker明示拒否
- ボリュームビットマップの開始LCN丸め、使用範囲復号、結合
- 合成GPTディスクの拡大先クローン、GUID再生成、副GPT移動
- EFI/回復の全領域、基本NTFSの使用範囲、MSR再作成
- 確認不一致、識別変化、破損GPT、不明種別、読戻し不一致での停止
- BCDBoot固定引数、Windowsコマンドライン引用、絶対パス/署名ゲート、非ゼロ終了
- 単独起動修復のUEFI GPT/ESP/NTFS物理対応、BIOS MBR/一意Active、異ディスクESP、対象差替え、二段階確認を実ディスクなしで確認
- Windows標準のカタログ署名付き`System32\bcdboot.exe`と`mbr2gpt.exe`を許可し、署名のないテスト実行ファイルを拒否するWindows統合試験
- MBR→GPT別ターゲットの配置とGPTメタデータを相互検証し、回復領域の
  Windows直後への再配置/欠損時新設、GUID/型/属性/LBA/commit順の改ざん拒否、
  最初の変更前の6読取り専用ゲート、コピー元書込み禁止、最終UEFI起動要求を
  合成試験で確認
- 完全/欠落ADK、`/bootex`なし、reparse、署名不信頼、x86指定をモックし、メディア作成が安全側に停止すること
- ホストの固定ADK候補を読み取り専用で照会し、未導入/不完全時にエラー診断だけを返すWindows統合試験
- Windows GUI用ADK事前診断表示が、基本構成、`/bootex`、版/更新ゲートを
  区別し、成功時に未使用候補の欠落診断を混在させないことをモックで確認
- レスキューメディア計画がADK作成許可を最初のゲートにし、2023 CAでは
  `/bootex`を必須とし、相対/UNC/走査/非`.iso`パスを拒否することを確認
- USB作成候補が非システム、USB Bus、取り外し可能、オンライン、
  非読み取り専用、既知の基本GPT/MBR、単一区画、安定識別をすべて要求し、
  GPT USBもMBR/FAT32自動初期化の全区画削除と対象固有確認語を要約することを実ディスクなしで確認
- 物理I/O前の再列挙、列挙問題、ディスク番号変更、対象差替え、
  システムディスク化、offline/read-only変更、二段階確認語をモックで確認
- USBの物理ディスク番号とドライブ文字を単一Extent/区画範囲で一意照合し、
  複数ディスク、複数文字、容量外、区画不一致、非USB、システム、
  read-only/offline、安定識別不足を実ディスクなしで拒否する
- MediaBuilderのUSB書込みを、再識別済みUSBオブジェクト1件への`Clear-Disk`、
  RAW/区画0件の読戻し、MBR初期化、最大30 GiB単一FAT32作成、作成後MBR確認の
  各1箇所へ静的に制限する
- WinPEAppのヘルプが製品実行名と読み取り専用境界を示し、列挙を開始しないことをモックで確認
- WinPEAppのプリフライト成功、JSON、非RAWコピー先、列挙診断、通常製品`--clone-execute`の列挙前拒否をモックで確認
- ジョブJSONの正規形、SHA-256、UTF-8、64KiB上限、未知/末尾データ、
  対象差替えをモックで確認
- ジョブv4の`transferMode=exact/shrink`とv3/v4の
  `review-required`/`auto-once`がハッシュ対象として往復し、旧v2は常に
  手動確認へ、旧v2/v3は常に通常モードへ移行することを確認
- ジョブ通常ファイル保存の新規作成限定、既存ファイル非上書き、全バイト読戻し、
  ハッシュ再検証、不正ジョブ時の出力なしを確認
- `auto-once`開始記録がジョブSHA-256へ結び付き、最初の`CREATE_NEW`だけ成功し、
  同一ジョブ2回目/相対パス/保存不能では対象サービスへ進まないことを確認
- Windows詳細起動オプションへの再起動計画がWindows 8以降/管理者手動起動だけを
  許可し、標準権限とWindows 7ではバックエンドを呼ばず手動手順へ戻ることをモック確認
- Windows GUIの対象情報確認、誤入力時無効、完全一致時有効、保存、
  WinPE読取り専用プリフライトまでを標準権限で確認
- 注入サービスがあっても確認トークン不一致では到達せず、成功時だけ安定識別と承認情報を受け取ることをモックで確認
- WinPEAppの単独起動修復プリフライトが読取り専用JSONと対象固有確認語を返し、確認不一致、サービス未注入、実行成功レポートをモックで確認
- WinPE GUIの画面モデルが正常な固定非システムディスクだけを候補にし、
  空一覧、列挙診断、不明なread-only属性ではジョブ/起動修復確認を
  無効化することを実ディスクなしで確認
- LINE Seed JP Regular/Boldの埋込み資源をプロセス内へ読み込み、GDIが
  `LINE Seed JP App_TTF Regular/Bold`を実際に選択したことをヘッドレスで確認
- Windows GUIのクローン/レスキュー用コンボ、保存先欄、参照ボタンが
  966/1024/1266/1280/1600pxの各クライアント幅でカード内に収まり、
  相互に重ならず、右余白を保持することを純粋レイアウトテストで確認
- WinPE環境診断JSONをCTestで解析し、メディア生成スクリプトの出力先拒否・事前診断非生成・構文境界を確認
- WinPE媒体事前診断が静的CLI/GUIそれぞれのAMD64 PE32+、
  SHA-256、固定System DLLと、ローカルADK日本語フォントCABの
  repositoryCopy=false/SHA-256を記録し、出力を作らないことを確認
- Windows CNG SHA-256既知ベクトル、`.dcimg`Zstandard/非圧縮/ゼロチャンク往復、マニフェスト/展開後チャンク/全体ハッシュを確認
- Zstandardの単回コピー元読取り、非圧縮fallback、圧縮破損、辞書、連結/末尾フレーム、内容サイズ不明/不一致、読戻し破損時abortを確認
- `.dcimg`の切詰め、未知版、長さオーバーフロー、論理/保存範囲重複、ハッシュ表不一致、1ビット破損を拒否
- 有界Readerからヘッダー/索引/フッター/マニフェスト/パーティション表/
  ハッシュ表を読み、最大指定ブロックで全体・全チャンクSHA-256を検証し、
  検証中のヘッダー差替えを拒否することを確認
- Windows復元プリフライトが現行`.dcimg`の完全検証と意味照合を通しても
  `restore_execution_enabled=false`を維持し、破損、寸法矛盾、
  読取り前キャンセル、非`.dcimg`拡張子を安全側に拒否することを確認
- Windows実ファイルBackendで新規合成`.dcimg`を`GENERIC_READ`だけで開いて
  完全検証し、復元先ディスクI/Oを要求しないことを確認
- 復元先候補評価が列挙済み情報だけを使い、正常候補でも実行を有効化せず、
  元ディスク、システム、容量不足、論理セクター不一致、不明状態、
  リムーバブル、列挙診断ありを安全側に拒否することをモックで確認
- 復元予約ジョブが確認語`OK`の完全一致時だけ生成され、システムディスクと
  誤確認語を拒否し、正規JSON＋SHA-256で復元種別とイメージパスを保持する
- WinPE復元ジョブが`.dcimg`全体をディスク列挙前に再検証し、破損または
  検証サービス不在時は列挙へ進まず、容量不足、論理セクター不一致、
  イメージ元ディスクと復元先の同一性を安全側に拒否し、正常時も
  `executionEnabled=false`を維持することをモックで確認
- WinPEでドライブ文字が変わった想定の`--image-path`再選択が、ジョブv4に
  記録したイメージ長とSHA-256の一致時だけ成功し、別イメージは
  ディスク列挙前に拒否されることをモックで確認
- `--search-image`がドライブ文字だけを置換して同じ相対パスを保持し、
  WinPEのX:を除外し、別指紋の候補を読み飛ばして一致候補だけを採用し、
  一致なし/64候補超過ではディスク列挙前に停止することをモックで確認
- WinPE復元の実行直前安全判定が、未接続項目を`unknown`のまま必須合格に
  せず、MBR LDM型`0x42`、Storage Spaces BusType、未知BusTypeを
  危険／危険／不明として扱うことをモックで確認
- BitLocker、基本ディスク、Storage Spaces除外、ファイルシステム、AC電源の
  必須合格と、再起動保留の警告`unknown`を分離し、全必須合格時も
  `executionEnabled=false`を維持することをモックで確認
- WinPE製品`--job-execute`がクローン/復元予約ジョブだけを受理し、
  実行サービス不在、必須安全条件不合格、消去承認なし、確認語不一致では
  物理対象へ到達せず、正常時だけ種別別実行結果を報告することをモックで確認
- 縮小レイアウトがWindows/データ専用、GPT/MBRの各基本NTFS構成で、使用量、
  12.5%/固定安全余白、ESP/MSR/回復領域、1 MiB整列、残り容量配分を計算し、
  容量不足、BitLocker、非NTFS、形式変換、非AMD64 Windowsを拒否することを確認
- `.dcmig`の正規マニフェスト往復、未知版、重複番号、不正ラベル、WIM名・
  長さ・SHA-256改ざん、宣言外項目、reparse、既存出力非上書きを確認
- Windows/WinPEの通常/縮小選択、縮小時だけ小容量RAW候補を許すプリフライト、
  データ専用では起動役割とBCDBootを要求しないことをモックで確認
- 直接縮小クローンがコピー元/コピー先と別の第三物理ディスクへ検証済み
  `.dcmig`を確定してからだけコピー先へ進み、作業束の差替えを拒否することを確認
- WinPE GUI用のジョブ自動検出が固定名/固定位置だけを最大92候補で扱い、
  X:、候補なし、重複、複数、改ざんを安全側に処理することをモックで確認
- 事前確認後にジョブを別の正規ジョブへ差し替えた場合、承認済みpayload
  SHA-256との不一致でディスク列挙前に停止することを確認
- 実行結果ログがジョブハッシュ/詳細SHA-256へ相関し、64 KiB上限、時刻付き
  `CREATE_NEW`、flush、全バイト読戻し、既存非上書きを守ることを確認
- Windows結果取込みがC:～Z:（X:を除く）の固定/リムーバブル媒体にある
  ドライブ直下/`Tsumugi`直下だけを扱い、厳格検証済み完了UTCの新しい順に
  表示することをモックで確認
- 結果ログの改ざん、追記、重複、非正規名、ファイル名と本文のジョブ種別/
  完了UTC不一致では、検証済み結果の一部だけを表示せず停止することを確認
- 製品クローンの読取り専用事前判定が、GPT/MBR基本ディスクを許可し、
  BusType不明、Storage Spaces/LDM、システム/removable/offlineコピー元、
  空RAWと既知の基本GPT/MBRコピー先を許可し、不明・動的・Storage Spaces、容量不足、セクター不一致を拒否することを確認
- 製品クローンジョブサービスへFreshなコピー元/コピー先安定識別、
  空のVM許可語、二段階確認、進捗Callbackだけが渡り、読戻し/区画確定/
  online復帰の欠けたレポートをPASSにしないことをモックで確認
- 製品復元先境界が、開始直前再列挙、安定識別、システム/removable/
  読取り専用/offline/未知状態、512バイト論理セクター、offline化後の
  再列挙、物理ハンドル寸法をすべて検証することを実ディスクなしで確認
- 同じReaderでdcimg全体/全チャンク/ジョブ指紋を完全検証した
  単回使用の準備済み復元元だけを物理復元へ渡し、完全検証を二重走査せず、
  非ゼロチャンクだけを書込み直前に再読込みすることを確認
- ジョブ記録済みdcimg長さ/全体SHA-256不一致では対象書込みが0件であること、
  準備済み復元元を再利用できないことを確認
- 正規バックアップマニフェストの決定的エンコード、UTF-8/予約領域/位置、
  Windows x64、GPT/MBRと起動方式、BitLocker完全復号、全区画意味を確認
- 実ディスクを使わず、MBR先頭セクターとGPT主副メタデータだけを
  読み取り専用キャプチャし、区画内容を読まないことを確認
- 抽象`.dcimg`ストリームが既存メモリ作成結果とバイト一致し、全索引/チャンク/全体ハッシュ再読込み後だけcommitすることを確認
- コピー元失敗、再読込み破損、begin/commit/abort失敗、短いSHA読取りで完成扱いにせず、開始後はabortを試みることを確認
- Windows保存先の同一コピー元/コピー先、容量不足、reparse、既存完成/未完了ファイルを作成前に拒否することをモックで確認
- Windows保存先の確定前識別差替え、所有前/所有後作成失敗、非上書き確定失敗、未完了削除失敗を安全側に処理することをモックで確認
- MBR/GPTパーティション表スナップショットの正規往復、領域ハッシュ破損、配置不正を拒否
- 合成`.dcimg`復元で、全体検証後にパーティション表無効化、データ読戻し、パーティション表最終確定の順序を確認
- 破損イメージ、確認不一致、容量不足、データ/パーティション表重複、読戻し不一致では確定書込みへ進まないことを確認
- MBR署名、予約領域、Active、32bit LBA、区画重複、保護/拡張型、512バイト制限を合成ディスクで確認
- MBRコピー先署名が0、コピー元、接続中署名を避け、有界衝突時に停止することをモックで確認
- 合成MBRクローンでNTFS使用クラスタ/回復NTFS全領域だけをコピーし、全読戻し後にMBRを最後に確定することを確認
- MBRクローンの確認語不一致、NTFS先頭クラスタ欠落、読戻し不一致、BitLocker形式で安全停止することを確認
- BCDBootの`/f BIOS`が固定引数で、Active Windows区画とシステム区画が同一の構成だけを明示的に許容することを確認
- MBR2GPTの固定引数、未知動作、署名ゲート、validate失敗、validate後の対象差替え/番号変更、convert失敗をモックで確認
- MBR2GPT前のオフラインWindowsカーネル検査でAMD64 PE32+だけを許可し、x86/ARM64/不一致/切詰めPEを拒否
- WinRE診断の署名済みREAgentC固定`/info /target`引数、ASCII/UTF-16LE登録先解析、対象ディスク照合、登録先/Windows内フォールバックの読取り専用イメージ確認、複数登録/古い登録/欠損/非ゼロ終了/署名失敗の安全停止、確認済み事実だけの再構築入力をモックで確認
- WinPE CLIのWinRE診断で固定引数解析、ディスク再列挙/安定識別、不正パスのアクセス前拒否、登録済みJSON、REAgentC不明時の診断出力＋失敗終了、サービス未接続時の列挙前拒否をモックで確認
- VSSの管理者権限、Volume GUID/NTFS/重複、固定手順、Writer異常、Snapshot対応不整合、全失敗段階の削除試行をモックで確認
- VSS非同期操作のPending→完了、明示キャンセル、有限timeout、状態照会失敗、操作HRESULT失敗、無限待機設定拒否と`Cancel`試行をモックで確認
- Windows VSS具体バックエンドがSnapshot専用コピーCallback未設定時にCOM初期化前で停止することを確認
- Snapshot ReaderがライブVolume GUID、不正容量/セクターをopen backend到達前に拒否し、検証済みSnapshot Identityだけを渡すことをモックで確認
- Snapshot専用Bitmap Providerと通常Volume GUID Providerが相互のパス種別と重複Bindingを拒否することを実デバイスなしで確認
- Snapshot Bitmap使用範囲を16/32MiB以下へ分割して物理ディスク論理位置へ変換し、抽象`.dcimg`ステージングへ渡すことをモックで確認
- Snapshot Bitmap範囲外、Reader Geometry不一致、Bitmap照会失敗、Snapshot消失、重複パーティションBindingを開始前または未完了abortで拒否することを確認
- EFI/回復raw＋Windows VSS＋MSR再作成のGPT計画、MBR計画、
  表/マニフェスト/ブートセクター不一致、BitLocker署名をモックで確認
- `.dcimg`全検証後もVSS完了まで確定を遅延し、`BackupComplete`または
  Snapshot削除失敗時に未完了出力をabortすることを確認
- Windows製品ジョブが標準権限、非システムディスク、再識別差替えを
  物理ディスク/VSS到達前に拒否し、正常時だけ製品Executorへ進むことを確認

## ビルド・解析ゲート

- MSVC x64/C++20、`/W4 /WX /permissive- /sdl /guard:cf`
- CTest
- MSVC`/analyze`
- MSVC AddressSanitizer
- ライセンス台帳、SBOM一致、物理書込みAPI/禁止成果物検査
- Windows PowerShell 5.1互換のため、全`.ps1`がUTF-8 BOM付きであること

## WinPE GUIの実画面・媒体試験

- ホスト標準権限の1280x720で予約ジョブ、起動修復、ディスク診断を操作し、
  文字切れ、重なり、無効ボタン、1行一覧、Tab到達を確認
- GUI、日本語フォントCAB、CLIを統合したISOをUAC一括作業で生成し、
  WIM内ハッシュとmanifestを照合
- WinPE内1280x720で日本語グリフ、GUI自動起動、ファイル選択、
  読み取り専用列挙を確認
- Legacy BIOS、UEFI、Secure Boot 2011 CA/2023 CAの各媒体で
  GUI起動まで確認
- 起動修復の実行確認は専用VMの合成媒体だけで行い、ホスト実ディスクは
  最終実機試験まで使用しない
- 復元の実行確認は専用の合成コピー元dcimgと専用復元先VDIだけを使い、
  全体検証前の対象offline化なし、成功時online復帰、失敗/中止時offline保護、
  パーティション/データ読戻しを確認
- クローンの実行確認は専用GPT/MBRコピー元VDIと新規RAWコピー先VDIだけを使い、
  コピー元が同じ読取り専用ハンドルで保持されること、MBR署名衝突回避、
  全書込み読戻し、成功時online復帰、失敗/中止時offline保護を確認
- 縮小移行の実行確認は4 GiBの合成GPT/NTFSデータ専用原本、2 GiBの新規RAW
  コピー先、既存の第三ディスクだけを使い、VSS作成、全WIM/manifest検証、
  コピー元安定識別とsentinel不変、縮小復元、復元sentinel一致、BCDBoot未実行、
  Snapshot残留0件、自動マウント/Shellサービス復元を確認
- 検証中、書込み前、データ復元中、最終パーティション表確定の各工程で
  キャンセル可否とGUI応答を確認し、確定開始後は中断できないことを確認

## VMラボ方針

共有VMラボを1タスクずつ使用し、NIC無効、合成データのみ、検証済みスナップショットを保持します。まずWin10 x64 UEFIとWin11 x64 UEFI/Secure Boot有効環境で、静的ランタイム版の全テスト実行を確認します。

次の段階では専用の追加仮想ディスクだけを使い、次を確認します。

1. GPT/UEFI Windowsをオフラインクローンする。
2. BCDBootをコピー先WindowsとESPに対して実行する。
3. コピー元仮想ディスクをVMから外した状態でコピー先だけを起動する。
4. Windows起動、EFI、回復環境、ファイル整合性、GUID分離を確認する。
5. 中断、容量不足、対象差替え、破損GPT、BitLocker、不明構成で安全停止する。

Phase 3では、既存48GiB Windows 10 MBR媒体を直接変更せず独立複製を作り、新規56GiBターゲットを組み合わせます。Legacy BIOSのMBR→MBR/BCDBoot/コピー元不在起動を先に確定します。Phase 4は製品対象のx64要件を守るため、Phase 3のx86成功媒体を流用せず、新規のWindows 10 x64/Legacy BIOS/MBR/56GiB専用VMでMBR2GPT/UEFI/Secure Bootを試験します。通常製品とは分離した固定プロファイルと許可語を使い、WinPE WIM統合に必要なUACだけを人間が手動承認します。

2026-07-30に48GiB媒体の独立単体VDI、新規56GiB RAW媒体、Legacy BIOS/NICなし専用VMを作成しました。読み取り専用製品WinPEで固定構成を確認後、実WinPEでMBRクローンと署名済みBCDBootを完走しました。コピー元とISOを外した56GiBコピー先だけでWindows 10 10.0.19045がLegacy BIOS起動し、NIC 0を確認しています。確定証跡は`.validation/evidence/phase3-legacy-bios-vm/20260730-2010-retry/`です。起動メニューの重複項目は製品統合前の修正課題です。

Phase 4では別の新規Windows 10 Pro x64 VMをLegacy BIOS/MBR、固定56GiB、NICなしで作成しました。初回VM専用WinPEでMicrosoft標準MBR2GPTのvalidate/convertとGPT/ESP再列挙を完走し、Legacy BIOS WinPEの`mountvol /S`失敗は成功扱いせず保存しました。修正版は使用中`S:`を拒否し、固定disk 0 / partition 2だけをDiskPartで割り当て、BCDBootハーネスが同じVirtualBox媒体、物理offset、FAT32/NTFS、GPT種別を再検証します。再開ISOで署名検証済みBCDBootとドライブ文字解除を完走し、対象VDI 1台、全ISOなし、NICなしでWindows 10 `10.0.19045`がUEFI64とSecure Boot有効の両方で起動しました。確定証跡は`.validation/evidence/phase4-mbr2gpt-vm/20260730-211544/`、`.validation/evidence/phase4-uefi-boot/20260730-213451/`、`.validation/evidence/phase4-secureboot/20260730-214614/`です。修正済み一体型媒体の先頭からの回帰試験は未実施です。

単独起動修復は既存の起動可能VDIを直接変更せず、UEFI/Secure Boot用と
Legacy BIOS用の独立フルクローン2台で試験します。2026-07-31に
`YDC-Standalone-BootRepair-UEFI`へ製品WinPEApp統合ISOを接続し、読み取り専用
プリフライト後に`EFI\Microsoft\Boot\BCD`を退避しました。ISOを外した起動は
`0xc0000098`で失敗し、同じ製品CLIへ対象固有確認語を渡した修復では
`result=PASS`、`microsoftSignatureVerified=true`、
`bootStoreVerified=true`、終了コード0を確認しました。ISOを再び外し、
EFI64、Secure Boot有効、NICなしのままWindows 10とGuest Additions 7.1.4が
起動しました。証跡は`.validation/evidence/standalone-boot-repair-vm/20260731-uefi/`
と`.validation/evidence/standalone-uefi-*.png`です。次は同じ順序を
`YDC-Standalone-BootRepair-BIOS`で確認します。元VDI、既存スナップショット、
資格情報は変更しません。

Phase 5のWindows VSS具体バックエンドはWindows SDK標準APIだけでビルドし、非同期制御とWorkflow連携をモックと専用VMの両方で検証しています。2026-07-30に新規リンククローン`YDC-Phase5-VSS-x64`をEFI64/NICなしで用意し、固定許可語、管理者、VirtualBox、固定C: NTFS、固定合成Sentinelの全ゲート通過後にライブVSSを実行しました。Snapshot内Sentinel、raw boot sector、容量/論理セクター、Snapshot専用Bitmap 2,889区間、Writer 10件、`BackupComplete`を同一Workflowで確認し、開始前と終了後のShadow Copyはいずれも0件です。`VSS_WS_WAITING_FOR_BACKUP_COMPLETE`はSnapshot直後だけ正常状態として許可し、BackupComplete後はStableを要求します。SnapshotデバイスがStorageAccessAlignmentPropertyを未サポートと明示する場合だけ、`GetDiskFreeSpaceW`で論理セクターを再確認します。確定証跡は`.validation/evidence/phase5-vss-vm/20260730-231804/`です。2026-07-31には同じNICなしVMへ固定128MiB RAW識別ディスクと512MiB NTFS保存先VDIを接続し、合成ReaderからWindows具体ファイルBackendへ1,049,741バイトの`.dcimg`を作成しました。全件再読込み、全ハッシュ、非上書き確定、`.partial`残留なし、ゲスト/ホストSHA-256一致を確認し、証跡を`.validation/evidence/phase5-file-staging-vm/20260731-004413/`へ保存しています。次の安全ゲートはライブVSS Callbackと実ファイルBackendのVM統合です。

2026-08-03のデータ専用縮小移行試験では、同じNICなしVMへ新規4 GiB RAW原本と
2 GiB RAWコピー先だけを追加し、オンラインVSS `.dcmig`作成とSnapshot削除までは
合格しました。初期試行で一時Volume割当のWindows APIエラー87、次にFORMAT引数を
全項目引用したことによる終了コード4を検出し、Volume GUIDからNTデバイスを
再確認する一時DOSデバイス割当と、空白を含まない引数を無引用にする修正を
単体回帰へ追加しました。修正後はフルWindows Explorerが新規未フォーマット領域を
検出して確認画面を出し、製品ハーネスが待機しました。これはWinPEにはない
試験環境干渉なので、VMランナーが原本fixture作成後から試験終了までだけ
`ShellHWDetection`と自動マウントを停止し、成功/失敗の両方で元へ戻すように
しました。17:07のUAC付き試行は`NoAutoMount`値欠落の扱いで製品ハーネス前に
FAILとなり、欠落値をWindows既定の`0`として扱う修正を追加しました。失敗証跡は
`.validation/evidence/product-data-shrink-vm/20260803-170747/`、Explorer干渉の
画面証跡は`.validation/evidence/product-data-shrink-vm/20260803-125409/`です。VMの既存
SATA 0～3、検証済みスナップショット、NICは変更していません。その後、FORMATへ
渡せるMount Managerドライブ文字の登録・再照合・解除を実装し、22:47試行でPASS。
確定証跡は`.validation/evidence/product-data-shrink-vm/20260803-224747/`です。失敗証跡も
履歴として保持し、合格へ書き換えていません。

物理ターゲットライターと専用ハーネスは追加済みです。自動UAC承認、UAC無効化、非公式昇格ツールは採用せず、VM画面内の専用ランチャーとUACだけをユーザーが手動承認します。2026-07-29の2GiB→3GiB専用VM試験では、物理I/O、読戻し、GUID再生成、4区画維持、ファイルハッシュ照合が成功しました。確定証跡は`.validation/evidence/phase1-physical-vm/20260729-232003/`です。

起動試験用には、小容量試験と異なる固定許可語を使う96GiB GPT→110GiB RAWプロファイル、コピー先Windows/ESPの物理ディスク・開始位置・NTFS/FAT32・GPT種別を再確認するBCDBoot専用ハーネス、署名検証済みMicrosoft標準BCDBoot、コピー元を接続しないBoot-Check VMを追加しました。最初の128GiB Win11候補はBitLocker形式だったため、コピー計画中かつ最初のターゲット書込み前に仕様どおり停止しました。証跡は`.validation/evidence/phase1-boot-vm/20260729-235926/`です。BitLocker解除や回避は行いません。

2026-07-30の最終試験では、Win11 Worker上で96GiB Windows 10 GPTコピー元から110GiB RAWコピー先へ13,218,922,496バイトをコピーし、3区画コピー、MSR 1区画再作成、全書込み読戻し、主GPT最終確定、新しいDisk GUID/Partition GUID、Windows/EFIファイルハッシュ一致を確認しました。クローン証跡は`.validation/evidence/phase1-boot-vm/20260730-042404/`です。続けてWindowsシステムカタログで署名された`System32\bcdboot.exe`を検証・実行し、コピー元を接続せずコピー先だけを接続したUEFI64/NICなしのBoot-Check VMでWindows 10 10.0.19045の起動、Guest Additions応答、`C:\Windows\System32\winload.efi`の存在を確認しました。最終結果はPASS、証跡は`.validation/evidence/phase1-boot-vm/20260730-051853/`です。

この最初のBoot-Checkの画面キャプチャはヘッドレス/ロック状態のため黒画面で、視覚証跡には使用していません。起動判定は、コピー元不在・コピー先のみの媒体構成、Guest AdditionsのOS情報、ゲストファイル照会を組み合わせています。後述の実WinPE再試験では別のSecure Boot有効Boot-Checkを実施しました。既存VM、スナップショット、資格情報、証跡、失敗媒体を削除・登録解除・改名しません。

## WinPE/起動方式の後続検証

2026-07-30の承認後、ADK `10.1.26100.2454`、WinPE Add-on、KB5101684をローカル導入しました。ホスト診断は固定構成、Microsoft署名、`/bootex`、製品バージョン、適用済みOscdimg/DISMパッチ、DISM `10.0.26100.8972`を確認し、終了コード`0`、`mediaCreationPermitted=true`です。モックではパッチ欠落、製品バージョン不一致、DISM版不一致がすべて作成拒否になることを確認しています。

`scripts/New-WinPEBootValidationMedia.ps1`は診断ゲート通過後だけ、リポジトリ外へ標準2011 CA版と2023 CA版の検証用ISOを生成します。後者はMicrosoftの`/bootex`経路と同じ`efisys_EX.bin`をEl Torito UEFIブートイメージに使用します。生成時のWIM/ISOのサイズとSHA-256はリポジトリ外のmanifestへ保存し、Microsoftファイルはリポジトリへコピーしません。

HDDなし、NICなしの専用VM `YDC-WinPE-Boot-Matrix`で次を確認しました。

| ファームウェア | Secure Boot | ISO | 結果 |
|---|---:|---|---|
| Legacy BIOS | 無効 | 2011 CA | WinPEプロンプトまで起動 |
| UEFI x64 | 無効 | 2011 CA | DVD起動キー入力後、WinPEプロンプトまで起動 |
| UEFI x64 | 有効 | 2011 CA | WinPEプロンプトまで起動 |
| UEFI x64 | 有効 | 2023 CA | WinPEプロンプトまで起動 |

画面証跡は`.validation/evidence/winpe-boot-matrix/20260730-1045/`です。VMは電源OFF、Secure Boot有効、NICなし、2023 CA ISOだけを接続した状態で保持しています。後続試験でWinPE内クローン、BCDBoot、コピー元不在のSecure Boot起動まで完了しました。Secure Boot無効化や署名回避は要求しません。

2026-07-31には配布版GUIのelevated製品経路で、ローカルADK媒体準備、
日本語フォント追加、静的CLI/GUI照合、WIM commit、2023 CA ISO生成まで
実測しました。ISO生成後のmanifest保存がWindows PowerShell 5.1非互換の
`utf8NoBOM`指定で停止したため、.NETのUTF-8 BOMなし書込みへ修正し、
互換回帰検査を追加しました。失敗時に保持された中間ISOは
429,881,344バイト、SHA-256
`4D64183F3BAF4A7D723F396798AB315B880B4445E4E546FDF6CB79AA345751CA`
です。HDD/NICなしの`YDC-WinPE-Boot-Matrix`へ一時接続し、
Legacy BIOSとUEFI64/Secure Boot有効の双方で日本語製品GUIの
自動起動を確認しました。証跡は
`.validation/evidence/winpe-boot-matrix/20260731-product-intermediate-keyed-135735/`
および
`.validation/evidence/winpe-boot-matrix/20260731-product-intermediate-bios-keyed-135953/`
です。試験後は電源OFF、NICなし、元ISO、UEFI64/Secure Boot有効へ
復元しました。これは中間ISOの内容検証であり、修正版の完成ISO発行成功を
代替しません。

2026-08-01には全最新差分を収録した完成候補を2011 CA版と2023 CA版の
両方で新規生成し、manifestと現在の製品CLI/GUI、WIM内UEFIブート
マネージャーのSHA-256、AMD64 EFI形式、Microsoft署名を照合しました。
2011 CA版は429,942,784バイト、SHA-256
`B0B82226323A7F55F82DC35C677184F68533A968B9F53E932BB60FD3A381F19A`、
2023 CA版は429,942,784バイト、SHA-256
`05302CF7B2F9B7C3E398C7A95B4D8C35D4500D6F3F742C8149F3E93163BD350E`
です。`YDC-WinPE-Boot-Matrix`はHDD 0、NICなしのまま、Legacy BIOSと
UEFI64、Secure Boot有効/無効、両CAの6条件すべてで同一の日本語製品GUIを
表示しました。試験後はpoweroff、元のUEFI64、Secure Boot有効、元ISOへ
復元し、稼働中VM 0台を確認しました。取得結果と目視確認の確定証跡は
`.validation/evidence/winpe-product-boot-matrix/20260801-102638/`です。

製品WinPEAppの最初の実行試験では、静的MSVC x64バイナリを自作ファイルだけの補助ISOへ収録し、基本WinPE起動ISOとは分離しました。`YDC-WinPE-App-Diagnostic`はUEFI64、Secure Boot有効、NICなしで、合成2GiB GPT媒体の複製をimmutableとして接続しています。WinPE内で`--text`と`--json`を実行し、ディスク0、`VBOX HARDDISK`、容量2GiB、SATA、512/512バイトセクター、GPT、ESP/MSR/基本データ/回復の4パーティション、`issues: []`を確認しました。証跡は`.validation/evidence/winpe-app-read-only/20260730-1252/`です。VMは電源OFFで保持し、既存の合成コピー元とホスト実ディスクには書き込んでいません。

次に、監査付きスクリプトで許可済みADKのWIM複製へWinPEApp、起動cmd、`winpeshl.ini`だけを追加しました。追加前後のWIMハッシュが変化し、WIM内EXEのハッシュが元の静的バイナリと一致することを確認し、2023 CA ISOとmanifestをリポジトリ外へ生成しました。ISOは397,684,736バイト、SHA-256 `1E2BB7C2ED8AF913D376AD65D329AE7E80B224FF9F17FCA0B070A2CB9BED0549`です。`YDC-WinPE-App-Integrated`をUEFI64/Secure Boot有効/NICなしで起動するとWinPEAppが自動実行され、同じ合成2GiB GPTディスクをテキスト/JSONで列挙して`issues: []`を返しました。証跡は`.validation/evidence/winpe-app-integrated/20260730-1310/`です。VMは電源OFF、稼働中VMは0台です。この媒体にクローン機能はありません。

VM限定ハーネスを自作補助ISOへ収録し、`YDC-WinPE-Clone-Execution`のUEFI64/Secure Boot有効/NICなしWinPEで2GiB GPTから新規3GiB RAWへ実行しました。668,758,016バイト、3区画をコピーし、MSR 1区画を再作成しました。全書込み読戻し、主GPT最終確定、新しいDisk GUID、`issues: []`を確認しています。証跡は`.validation/evidence/winpe-clone-execution/20260730-1321/`です。これはVM専用ハーネスであり製品Appの書込み入口ではありません。

続けて最新の製品WinPEAppを別の自作補助ISOから`YDC-WinPE-Product-Preflight`で実行し、2GiB GPTコピー元と新規3GiB RAWコピー先をテキスト/JSONで再識別しました。コピー先固有トークン、`executionEnabled=false`を確認し、実行後もコピー先はRAW、パーティション0件、`issues: []`でした。証跡は`.validation/evidence/winpe-product-preflight/20260730-1331/`です。

2026-07-31には、その後の差分を含む最新静的CLI/GUIだけを読み取り専用VISOで
接続し、既存の検証済み2023 CA WinPEから実行しました。
`YDC-WinPE-App-Integrated`はEFI64、Secure Boot有効、NICなしのまま、
最新版CLIが合成2GiB GPTを読み取り専用列挙してプロンプトへ戻り、
最新版GUIも起動しました。起動に使った既存WIMは日本語フォント統合前なので
画面上の日本語は□ですが、レイアウトと実行性の証跡としてのみ扱います。
続く`YDC-WinPE-Product-Preflight`では、前回クローン済みの3GiB GPTコピー先を
指定すると、最新版`--clone-preflight`が必要なRAW状態ではないとして
終了コード1で拒否しました。書込みは行っていません。証跡は
`.validation/evidence/winpe-latest-self-payload/20260731-194300/`です。

その後、製品と同じ`--clone-execute`引数解析・再列挙・プリフライト・確認境界へ、破壊的VMビルドだけが実行サービスを注入する構成へ変更しました。通常製品はサービス未注入のため列挙前に終了コード`64`で拒否します。`YDC-WinPE-Product-Preflight`の実WinPEでは2GiB GPT→3GiB RAWを実行し、668,758,016バイト、3区画コピー、MSR 1区画再作成、全読戻し、主GPT最終確定、コピー先online復帰、4区画と`issues: []`を確認しました。証跡は`.validation/evidence/winpe-product-execution/20260730-1352/`です。

2026-07-31には直接`--clone-execute`ではなく、通常製品と同じ
`--job-execute`と製品クローンサービスを最新静的EXEで実測しました。
破壊的VM構成だけの補助はVirtualBox、管理者、固定Disk 0、固定Disk 2、
2GiB GPT→新規3GiB RAW、固定許可語を確認し、RAM上へ正規ジョブを
新規保存するだけです。製品EXE側がジョブハッシュ、安定識別、二段階確認、
RAW状態を再検証してから実行し、668,758,016バイト、3区画コピー、
MSR 1区画再作成、全読戻し、主表確定、online復帰、終了コード0となりました。
完了後に同じ対象へ再度クローンプリフライトを行うと、GPT化済みでRAWではないため
終了コード1で拒否しました。試験後はVMを電源OFF、元の補助ISO、SATA 2ポート、
UART offへ戻し、新規VDIは取り外して証跡として保持しています。証跡は
`.validation/evidence/product-job-vm/20260731-200028/`です。

起動試験では実WinPEから固定96GiB Windows 10 GPT→110GiB RAWを実行し、12,950,999,040バイト、3区画コピー、MSR 1区画再作成、全読戻し、主GPT最終確定、コピー先online復帰を確認しました。続いてWinPE内のMicrosoft署名検証を必須とするBCDBootハーネスが終了コード`0`となり、BCDストアとEFIフォールバックファイルを確認しました。コピー元と全DVDを外し、コピー先だけを接続した`YDC-WinPE-Boot-Check-Secure`はEFI64、Secure Boot有効、NICなしでWindows 10 10.0.19045まで起動し、Guest Additions応答とログイン画面を確認しました。証跡は`.validation/evidence/winpe-boot-secure/20260730-1409/`です。

基本WinPEには日本語フォントを追加していないため、基本媒体や旧媒体上の
テキスト出力は画面上で□になります。製品媒体生成では、利用者のローカルADKに
ある`WinPE-FontSupport-JA-JP.cab`を生成WIMへだけ追加し、CAB自体を
リポジトリや配布ZIPへ同梱しません。中間製品ISOで日本語グリフは確認済みですが、
最新GUIを含む完成ISOでの回帰を最終UAC項目に残します。

Secure Boot有効Windows 11 VM内でアプリの合成テストが動くこと、WinPEメディア自体がSecure Boot起動すること、WinPE内で製品境界が動くこと、クローン後のWindowsがSecure Boot起動することは別の検証として扱います。今回までに4項目を個別に確認しました。実機のファームウェアや2023 CA移行状態まで保証するものではなく、実機確認は全機能が揃った最後に行います。
