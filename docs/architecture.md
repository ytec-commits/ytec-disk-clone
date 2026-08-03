# 現行アーキテクチャ

## コンポーネント

- `CloneCore`: エラー型、`Result<T>`、ログ、RAIIハンドル、ディスクI/O境界、安定識別、CRC32、GPT解析/生成、オフラインGPTクローン計画/実行、MBR解析/署名再生成/合成オフラインクローン
- `DiskModel`: 読み取り専用Windowsディスク列挙、実行中Windowsの所属ディスク検出、内部用デバイスインスタンスID、JSON/テキスト整形、安定識別への変換、検証済み物理ディスクI/O
- `BootRepair`: 信頼済み`System32`配下にある、埋込み署名またはWindowsカタログ署名でMicrosoft署名者を確認した`bcdboot.exe`、`mbr2gpt.exe`、`reagentc.exe`だけを固定引数で呼び、単独起動修復対象を再識別し、オフラインWindowsのWinRE登録先とイメージを読み取り専用診断する境界
- `MediaBuilder`: 利用者PCのローカルADK/WinPE Add-onを読み取り専用で検出し、固定構成、reparse、Microsoft署名、`/bootex`対応、作成許可ゲートを診断する境界
- `CliTools`: Phase 0から継続する読み取り専用診断CLI
- `WinPEApp`: 静的ランタイム構成で、読み取り専用列挙、クローン選択プリフライト、改ざん検知付きジョブの読取/再識別プリフライト、WinRE読み取り専用診断、VM専用直接クローン境界、製品向け予約ジョブクローン/復元サービス、単独起動修復を呼ぶCLIと、予約ジョブ/起動修復/診断、クローン/復元進捗/ETA/安全キャンセルを日本語で操作するネイティブWin32 GUI
- `ImageFormat`: 通常`.dcimg` v1、縮小`manifest.dcmig` v1、チャンク索引、Zstandardプロファイル1/非圧縮/ゼロチャンク、Windows CNG SHA-256、MBR/GPTパーティション表スナップショット、メモリ/有界Reader検証、WindowsファイルBackend、正規UTF-8 JSON＋SHA-256のWinPE引継ぎジョブv4
- `MigrationCore`: Windows/データ専用、GPT/MBRの基本NTFS使用量と安全余白から、コピー元を変更しない縮小移行先の純粋レイアウトを計算する
- `MigrationEngine`: コピー元の読取り専用役割解析、VSS/DISM WIM取得、`.dcmig`非上書き確定・完全検証、コピー先GPT/MBR作成、FORMAT、WIM適用、読戻し、WindowsディスクだけのBCDBootを担当する
- `VssRequester`: 固定VSS Workflow、Windows SDK標準COMバックエンド、Writer監査、有限非同期待機、Snapshot専用コピー境界、Bitmap使用範囲から`.dcimg`論理チャンクへの変換
- `WindowsApp`: ネイティブWin32の製品画面と、通常/縮小モード、Windows/データ専用ディスク、進捗/残り時間、クローン選択、`.dcimg`/`.dcmig`完全検証、復元先候補、レスキューメディアUI。オンラインイメージ作成だけは管理者確認後にVSSを使用し、ディスク復元書込みは再起動後のWinPE製品サービスに限定する
- `WindowsApp`の再起動引き継ぎは、管理者として手動起動済みのWindows 8以降だけでWindows標準`InitiateShutdownW`の詳細起動オプションを既定OFFで提案する。UAC、BCD/NVRAM/BootNext変更、他セッション/アプリの強制終了、USBの機種依存自動選択は行わない

依存方向は、Windows APIの具体処理を外縁に置き、GPT/クローン/縮小配置規則を合成メモリディスクで試験できる構造です。承認済み外部ライブラリはZstandard v1.5.7（BSD-3-Clause）、再配布する第三者UI資産はLINE Seed JP `LINESeedJP_20241105` Regular/Bold（OFL-1.1）です。

## 読み取り専用列挙

SetupAPIで`GUID_DEVINTERFACE_DISK`を列挙し、アクセス権`0`で開いたデバイスへ照会専用IOCTLだけを送ります。可変長データは上限と返却長を検証し、取得不能項目は推測せず診断へ残します。シリアルは末尾8文字以内だけを出力し、完全値を保持しません。

WinPEAppは列挙器とテキスト/JSON整形を`CliTools`経由で再利用し、`--clone-preflight --source N --target N`では再列挙した2台を`CloneCore::validate_clone_selection`へ渡します。プリフライトは未解決の列挙診断、同一ディスク、システムディスク、非GPTコピー元、空でない/非RAWコピー先、不明なoffline/read-only/removable属性、容量不足、非512バイト論理セクター、不安定識別を失敗として扱います。成功時も対象固有の確認トークンと`executionEnabled=false`を返すだけで、物理ディスクをoffline化せず、書込みを開きません。

`--clone-execute`は、対象消去承認、正確な確認トークン、許可語を必須とし、列挙とプリフライトをやり直してから`ICloneExecutionService`へ安定識別を渡します。通常製品の`main`はこの直接実行サービスへ明示的に`nullptr`を渡すため、実行要求を列挙前に終了コード`64`で拒否します。`tests/VM`の破壊的ビルドだけがVirtualBox、固定プロファイル、管理者権限、固定許可語を再検証するサービスを注入します。製品予約ジョブサービスは別の引数で接続し、VM固定許可語を受け付けません。

`--job-preflight --job-path <絶対パス>`は、64 KiB以下の通常ファイルを
`GENERIC_READ`だけで開き、reparse pointと読取り中の長さ変更を拒否します。
正規JSONとpayload SHA-256の検証後、保存されたディスク番号を信頼せず、
モデル、容量、論理セクター、シリアル末尾、デバイスインスタンスIDで
コピー元/コピー先を一意に再解決します。未解決の列挙診断、複数一致、
コピー先のシステム/読取り専用/removable/非512論理セクターは停止します。
復元ジョブではBitLocker完全復号、既知LDM型、Storage Spaces、
対応ファイルシステム、AC電源を合格/危険/不明で判定し、不明な必須項目を
安全扱いしません。再起動保留は警告`unknown`として残し、
`executionEnabled=false`を固定します。

`--job-execute`はクローン/復元予約ジョブだけを受け付けます。クローンは
同じ呼出し内でジョブ、コピー元/コピー先の安定識別、固定/オンラインの
基本GPT/MBRコピー元、同容量以上の空RAWまたは既知の基本GPT/MBR固定コピー先、既知BusType、
512バイト論理セクター、対応パーティション種別、二段階確認を再検証します。
製品クローンサービスはコピー元を`GENERIC_READ`で一度開いて同じハンドルを
保持し、GPT/MBRとVolume対応、MBRでは接続済み全MBRディスクの署名を
読取り専用で確認してから、コピー先だけをoffline化します。対象Writerを
開いた後にもコピー元/コピー先と空RAW状態を再確認し、全書込みの読戻しと
パーティション表最終確定後だけonlineへ戻します。失敗/中止時はonlineへ
戻しません。直接`--clone-execute`は引き続きVM専用です。

WinPE GUIは初回の読取り専用ディスク診断後、固定/リムーバブル媒体の
ドライブ直下または`Tsumugi`直下にある製品既定ジョブ名だけを最大92候補で
列挙します。候補なしは手動選択へ戻し、重複/複数/不正候補は自動選択しません。
一意な正規ジョブだけを自動プリフライトします。通常ジョブと旧v2の破壊的実行は
WinPE上の二段階確認を維持します。Windows側で既定OFFの一回限り自動実行を
明示したv3クローン/復元は、payload SHA-256へ結び付く開始記録をジョブ横へ
`CREATE_NEW`保存・flush・全バイト読戻ししてから同じ実行境界へ進みます。
既存/不完全な開始記録は削除せず、同じジョブの自動再実行を停止します。
プリフライト時のpayload SHA-256へ実行ローダーを固定するため、
確認後のジョブ差替えは物理I/O前に停止します。実行試行後はジョブ横へ
時刻付き結果ログを`CREATE_NEW`で保存し、flush/全バイト読戻し後だけ成功表示します。
Windows版の「ログ・診断」は同じ固定位置を読み取り専用で最大256件列挙します。
ImageFormatの共通パーサーが正規UTF-8、項目順、詳細長/SHA-256、完全再生成を
検証し、WindowsAppがファイル名のジョブ種別/完了UTCまで本文へ照合します。
改ざん、追記、重複、読取り不能が1件でもあれば、部分的な正常結果を表示しません。

復元では同じ呼出し内でジョブ/イメージ/対象ディスク/必須安全条件と
二段階確認をすべて再検証します。
製品復元サービスはコピー元物理ディスクを開かず、`.dcimg`を
`GENERIC_READ`かつ書込み/削除共有なしの同一ハンドルで完全検証し、
ジョブ記録済み長さと全体SHA-256へ一致した後だけ、再識別済みの復元先を
offline化します。対象は固定・非システム・非removable・オンライン・
書込み可能・既知状態・512バイト論理セクターに限定し、offline化後の
再列挙と物理ハンドル寸法も再確認します。途中失敗時は部分復元先を
onlineへ戻さず、全読戻しとパーティション表最終確定の成功後だけonlineへ
戻します。

`--boot-repair-preflight`は明示したディスク番号、Windowsルート、
システムルート、UEFI/BIOS方式を受け取り、ボリュームを`GENERIC_READ`だけで
開きます。単一extent、同一物理ディスク、NTFS、UEFIではFAT32 ESP、BIOSでは
一意なActive MBR区画、Windows 10/11 x64のカーネルとブートローダー、非reparse
通常ファイル、安定識別を検査して対象固有の`REPAIR BOOT`確認語を出します。
`--boot-repair-execute`は変更承認と確認語を必須とし、サービス内で同じ検査を
再実行してからだけ署名検証済みBCDBootを呼びます。終了コード0に加え、
UEFIは`EFI\Microsoft\Boot\BCD`、BIOSは`Boot\BCD`を通常ファイルとして
再確認します。通常製品は起動修復、予約ジョブクローン、復元サービスを
注入しますが、VM専用の直接`--clone-execute`サービスは注入しません。

実行中Windowsの所属ディスク判定を`system_disk.cpp`へ分離し、通常WinPEAppのリンク時にコピー先ライターを含む`physical_disk.cpp`が要求されないオブジェクト境界にしています。単独起動修復とジョブSHA-256検証を含む通常静的構成の依存DLLは`SETUPAPI.dll`、`KERNEL32.dll`、`ADVAPI32.dll`、`bcrypt.dll`、`CRYPT32.dll`、`WINTRUST.dll`で、VCランタイムDLLへ依存しません。`bcrypt.dll`はWindows CNG SHA-256、`CRYPT32.dll`と`WINTRUST.dll`はMicrosoftシステムツールの署名・証明書検証にだけ使用します。

WinPE GUIは同じ`WinPEAppLogic`を再利用し、追加の静的依存は
`USER32.dll`と`GDI32.dll`です。ファイル選択はSystem32の絶対パスから
`comdlg32.dll`を動的に読み込み、WinPE構成に存在しない場合はフルパスの
直接入力へ安全にフォールバックします。GUIの非同期処理はディスク番号などの
可変状態を保持せず、実行直前に既存サービス側で再列挙します。次回媒体は
ローカルADKの`WinPE-FontSupport-JA-JP.cab`をWIMへ直接追加しますが、
CABまたは生成WIM/ISOをリポジトリへ保存しません。

`UiSupport`はWindows版とWinPE版の実行ファイルへ埋め込んだ未改変のLINE Seed JP
App TTF Regular/Boldを`AddFontMemResourceEx`でプロセス内だけへ読み込みます。
OSのフォント登録は変更せず、2ウェイトのどちらかを読み込めない場合は両方を解除して
`Yu Gothic UI`へフォールバックします。埋込み資源を実際に選択し、GDIが返す書体名を
照合するヘッドレステストを持ちます。

## ディスクI/O境界

`ISourceDiskReader`は容量、論理セクターサイズ、範囲読取りだけを公開します。`ITargetDiskWriter`は別型で、書込み、読戻し、flushを公開します。Windows具体実装ではコピー元を`GENERIC_READ`だけで、コピー先を`GENERIC_READ | GENERIC_WRITE`、`FILE_FLAG_WRITE_THROUGH`で開きます。`WriteFile`は`src/DiskModel/src/physical_disk.cpp`のコピー先ライターだけに置き、安全境界検査で他の配置を拒否します。

物理I/Oの前に全ディスクを再列挙し、モデル、容量、論理セクター、シリアル末尾、デバイスインスタンスID、二段階確認を再検証します。ディスク番号はパス生成に使う直前の観測値であり、同一性判断には使いません。コピー先は現在のシステムディスクではなく、offlineかつ書込み可能と再確認できた場合だけ開きます。offline/online変更にも同じ再識別を適用し、変更後の再列挙結果が一致しなければ失敗します。Phase 1では実行中Windowsのシステムディスクをコピー元にする経路も拒否します。

具体的な破壊的実行サービスは`YTEC_BUILD_DESTRUCTIVE_VM_TESTS=ON`時だけビルドし、小容量ではVirtualBoxゲスト、8GiB以下の専用追加ディスク、RAWコピー先、固定許可語、管理者権限を追加条件とします。起動試験は別の固定96GiB→110GiBプロファイルと許可語だけを認めます。通常製品UI/CLIからこのサービスを生成・注入する経路はありません。

## GPTクローン処理

1. 保護MBR、主/副GPTヘッダーとパーティション配列を読取り、署名、CRC、LBA、サイズ、重複GUID、パーティション重複を検証する。
2. コピー先容量とセクターサイズを検証し、新しいDisk GUIDと全Partition GUIDを生成する。
3. EFI FAT32、MSR、基本NTFS、Windows回復NTFSの既知構成だけを計画し、それ以外は停止する。
4. EFI/回復は全領域、NTFSは`FSCTL_GET_VOLUME_BITMAP`由来の使用クラスタ範囲をコピーし、MSRはデータをコピーしない。
5. コピー前に安定識別と二段階確認トークンを再検証する。
6. ターゲットGPTを無効状態にしてからデータを書き、すべてを読戻し比較する。
7. 保護MBR、主/副配列、副ヘッダーを書き、主GPTヘッダーを最後に確定する。

GPT/MBRクローンと合成dcimg復元は共通`DiskOperationCallbacks`を受け取り、
計画、ターゲット無効化、データコピー、flush、パーティション表仮配置、
最終確定、完了を通知します。読取り、書込み、読戻し検証済みバイトと
各合計は別々に保持し、ゼロ復元を読取り量へ誤算入しません。キャンセルは
データコピーと先頭パーティション表の最終確定前まで受け付け、既存書込みを
flushして`cancelled`を返します。最終確定開始後はキャンセル不可を通知し、
コールバックを再照会しません。WinPE表示モデルは検証済みバイトと経過時間から
速度/残り時間を計算し、16 MiBかつ3秒に満たない推定は表示しません。

現段階の実行は512バイト論理セクターに限定しています。4Knの解析自体を許容しても、実際の書込み有効化は専用VM/実機相当検証後です。

## 起動修復境界

`BootRepair`は`PATH`検索をせず、現在のWindowsまたはWinPEから得た絶対`System32`パスを使用します。通常ファイルかつ非reparseであることを確認したうえで、まず埋込みAuthenticode署名を`WinVerifyTrust`で検証します。埋込み署名がない場合だけファイルハッシュからWindowsシステムカタログを列挙し、カタログメンバーとして`WinVerifyTrust`で検証します。どちらも信頼チェーンが有効で、署名証明書の組織名が厳密に`Microsoft Corporation`の場合だけ許可します。署名不正、カタログ不在、API失敗はすべて停止し、`bcdboot <target> /s <system> /f UEFI|BIOS /v`の列挙済み固定値だけを実行します。UEFIはWindows区画とESPの分離を要求し、BIOSはActive Windows区画がシステム区画を兼ねる構成を許可します。標準出力/標準エラーと終了コードを取得し、非ゼロ、タイムアウト、出力上限超過を失敗にします。

単独起動修復はパーティション作成、削除、移動、フォーマット、Active変更を
行いません。必要なWindows/ESP/Active構成が存在しない場合やBitLocker等で
通常のNTFS/FAT32として確認できない場合は、BCDBootを起動する前に停止します。
システム領域は、WinPEで安全に割当済みのルートを明示する方式に加え、対象の
固定・書込可能・非システムディスク上で一意な未割当FAT32 ESP（UEFI）または
Active `0x07` NTFS（BIOS）を実行時だけ一時マウントできます。Volume GUID、
ディスク番号、開始位置、extentを完全一致させ、既存マウント、候補重複、対応重複、
空き文字なしはBCDBoot前に拒否します。Y:から降順にX:を除外して選び、実行後の
明示解除成功まで完了にしません。RAIIデストラクタは例外を外へ出さないbest-effort
解除だけを担い、明示解除失敗を成功に変えません。

## イメージ形式境界

`ImageFormat`は全入力を不正データとして扱い、形式マジック、版、64bit位置/長さ、加算/乗算、正規セクション順序、予約領域、論理/保存範囲の重複を検査します。Windows CNG SHA-256でマニフェスト、全チャンク、独立ハッシュ表、フッター直前までの全体ハッシュを確認し、1バイトでも不一致なら失敗します。バックアップマニフェストは固定リトルエンディアン形式で、コピー元安定識別、Windows起動ディスクかデータ専用か、Windows時の版/AMD64、BitLocker完全復号、圧縮、起動方式、全区画の役割/範囲/ファイルシステムを保持します。解析後の再エンコードまで一致しなければ受理せず、復元前にはコンテナ、パーティション表、全データチャンクと意味を照合します。この有界ファイル検証器はWindowsAppとWinPEAppで共有し、同じ受理条件を使います。

製品の復元プリフライトはローカルドライブ文字の`.dcimg`通常ファイル、または
非reparseの`.dcmig`ディレクトリ内にある固定名`manifest.dcmig`を
`GENERIC_READ`で開き、reparse、ネットワーク、デバイス、ドライブ直下、
読取り範囲上限を拒否します。有界Readerは最大4 MiB単位で全体と全チャンクを
検証し、開始/終了時にヘッダーとフッターを再読込みして検証中の差替えも
受理しません。共通のメタデータ/復元領域ゲートを合成復元と共有しますが、
この入口は復元先を開かず、実行許可を常にfalseとします。完全検証後の
復元先候補評価も、既に列挙した情報だけを受け取る純粋な判定です。
安定識別、イメージ元との同一性、システムディスク、読取り専用/
リムーバブル/不明状態、パーティション形式、容量、論理セクターを
フェイルクローズで検査します。WindowsAppでは合格後に二段階確認を行い、
復元先を開かずハッシュ付き予約ジョブだけを新規保存します。WinPEAppの
`--job-preflight`は復元ジョブのSHA-256検証後、ディスク列挙より先に同じ
対応する`.dcimg`/`.dcmig`検証器を再実行し、復元先容量、論理セクター、元ディスクとの分離を
確認します。ドライブ文字が変わった場合は明示された別パスを完全検証し、
ジョブv4（旧v2/v3読込み互換）へ保存したイメージ長とSHA-256の完全一致を要求します。
既知LDMパーティション型/Storage Spaces BusType・保護型の基礎検査を
含みます。ドライブ文字変更は固定/リムーバブル媒体の同じ相対パスだけを
最大23候補として列挙し、完全検証とジョブ指紋一致後だけ自動再解決します。
任意フォルダの再帰探索、再起動保留の具体検査、物理復元許可は含まず、
`executionEnabled=false`を固定します。

パーティション表スナップショットはMBRの先頭1セクター、またはGPTの保護MBR/主副ヘッダー/主副エントリ配列だけを読取り専用コピー元から取得し、先頭/末尾2領域の正規形式で保持します。合成復元はコンテナ全体、マニフェスト、スナップショット、復元先安定識別、容量、セクター、非システム、二段階確認、チャンクの宣言区画内配置を最初の書込み前に検証します。有界Reader版はイメージ全量を保持せず完全検証し、その結果と同じReaderを再走査不能な準備済み復元元へ封入します。復元時は完全検証を二重走査せず、書込み直前に非ゼロチャンクだけを最大32MiB保持し、Zstandardなら有界展開して記録済みSHA-256を再確認します。Reader例外、短い読取り、検証中キャンセルはターゲット書込み前に失敗し、完全検証後に内容が変化したチャンクもパーティション表確定前に拒否します。パーティション表を先に無効化し、全データの書込み/読戻し/flush後に末尾側から確定して先頭側を最後にします。作成側はチャンクごとのZstandard level 3圧縮（小さくならない場合は非圧縮fallback）、抽象ステージングへの有界ストリーム、正規最終長への縮小、全索引/展開後チャンク/全体SHA-256再読込み、成功後commit、失敗時abortまで実装しています。WindowsファイルBackendは保存先ボリュームを物理ディスクへ対応付け、開始時/確定前に同じ再列挙結果へコピー元/指定コピー先/保存先を一意に対応付け、相互の同一性、空き容量、ローカル絶対パス、reparse不使用、完成/未完了ファイルの不存在を検査します。保護DACL付き`CREATE_NEW`の`.partial`だけをwrite-throughで所有し、全件検証後に所有中ハンドルへ`FILE_RENAME_INFO`を指定して上書きなしで完成名へ確定します。製品VSS経路ではこの確定をさらに遅延し、`BackupComplete`とSnapshot set削除後だけ完成名へします。Zstandardは公式v1.5.7をBSD-3-Clause条件で静的リンクし、辞書/legacy/連結フレームを許さないプロファイル1へ固定しています。

縮小`.dcmig`は単一ファイルではなく、固定名`manifest.dcmig`と各NTFS内容の
WIMを持つディレクトリ束です。`MigrationCore`はコピー元総容量を使わず、使用量、
安全余白、ESP/MSR/回復の必須量からコピー先最小容量を計算します。
`MigrationEngine`はコピー元を読取り専用で解析し、オンライン作成ではVSS
SnapshotのVolume GUIDだけをDISMへ渡します。全WIMとmanifestの長さ/SHA-256、
非reparse、宣言外項目なし、VSS cleanupを確認後だけ最終名へ確定します。復元では
同じ束を置換不能の読取り専用ハンドルで保持し、コピー先GPT/MBRを新規作成して
FORMAT/WIM適用/読戻しを行い、WindowsディスクだけBCDBootを実行します。

## MBR/Legacy BIOS境界

MBRパーサーは512バイト論理セクター、`0x55AA`、予約領域、4個のプライマリエントリ、Active値、32bit LBA境界、重複を検査します。GPT保護MBR、複数Active、拡張パーティションは現段階で停止します。書込み計画はコピー元と接続中ディスクの署名を禁止集合にし、Windows CNG乱数を最大32回だけ試して新しい署名を生成します。合成実行ではNTFS使用クラスタと回復NTFS全領域をコピーして各書込みを読戻し、コピー先MBRを最初に無効化してデータflush後の最後に確定します。

既定OFFの破壊的VM構成には固定48GiB MBR→56GiB RAWプロファイルと、固定56GiBコピー先だけを扱うBIOS BCDBootハーネスがあります。どちらも通常製品から分離し、VirtualBox、安定識別、管理者権限、固定許可語、二段階確認を要求します。実WinPEクローン、BCDBoot、コピー元/ISOを外したLegacy BIOS単独起動まで確認済みです。

## MBR→GPT境界

独自変換は行わず、現在のWinPEのMicrosoft署名済み`System32\mbr2gpt.exe`だけを使います。引数は`/validate /disk:N`と`/convert /disk:N`に限定し、`/allowFullOS`と`/map`を生成しません。安定識別、非システム、二段階確認、Microsoft署名を検証して`/validate`を実行し、終了コード0の後に同じディスク番号を再列挙して安定識別を再検証してからだけ`/convert`へ進みます。

WinREの有無と登録状態は、Microsoft署名を確認した現在の
`System32\reagentc.exe`へ`/info /target <オフラインWindows>`だけを渡して
読み取ります。出力に含まれる登録ディスク番号を再識別済み対象へ照合してから、
登録先または固定フォールバックの`Winre.wim`を`GENERIC_READ`で開き、
通常ファイル、非reparse、容量上限を確認します。REAgentC失敗、複数の異なる
登録先、対象ディスク不一致、登録先とイメージの食い違いは不明として停止し、
確認できた事実だけを別ターゲット再構築の純粋プランへ渡します。この診断は
デバイス、BCD、WinRE登録へ書き込みません。

製品WinPE CLIでは`--winre-diagnostic --disk N --windows-root W:\`として
公開します。引数を固定形式で検証し、物理ディスクを読み取り専用で再列挙して
安定識別を作れることを確認してから診断サービスへ渡します。結果はテキスト/
JSONで`readOnly=true`、`executionEnabled=false`として出力し、REAgentC非ゼロ
終了や不明状態では診断内容を残しつつ終了コード1で停止します。

この境界はモックと署名統合試験済みです。物理実行は既定OFFの固定56GiB VirtualBox専用ハーネスだけに準備し、通常製品には未接続です。BitLocker、動的ディスク、Windowsインストール、UEFI対応の周辺ゲートを製品側へ実装するまで一般入口は作りません。

## Windows VSS境界

`VssRequester`の共通Workflowは管理者、正規Volume GUID、NTFS、重複を開始前に検証し、`InitializeForBackup`から`DeleteSnapshots`までの順序を固定します。Writerが0件、名前なし、Snapshot直後のStable/BackupComplete待ち以外、HRESULTが`S_OK`以外の場合はコピーへ進みません。`BackupComplete`後はStableだけを許可します。Snapshot set作成後の失敗は作成したsetだけの削除を試み、Cleanup失敗も成功扱いしません。

Windows具体バックエンドはWindows SDKの`IVssBackupComponents`と`IVssAsync`だけを使用します。COMの致命的例外を隠さない設定、ローカルVSS向けプロセスセキュリティ、有限timeoutとキャンセル、Writer名/ID/状態/HRESULTのログ、BSTR/COM/Snapshot属性のRAII解放を実装しています。`GetSnapshotProperties`のset ID、Snapshot ID、元Volume、Snapshotデバイス形式を再検証し、後段へはSnapshotデバイスパスだけを渡します。Snapshot Readerは`GENERIC_READ`、容量/セクター再照合、有界・セクター整列読取りに限定します。StorageAccessAlignmentPropertyをSnapshotデバイスが未サポートと明示した場合だけ、ファイルシステムbytes-per-sector照会へ限定fallbackします。Snapshot専用Bitmap Providerは通常Volume GUIDを型とパス検査で拒否します。

再識別済みの読取り専用物理ディスクから、GPTはEFI/回復をraw、Windows NTFSをVSS、MSRを表から再作成し、MBRはWindows NTFSをVSS、FAT32/回復をrawへ振り分けます。rawとSnapshotの全Reader Geometry、ブートセクター、Volume対応、区画境界を検査し、同じ`.dcimg`へ統合します。Windows VSS具体バックエンド、実ファイルBackend、遅延確定、製品GUIまで接続し、標準権限では最初の物理ディスクオープン前に停止します。管理者権限を使う製品一体経路は固定Windows 10 x64 VMで容量不足、コピー中キャンセル、正常完了を実行し、Writer 10件、全経路のShadow Copy残留0、`.partial`残留なし、正常経路の完成`.dcimg`確定を確認しました。独立ハーネスでもSnapshot内Sentinel、raw boot sector、容量/論理セクター、使用範囲2,889件を確認しています。VSS生成イメージから別ディスクへ復元してWindows起動する回帰は未実施です。

## WinPE環境検出境界

`MediaBuilder`は固定した標準パス、環境変数、Windows Kits登録情報だけからADK候補を作ります。amd64のDeployment Tools、WinPE Add-on、`copype.cmd`、`MakeWinPEMedia.cmd`、基本`winpe.wim`、DISM、Oscdimgを読み取り専用で確認します。必要ファイルのreparseを拒否し、実行可能ファイルは`BootRepair`と同じMicrosoft署名検証境界へ渡します。コマンドスクリプトは1MiBを上限に読み、`/bootex`の存在だけを診断します。

Windows GUIのレスキューメディア画面はこの検出結果をバックグラウンドで
読み取り専用照会し、BIOS/UEFI基本構成、2023 CA用`/bootex`、
検証済みバージョン/必須更新、作成許可ゲートを表示します。
成功時は選択された候補だけの診断を表示し、未使用候補の欠落を混在させません。
合格後はISO/USB、2011 CA互換/2023 CA、出力先、作成前要約へ進みます。
ISOはドライブ文字付きローカル絶対`.iso`新規パスだけを受け付けます。
USBは列挙済み情報から非システム、USB Bus、取り外し可能、オンライン、
非読み取り専用、既知パーティション形式を要求し、安定識別情報と
対象固有の確認語を作ります。この段階は計画だけで、出力ファイルやUSBを
開きません。後続のelevated実行サービスは開始直前に同じパスと安定識別を
再検査し、USBでは二段階確認を再要求する設計です。
この入口はUACを要求せず、WIM、ISO、USBを変更しません。

ファイル配置と署名が揃った状態は「ローカル構成準備済み」に過ぎません。2026-07-30時点の許可ポリシーとして、Windows Installer製品バージョン`10.1.26100.2454`、KB5101684のOscdimg/DISMパッチが適用済みであること、Microsoft署名済みDISMが`10.0.26100.8972`であることを追加確認します。いずれかが不明なら`media_creation_permitted=false`を維持します。Microsoftのポリシー更新後は許可リストを再評価するまでフェイルクローズします。

`New-WinPEAppValidationMedia.ps1`はこの診断ゲートの後段にあり、既定では何も作成しない事前検証だけを行います。明示的な`-BuildMedia`、管理者権限、リポジトリ外の未作成出力先が揃った場合だけ、ADK媒体とWIMの複製を作業対象にします。元WIMは変更せず、予約先の既存ファイルを拒否し、自作WinPE CLI/GUI、起動用cmd、`winpeshl.ini`、第三者通知、LINE Seed JPのOFL本文、ローカルADKの日本語フォントサポートだけを追加します。DISMマウントが通常ファイルへ付けるreparseはWindows SDK定義の`IO_REPARSE_TAG_WIM (0x80000008)`だけを許し、抽出後の通常ファイルでSHA-256、AMD64 EFI形式、Microsoft署名を再検証します。追加前後のWIM、追加ファイル、生成ISOのSHA-256とサイズをmanifestへ記録し、失敗時はマウントを破棄して成功扱いにしません。通常製品WinPEAppには予約ジョブクローン/復元サービスと単独起動修復サービスを接続します。2026-07-31に単独起動修復シナリオの2023 CA ISOをリポジトリ外へ生成し、UEFI64/Secure Boot有効の独立VMでBCD破損、起動失敗、Microsoft署名済みBCDBootによる再構築、BCD再読込み、Windows再起動を確認しました。Legacy BIOS/MBRでも製品プリフライトとBCDBoot後に修復前`0xc000000e`からWindowsデスクトップへ復帰しました。製品予約ジョブはGPT/MBRクローンと小容量MBR dcimg復元を実WinPEで完走しています。2026-08-01にはクローン／復元キャンセル、破損dcimg、改ざんジョブの製品WinPE経路を合成VDIで確認し、失敗時のパーティション表未コミットと復元先不変を実証しました。同日のLINE Seed追加直前版2011/2023 CA完成ISOは、HDD/NICなしVMのLegacy BIOS、UEFI、Secure Boot有効/無効6条件で日本語製品GUIまで起動しました。現在はLINE Seed JP埋込みEXEとOFL本文を含むISOの再生成待ちです。VSS生成イメージからのWindows起動復元は未確認です。

## 後続Phaseとの境界

- Phase 1: GPT/UEFIオフラインクローン
- Phase 2: イメージバックアップ/復元
- Phase 3: MBR/Legacy BIOS
- Phase 4: MBRからGPTへの明示変換
- Phase 5: Windows VSSイメージ作成
- Phase 6: Windows直接クローン・WinPE引継ぎ
- Phase 7: 利用者のADK/WinPE Add-onからのレスキューメディア生成と公開品質。ADKバージョンと更新状態を検査し、Windows UEFI 2011 CA互換メディアと`MakeWinPEMedia /bootex`による2023 CAメディアを明示的に区別する

GPTからMBRへの変換、BitLocker回避、Secure Boot回避は対象外です。
