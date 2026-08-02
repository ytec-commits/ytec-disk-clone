# Windowsディスククローン・イメージバックアップツール 開発仕様書

**Codex実装用／社内配布・一般公開対応**  
文書版数: 1.0  
作成日: 2026年7月29日  
プロジェクト名: Windowsディスク移行・復旧ツール（仮称）  
文書区分: 要件定義・基本設計・ライセンス適合要件・実装指示

> 本文書は、OpenAI Codexにリポジトリ単位で実装作業を依頼するための基準文書である。Codexは本文書を最上位のプロジェクト指示として扱い、機能追加、依存ライブラリ追加、Microsoft製ファイルの配布、破壊的ディスク操作を独断で行ってはならない。

---

## 1. 文書の目的

本プロジェクトは、Windows 10／11搭載PCのSSD・HDD・NVMe交換、同一PCでの障害復旧、および適切にライセンスされた環境への移行を目的とする、限定機能のディスククローン／イメージバックアップツールを開発する。

本ツールはAcronis、HD革命、Macrium等の既存製品を複製するものではない。対応範囲を明確に限定し、独自カーネルドライバ、増分バックアップ、クラウド、異機種復元等を実装しないことで、安全性、保守性、ライセンス適合性を優先する。

### 1.1 最終目標

- Windows 10／11のシステムディスクを同容量以上の別ディスクへ複製できる。
- GPT／UEFI構成とMBR／BIOS構成を扱える。
- MBR構成をコピー先でGPT／UEFI構成へ自動変換できる。
- Windows上ではVSSを使用したオンラインバックアップ／クローンを実行できる。
- WinPE上ではオフラインクローン、バックアップ、復元、起動修復を実行できる。
- ディスク全体を独自バックアップイメージとして保存・検証・復元できる。
- 社内配布と一般公開の双方に耐えるライセンス管理、SBOM、第三者通知を備える。

### 1.2 成果物

1. Windows版GUIアプリケーション
2. WinPE版GUIアプリケーション
3. 共通ネイティブクローン／イメージエンジン
4. WinPEメディア作成ウィザード
5. CLIテスト・自動化ツール
6. インストーラーまたは配布ZIP
7. 操作マニュアル
8. THIRD-PARTY-NOTICES.txt
9. SPDX形式のSBOM
10. ライセンス監査記録
11. テスト仕様書・テスト結果

---

## 2. 最重要方針

### 2.1 安全性優先順位

優先順位は次のとおりとする。

1. コピー元データを変更・破壊しない
2. 誤ったコピー先を消去しない
3. 破損したイメージを復元しない
4. 起動不能な複製を成功扱いにしない
5. ライセンス違反の配布物を生成しない
6. 速度を最適化する

速度や操作性のために安全確認を省略してはならない。

### 2.2 実装禁止事項

Codexは以下を実装・追加してはならない。

- 独自カーネルドライバ
- ファイルシステムフィルタドライバ
- 変更ブロック追跡ドライバ
- BitLocker解除・回避・資格情報抽出
- Windowsライセンス認証の回避
- Secure Boot回避
- パスワード回復・解析
- GPL、AGPL、SSPL、Commons Clause、ライセンス不明コードの組み込み
- Acronis、HD革命、Macrium等のコード・画面・文言・独自形式の模倣
- Microsoft製EXE、DLL、WIM、ISO、ロゴ、アイコンの配布物へのコピー
- 完成済みWinPE ISO、boot.wim、WinPE入りUSBイメージの一般配布
- テレメトリ、広告、クラウド通信、利用者追跡
- ユーザー確認を省略した破壊的処理
- サポート対象外構成を推測で処理する「ベストエフォート復元」

### 2.3 失敗時の原則

不明、矛盾、検証失敗、VSS Writer異常、ディスク識別不能、対象外パーティション検出時は、処理を開始せず安全側に停止する。エラーを無視して続行する隠しオプションは作らない。

---

## 3. 対応範囲

### 3.1 対応OS

- Windows 10 x64
- Windows 11 x64
- Windows PE x64（利用者がWindows ADK／WinPEアドオンから生成した環境）

ARM64および32bit Windowsは初版対象外とする。

### 3.2 対応ディスク

- SATA SSD
- SATA HDD
- NVMe SSD
- USB接続されたSATA／NVMeケース内のディスク
- Windowsから通常の物理ディスクとして認識される固定ディスク

### 3.3 対応パーティション方式

| コピー元 | コピー先 | 初版対応 | 備考 |
|---|---|---:|---|
| GPT／UEFI | GPT／UEFI | 必須 | 標準経路 |
| MBR／BIOS | MBR／BIOS | 必須 | レガシーPC向け |
| MBR／BIOS | GPT／UEFI | 必須 | WinPEで最終変換・起動修復 |
| GPT／UEFI | MBR／BIOS | 非対応 | 容量・パーティション数・Windows 11要件のため対象外 |

### 3.4 対応ファイルシステム

- NTFS: 使用クラスタ単位のスパースコピー
- FAT32: EFIシステムパーティションのコピー
- 未フォーマットMSR: 定義のみ再作成
- Windows回復パーティション: NTFSとしてコピー
- 認識済みOEMパーティション: 原則として同サイズの生ブロックコピー

不明なパーティション種別、Linux、BitLocker暗号化済み、ReFS、FAT16、ext系、LVM等を検出した場合は、初版では処理を停止する。

### 3.5 BitLocker

コピー元に存在する全BitLocker対象ボリュームは、処理開始時点で**完全復号済み**でなければならない。「保護の中断」のみでは不可とする。アプリは状態を検出し、暗号化率が0%でない場合は処理を禁止する。

本ツールは回復キーを収集、保存、送信しない。

### 3.6 容量条件

- コピー先物理ディスク容量はコピー元物理ディスク容量以上を原則とする。
- セクター数が1つでも不足する場合は開始不可とする。
- 「使用量が少ないため小容量ディスクへコピー」は初版対象外とする。
- コピー先が大きい場合、最後のWindowsパーティションを安全に拡張できる構成でのみ自動拡張する。
- 回復パーティションが末尾にある場合は、回復パーティション位置を維持する設計を優先し、無理な自動移動は行わない。

### 3.7 セクターサイズ

初版は、コピー元とコピー先の論理セクターサイズが同一の場合のみ対応する。

- 512／512e間: 同一論理セクターとして扱える場合に対応
- 4Kn→4Kn: 実機検証後に有効化
- 512系↔4Kn: 初版非対応

---

## 4. 非対応範囲

以下は明示的に対象外とする。

- 動的ディスク
- Storage Spaces
- WindowsソフトウェアRAID
- ハードウェアRAID配下で物理構成が取得できないケース
- 差分／増分バックアップ
- スケジュールバックアップ
- クラウド保存
- ネットワーク経由のベアメタル復元
- 重複排除
- バックアップ暗号化
- パスワード保護
- 異なるPCへの汎用復元
- ドライバ注入による異機種復元
- Linux／macOSの起動ディスク
- BitLocker有効状態を維持したクローン
- GPTからMBRへの変換
- 小容量ディスクへの縮小移行
- 稼働中システムディスクのその場上書き復元
- Windows Serverの正式サポート
- VHD／VHDXを製品機能として直接操作すること

---

## 5. ユースケース

### UC-01 GPTシステムディスクの直接クローン

利用者がWindows上またはWinPE上でコピー元とコピー先を選択し、GPT／UEFI構成を同容量以上のディスクへ複製する。コピー先には新しいディスクGUID・パーティションGUIDを割り当て、BCDを再構築する。

### UC-02 MBRシステムディスクの直接クローン

MBR／BIOS構成を同容量以上のディスクへ複製する。コピー先のディスク署名衝突を回避し、BIOS起動用のBCDを再構築する。

### UC-03 MBRからGPTへの自動移行

Windows版でクローンを開始した場合も、最終変換は利用者生成WinPEへ引き継ぐ。まずコピー先へMBR構成として安全に複製し、WinPE内に存在するMicrosoft署名済み`mbr2gpt.exe`で検証・変換し、`bcdboot.exe`でUEFI起動環境を構築する。変換失敗時はコピー元を変更せず、コピー先を失敗状態として明示する。

### UC-04 バックアップイメージ作成

物理ディスクのパーティション構成とデータを、独自イメージ形式へ保存する。Windows上ではVSSスナップショットから読み取り、WinPE上ではオフラインで読み取る。作成後に全チャンクのハッシュ検証を行う。

### UC-05 バックアップイメージ復元

WinPE上でイメージを検証し、同容量以上のディスクへ復元する。復元前にコピー先を二段階確認し、復元後に起動環境を再構築する。

### UC-06 レスキューメディア作成

利用者のPCにインストール済みのWindows ADKとWinPEアドオンを検出し、利用者のローカル環境内でWinPE USBまたはISOを生成する。製品配布物にWinPE本体は含めない。

---

## 6. システム構成

```text
Repository
├─ src/
│  ├─ CloneCore/              # 共通ネイティブライブラリ
│  ├─ DiskModel/              # ディスク・パーティションモデル
│  ├─ ImageFormat/            # 独自イメージ読み書き
│  ├─ VssRequester/           # Windows版VSS Requester
│  ├─ BootRepair/             # BCDBoot/MBR2GPT呼出しと検証
│  ├─ WindowsApp/             # Windows GUI
│  ├─ WinPEApp/               # WinPE GUI
│  ├─ MediaBuilder/           # 利用者側WinPE生成
│  └─ CliTools/               # 診断・テストCLI
├─ tests/
│  ├─ Unit/
│  ├─ Integration/
│  ├─ ImageFuzz/
│  └─ TestData/
├─ docs/
│  ├─ architecture.md
│  ├─ image-format.md
│  ├─ licensing.md
│  ├─ safety-model.md
│  └─ test-plan.md
├─ packaging/
├─ third_party/
├─ licenses/
├─ THIRD-PARTY-NOTICES.txt
├─ SBOM.spdx.json
├─ CMakeLists.txt
└─ README.md
```

### 6.1 言語・ビルド

- C++20
- CMake
- MSVC x64
- Unicode APIのみ使用
- Win32 GUIまたはWinPEで利用可能なネイティブUI
- 例外処理、RAII、スマートポインタを標準とする
- 警告レベルを高く設定し、警告ゼロを維持する
- `/permissive-`、`/W4`、可能な範囲で`/WX`
- AddressSanitizer対応のテスト構成を用意する

WinPE実行性のため、ランタイム依存は最小化する。MSVCランタイムの静的または動的リンク方式は、実機動作と組織のVisual Studioライセンス条件を確認して決定する。

### 6.2 外部依存

初版で許可する候補は以下のみとする。追加は人間の承認が必要。

| 用途 | 候補 | ライセンス方針 |
|---|---|---|
| 圧縮 | Zstandard | BSD条件を選択し通知同梱 |
| ハッシュ | Windows CNG / BCrypt | Windows公開API |
| JSON | 原則自作最小シリアライザ、必要時nlohmann/json | MIT、承認制 |
| テスト | Catch2またはGoogleTest | ライセンス確認後、開発時依存として承認制 |

製品本体にBoost、Qt、Electron、Chromium、.NET Desktop Runtime等の大型依存を安易に追加しない。

---

## 7. ディスク検出・モデル化要件

### 7.1 物理ディスク識別

各ディスクについて以下を取得し、UIとログへ表示する。

- Windowsディスク番号
- デバイスパス
- ベンダー／モデル名
- 容量（バイト、セクター数）
- 論理／物理セクターサイズ
- 接続方式
- シリアル番号（UIでは末尾4〜8桁のみ）
- GPT／MBR／RAW
- システムディスク判定
- ブートディスク判定
- オフライン／読み取り専用状態
- リムーバブル判定
- BusType

### 7.2 安定識別子

処理開始前に取得した情報と、実際の書き込み直前に再取得した情報を比較する。ディスク番号だけを信頼してはならない。モデル、容量、シリアル、デバイスインスタンス等の複合識別を使用し、差異があれば中止する。

### 7.3 コピー元保護

- コピー元ハンドルは可能な限り読み取り専用で開く。
- コピー元に対する`WriteFile`経路を設計上分離する。
- 書き込みAPIは`TargetDiskWriter`クラスからのみ呼べるようにする。
- コピー元とコピー先の安定識別子が一致した場合は即時中止する。
- システムディスクをコピー先に選択できない。

---

## 8. クローン方式

### 8.1 共通パイプライン

```text
Source Disk
  -> Preflight validation
  -> Snapshot provider (VSS or offline)
  -> Partition plan
  -> Sparse/raw reader
  -> Chunk hashing
  -> Target writer or Image writer
  -> Read-back verification
  -> Boot finalization
  -> Final report
```

### 8.2 NTFSコピー

NTFSは、スナップショット対象ボリュームに対して`FSCTL_GET_VOLUME_BITMAP`を使用し、使用中クラスタのみを読み取る。ライブボリュームから取得したビットマップとVSSスナップショットを混在させてはならない。

要件:

- ビットマップはVSSスナップショットデバイスまたはオフラインボリュームから取得する。
- `ERROR_MORE_DATA`を正しく処理し、全クラスタ範囲を反復取得する。
- クラスタサイズ、ボリューム総クラスタ数、NTFSメタデータを記録する。
- コピー中に読み取りエラーが発生した場合は標準動作として中止する。
- 不良セクターを0埋めして続行するモードは初版に実装しない。
- コピー先パーティションのサイズ変更を行う場合は、NTFSの公式Windows APIまたはWindows標準ツールを使用し、独自にNTFSメタデータを書き換えない。

### 8.3 FAT32／OEMパーティション

EFIシステムパーティションは、小容量であるため原則として全領域コピーまたはファイル単位コピーを採用できる。ただし最終的な起動ファイルは`bcdboot.exe`で再生成するため、元のBCDをそのまま信頼しない。

認識済みOEMパーティションは、サイズ変更せず全領域をコピーする。対象が大きすぎる場合や種別不明の場合は警告ではなく停止する。

### 8.4 コピー先GUID／署名

- GPTコピー先には新しいDisk GUIDを生成する。
- GPT各パーティションには新しいPartition GUIDを生成する。
- MBRコピー先には衝突しない新しいディスク署名を設定する。
- 既存BCDをそのまま複製せず、コピー先構成に基づき再構築する。
- コピー元とコピー先を同時接続してもWindowsがディスク署名衝突で誤動作しない設計にする。

---

## 9. Windows上のオンライン処理

### 9.1 VSS Requester

Windows版は、公開VSS APIを用いるRequesterとして実装する。

標準フロー:

1. 管理者権限確認
2. 対象ボリューム列挙
3. `IVssBackupComponents`初期化
4. バックアップ状態設定
5. Writer metadata収集
6. Snapshot set作成
7. 対象NTFSボリュームを追加
8. PrepareForBackup
9. DoSnapshotSet
10. Writer状態確認
11. スナップショットデバイスからコピー
12. BackupComplete
13. スナップショット削除

### 9.2 Writer異常

システム状態に関係するVSS Writerが失敗・不安定の場合は処理を中止する。初版では「無視して続行」機能を作らない。エラー詳細、Writer名、状態、HRESULTをログへ出力する。

### 9.3 オンライン復元

稼働中のWindowsシステムディスクを上書き復元しない。Windows版で復元を選択した場合、復元ジョブ定義を保存し、利用者生成WinPEへ再起動して実行する。

### 9.4 Windows版直接クローン

GPT→GPTおよびMBR→MBRのデータコピーはWindows上で実行可能とする。ただし、以下の場合はWinPE最終処理へ引き継ぐ。

- MBR→GPT変換
- 排他的アクセスが必要なパーティション変更
- オンラインで安全に完了できない起動修復
- コピー先がOSにより自動マウントされ安全なオフライン化ができない場合

---

## 10. WinPE処理

### 10.1 WinPE版の役割

- オフラインクローン
- オフラインイメージ作成
- システムディスク復元
- MBR→GPT変換
- BCD／UEFI起動修復
- ジョブ自動実行
- ログ保存

### 10.2 ジョブ引き継ぎ

Windows版は、次の情報を署名付きまたはハッシュ付きJSONジョブとして保存する。

- ジョブ種別
- コピー元／コピー先の安定識別情報
- イメージファイルパス
- 要求された変換方式
- 作成日時
- アプリバージョン
- 確認済みの破壊対象情報
- ジョブハッシュ

WinPE版はディスク番号を再解決し、モデル・容量・シリアル等が一致しない場合は自動実行せず利用者確認画面へ戻る。

### 10.3 WinPE不足コンポーネント

必要なWinPEオプションコンポーネントが利用者の環境に存在しない場合、メディア作成を中止し、不足項目とMicrosoft公式手順を表示する。Microsoft製CAB等を製品側から配布しない。

---

## 11. MBRからGPTへの自動変換

### 11.1 基本方針

自作コードでWindowsシステムディスクのMBR→GPT変換ロジックを直接実装しない。Microsoftが対象Windows／WinPEに提供する`mbr2gpt.exe`をローカル環境から検出して使用する。

### 11.2 処理手順

1. コピー元MBRディスクをコピー先へMBRとして複製
2. コピー元をオフラインまたは物理的に保護
3. WinPE上でコピー先を再識別
4. コピー先に対して`mbr2gpt /validate`を実行
5. 検証成功時のみ`mbr2gpt /convert`を実行
6. GPT構成を再列挙し、EFI／MSR／Windowsパーティションを確認
7. `bcdboot <WindowsDir> /s <ESP> /f UEFI`を実行
8. BCD、EFIファイル、パーティション属性を検証
9. 利用者へファームウェアをUEFIモードへ変更する必要性を明示
10. コピー元を接続したままの初回起動を避けるよう案内

### 11.3 コマンド実行安全性

- `mbr2gpt.exe`と`bcdboot.exe`は、現在のWindowsまたはWinPE内の正規パスからのみ実行する。
- Microsoftデジタル署名を検証する。
- PATH検索だけで実行しない。
- コマンドラインを文字列連結で組み立てず、厳格にエスケープする。
- 標準出力、標準エラー、終了コード、ログフォルダを保存する。
- 実行ファイルを自社配布物へコピーしない。

### 11.4 失敗時

- コピー元へ変更を加えない。
- コピー先を成功扱いにしない。
- コピー先の現在状態を記録する。
- 自動ロールバックで危険な再パーティションを行わない。
- 利用者にコピー先を再初期化して再試行する手順を提示する。

---

## 12. 独自バックアップイメージ形式

### 12.1 形式の目的

- ディスク全体構成を保存する
- 空き領域を保存しない
- チャンク単位で圧縮・検証する
- 破損を早期検出する
- 復元前に全体整合性を確認する
- 将来の互換性を維持する

### 12.2 拡張子

仮拡張子を`.dcimg`とする。製品名決定後に変更可能だが、形式マジックとバージョンは製品名に依存させない。

### 12.3 コンテナ構造

```text
[Fixed Header]
[Manifest JSON or binary manifest]
[Partition Table Snapshot]
[Chunk Index]
[Compressed Data Chunks]
[Hash Table]
[Footer + global hash]
```

### 12.4 必須メタデータ

- 形式マジック
- 形式メジャー／マイナーバージョン
- 作成アプリバージョン
- 作成日時（UTC）
- コピー元ディスク容量・セクターサイズ
- コピー元パーティション方式
- パーティション一覧
- ファイルシステム種別
- クラスタサイズ
- Windowsバージョン情報
- ブート方式
- BitLocker完全復号確認結果
- 圧縮方式とバージョン
- チャンクサイズ
- 各チャンクの論理オフセット、未圧縮長、圧縮長、SHA-256
- マニフェストハッシュ
- 全体ハッシュ

### 12.5 チャンク

- 標準チャンクサイズ: 16 MiBまたは32 MiB
- 0埋めチャンクはデータを保存せずフラグ表現
- 圧縮効果がない場合は非圧縮保存
- 各チャンクは独立復号可能
- チャンク長・オフセットは64bit
- オーバーフローと範囲外参照を全て検査する

### 12.6 整合性

- SHA-256はWindows CNGを使用する。
- イメージ作成後は全チャンクを読み戻して検証する。
- 復元開始前は、最低でもマニフェスト、インデックス、全チャンクハッシュを検証する。
- 検証を省略する「高速復元」は初版に実装しない。
- 1バイトでも不一致があれば復元不可とする。

### 12.7 分割ファイル

FAT32等の制約対応は初版では任意とする。実装する場合は4GiB未満の分割ファイルとし、全分割ファイルの順序・サイズ・ハッシュをマニフェストに記録する。欠損時は復元不可とする。

### 12.8 後方互換

- メジャーバージョン不一致は読み取り不可
- 未知の必須フラグは読み取り不可
- 未知の任意フィールドは無視可能
- 古い形式の読み取りテストデータをリポジトリに保持
- 形式仕様を`docs/image-format.md`に固定する

---

## 13. UI要件

### 13.1 メイン画面

- 「ディスクをクローン」
- 「バックアップイメージを作成」
- 「バックアップイメージを復元」
- 「レスキューメディアを作成」
- 「ログ／診断」

### 13.2 ディスク選択表示

最低限、以下を同時表示する。

- ディスク番号
- モデル名
- 容量
- 接続方式
- GPT／MBR
- システム／ブート表示
- パーティション図
- シリアル末尾
- コピー元／コピー先ラベル

### 13.3 破壊確認

コピー先消去前に二段階確認を行う。

第1段階: 対象のモデル、容量、シリアル末尾、全パーティションが削除されることを表示。  
第2段階: 表示された確認語を利用者が手入力する。確認語は固定の「YES」ではなく、ディスク番号またはシリアル末尾を含める。

### 13.4 進捗

- 現在工程
- パーティション名
- 読み取り済み／総データ量
- 書き込み済み量
- 検証済み量
- 転送速度
- 経過時間
- 推定残り時間（不確かな場合は非表示）
- VSS状態
- 中止可能／中止不可の表示

### 13.5 終了結果

成功時も単に「完了」とせず、以下を表示する。

- データコピー結果
- ハッシュ検証結果
- パーティション検証結果
- ブート構成結果
- MBR→GPT結果
- 次に行う操作
- コピー元を外して起動する指示
- ログ保存先

---

## 14. 安全機能

### 14.1 プリフライト

以下の全てが成功しなければ開始しない。

- 管理者権限
- コピー元・コピー先の一意識別
- コピー先容量
- セクターサイズ互換性
- BitLocker完全復号
- 基本ディスク
- 対応パーティション種別
- 対応ファイルシステム
- VSS Writer状態
- 保存先空き容量
- 保存先がコピー元ディスク上でないこと
- 保存先がコピー先ディスク上でないこと
- 電源状態（ノートPCはAC接続を推奨または必須設定）
- Windows Update／再起動保留状態の警告

### 14.2 コピー中止

- 読み取りエラー
- 書き込みエラー
- ディスク取り外し
- ディスク識別情報変化
- コピー先容量変化
- VSSスナップショット消失
- ハッシュ不一致
- 温度やSMART異常は警告とし、重大状態では停止可能な設計

### 14.3 電源断

初版は中断再開を保証しない。処理開始時にコピー先へ「未完了」マーカーを記録し、全検証・最終処理成功後にのみ「完了」へ更新する。未完了ディスクを起動可能として表示しない。

### 14.4 復元先の保護

復元先が現在起動中のシステムディスクである場合はWindows版で処理を禁止し、WinPEへ移行する。

---

## 15. セキュリティ要件

- 全てローカル処理とする。
- 初版はネットワーク通信を実装しない。
- 自動更新を実装しない。
- テレメトリを実装しない。
- ログへ回復キー、パスワード、ユーザー文書名を記録しない。
- シリアル番号は既定で部分マスクする。
- イメージパーサーは不正入力を前提に境界検査する。
- ファイルパスの正規化、長いパス、再解析ポイントを考慮する。
- 外部プロセス起動は絶対パス、署名確認、引数エスケープを必須とする。
- 一時ファイルはアクセス権を制限し、終了時に削除する。
- 管理者権限のプロセスへ一般ユーザーから任意コマンドを渡せるIPCを作らない。
- DLL検索パスを固定し、DLLプリロード攻撃を防止する。
- リリースビルドでControl Flow Guard、DEP、ASLR等の利用可能な保護を有効化する。

---

## 16. ログ・診断

### 16.1 ログレベル

- INFO
- WARNING
- ERROR
- DEBUG（開発版のみ既定有効）

### 16.2 記録内容

- アプリ／エンジンバージョン
- OS／WinPEバージョン
- ジョブID
- ディスク概要（シリアル部分マスク）
- プリフライト結果
- VSS Writer結果
- パーティション計画
- 各工程の開始・終了
- 読み書きエラー位置
- 外部ツールの署名・バージョン・終了コード
- ハッシュ検証結果
- ユーザーによるキャンセル

### 16.3 非記録項目

- ファイル名一覧
- 文書内容
- Windowsプロダクトキー
- BitLocker回復キー
- 保存パス内のユーザー名は可能な範囲でマスク

---

## 17. ライセンス適合要件

### 17.1 基本原則

本節は実装上の必須要件であり、説明資料ではない。違反する変更はマージ不可、リリース不可とする。

### 17.2 WinPE／ADK

一般公開パッケージに以下を含めない。

- WinPE ISO
- `boot.wim`、`winpe.wim`、`winre.wim`
- WinPE入りUSBイメージ
- ADKインストーラー
- ADKから抽出したMicrosoft製EXE／DLL／CAB
- `copype.cmd`、`MakeWinPEMedia.cmd`、`oscdimg.exe`等のコピー

メディア作成ウィザードは、利用者のローカルPCに正規にインストールされたADK／WinPEアドオンを検出して使用する。

- 利用者がMicrosoftの配布元から取得する。
- 製品がEULAへの同意を代行しない。
- インストーラーの無断再配布をしない。
- Microsoft公式ダウンロードページを案内する。
- 生成物は利用者自身のローカル出力として作成する。
- 自社サーバーやGitHub Releasesへ生成ISOをアップロードしない。

完成済みWinPEを一般配布する方針へ変更する場合は、Microsoftからの書面による再配布許諾または専門家の法的確認を取得するまでリリース停止とする。

### 17.3 Windows標準ツール

`mbr2gpt.exe`、`bcdboot.exe`、`dism.exe`、`diskpart.exe`、`reagentc.exe`等を製品へ同梱しない。利用者のWindows／WinPE内に存在する正規ファイルを検出して呼び出す。

### 17.4 Visual Studio／MSVC

- 開発組織はVisual Studio／Build Toolsの使用資格を確認する。
- Visual Studio Communityの利用可否を会社規模や用途を確認せず決めない。
- 再配布可能ランタイムはMicrosoftのREDIST一覧と使用中ライセンス条件を確認する。
- デバッグランタイムを配布しない。
- 再配布パッケージは改変しない。
- 開発に使用したツールチェーン版とライセンス証跡を記録する。

### 17.5 オープンソース

許可ライセンスの原則:

- MIT
- BSD-2-Clause
- BSD-3-Clause
- Apache-2.0（NOTICE等を含め条件確認）

禁止または個別審査:

- GPL全般
- AGPL
- SSPL
- LGPL
- MPL
- EPL
- Commons Clause
- Business Source License
- 独自非商用ライセンス
- ライセンス不明

依存追加PRには以下を必須とする。

- 名称
- バージョン
- 公式配布元
- ライセンス識別子
- ライセンス本文
- 用途
- 静的／動的リンク
- 製品配布物への含有有無
- 代替候補
- 人間の承認

### 17.6 Codex生成コード

OpenAIのサービス条件上、Codex等のコード生成出力が第三者ライセンスの対象となる場合があるため、生成コードも監査対象とする。

- 既存製品名をプロンプトに入れて模倣を依頼しない。
- 長い特徴的なコード断片を外部からコピーしない。
- 著作権表示を削除しない。
- 類似コード検出・秘密情報検出・依存監査を行う。
- 出所が説明できないコードは採用しない。

### 17.7 商標

- Microsoft、Windows、Windows PEのロゴを使用しない。
- Microsoft公式・公認と誤認させない。
- 「Windows 10／11対応」等の互換性説明に限定する。
- 競合製品の名称を製品名・画面・広告に使用しない。

### 17.8 Windowsライセンス注意文

製品内と利用規約へ以下の趣旨を明記する。

> 本ソフトウェアは、Windowsその他のソフトウェアに関するライセンスを付与または移転するものではありません。バックアップ、復元および別デバイスへの移行は、対象ソフトウェアの使用許諾条件に従い、適切にライセンスされた環境で実施してください。本ソフトウェアはライセンス認証の回避機能を提供しません。

### 17.9 リリース必須ファイル

- LICENSEまたは製品利用規約
- THIRD-PARTY-NOTICES.txt
- `licenses/`配下のライセンス本文
- SBOM.spdx.json
- 依存ライブラリ一覧
- プライバシーポリシー（通信なしであることを明記）
- WinPE非同梱の説明
- Windowsライセンス注意文

---

## 18. レスキューメディア作成ウィザード

### 18.1 検出

- ADK Deployment Toolsのインストール場所
- WinPEアドオンのアーキテクチャ別ファイル
- 必要コマンドの存在
- ファイルのMicrosoft署名
- 対応バージョン
- 出力先USB／ISOパス

### 18.2 作成手順

1. Microsoft公式ページへの案内
2. ADK／WinPEアドオン検出
3. 利用条件への同意は利用者がMicrosoftインストーラー上で行う
4. 作業用ディレクトリ作成
5. ローカルADKの標準スクリプト・ツールを呼び出す
6. 自作WinPEAppと必要な自作設定のみ追加
7. 必要なドライバを利用者が指定した場合のみ追加
8. USBまたはISOへ生成
9. 起動テスト手順を表示
10. 作業用ファイル削除

### 18.3 禁止動作

- ADKの自動ダウンロード・再配布
- EULA同意の自動化
- Microsoftファイルの製品キャッシュへの恒久保存
- 生成ISOのクラウドアップロード
- Microsoftロゴを使ったメディアラベル

---

## 19. テスト要件

### 19.1 単体テスト

- GPTヘッダー／エントリ解析
- MBR解析
- CRC計算
- GUID生成
- 容量・オフセット演算
- チャンクインデックス
- 圧縮／非圧縮判定
- SHA-256検証
- 不正イメージ境界検査
- ジョブJSON検証
- コマンド引数エスケープ
- ディスク安定識別

### 19.2 イメージファジング

- 長さオーバーフロー
- 末尾欠損
- 重複チャンク
- 重複オフセット
- 範囲外オフセット
- 巨大値
- 不正圧縮データ
- ハッシュ不一致
- 未知バージョン
- 循環参照に相当する不正インデックス

不正イメージでクラッシュ、任意コード実行、範囲外読み書きが発生してはならない。

### 19.3 仮想環境テスト

Hyper-V等で以下を反復テストする。

- Windows 10 MBR／BIOS
- Windows 10 GPT／UEFI
- Windows 11 GPT／UEFI
- MBR→GPT変換
- 同容量仮想ディスク
- 大容量仮想ディスク
- イメージ作成・破損検出・復元
- WinREパーティションあり／なし
- 複数回復パーティション

### 19.4 実機テスト

| ケース | 必須 |
|---|---:|
| SATA SSD→SATA SSD | 必須 |
| SATA HDD→SATA SSD | 必須 |
| NVMe SSD→NVMe SSD | 必須 |
| USBケース経由のコピー先 | 必須 |
| GPT→GPT | 必須 |
| MBR→MBR | 必須 |
| MBR→GPT | 必須 |
| Windows 10 | 必須 |
| Windows 11 | 必須 |
| コピー先が同容量 | 必須 |
| コピー先が大容量 | 必須 |
| BitLocker有効で開始拒否 | 必須 |
| 動的ディスクで開始拒否 | 必須 |
| 読み取りエラー | 必須 |
| イメージ破損 | 必須 |
| 電源断後の未完了判定 | 必須 |

### 19.5 起動受け入れ試験

- コピー元を物理的に外した状態で起動する。
- UEFIまたはBIOSの正しい方式で起動する。
- Windowsログオンまで到達する。
- システムボリュームが期待どおり認識される。
- イベントログに重大なディスク／NTFSエラーがない。
- `chkdsk /scan`が重大エラーを返さない。
- `reagentc /info`でWinRE状態を確認する。
- ディスクのパーティション配置を記録と比較する。
- コピー元とコピー先を同時接続しても署名衝突しない。

---

## 20. 受け入れ基準

### 20.1 機能受け入れ

- GPT→GPTクローン成功率がテストマトリクスで100%である。
- MBR→MBRクローン成功率がテストマトリクスで100%である。
- 対応条件を満たすMBR→GPTテストで検証・変換・起動に成功する。
- バックアップ作成後の自動検証が成功する。
- 1ビット破損させたイメージを確実に拒否する。
- Windows上のバックアップがVSSスナップショットから実行される。
- システムディスク復元がWindows上で実行されずWinPEへ誘導される。

### 20.2 安全受け入れ

- コピー元への書き込みAPI呼出しがコード監査で存在しない。
- コピー先の二段階確認を自動テストまたはUI試験で確認する。
- ディスク番号入れ替わり時に処理を中止する。
- BitLocker完全復号前は開始できない。
- 対象外構成を警告だけで続行しない。

### 20.3 ライセンス受け入れ

- 配布ZIP／インストーラーにWinPE、WIM、ADK、Microsoft標準ツールが含まれていない。
- 全依存関係がSBOMに記載されている。
- THIRD-PARTY-NOTICESとライセンス本文が一致する。
- 禁止ライセンスが検出されない。
- Codexが追加した依存関係に承認記録がある。
- Microsoftロゴ、競合製品ロゴ、模倣画面がない。

---

## 21. 開発フェーズ

### Phase 0: リポジトリ基盤・ライセンスゲート

- CMake構成
- コーディング規約
- CI
- 静的解析
- ライセンススキャン
- SBOM生成
- ログ基盤
- エラー型
- ディスク操作を行わないRead-only CLI

完了条件: 物理ディスクとパーティションを安全に列挙し、JSON診断を出力できる。

### Phase 1: WinPE GPT→GPTオフラインクローン

- GPT解析・作成
- NTFS使用クラスタ読取
- FAT32／回復パーティションコピー
- コピー先GUID再生成
- BCDBootによるUEFI起動修復
- 検証

完了条件: 仮想環境と実機でコピー元を外して起動できる。

### Phase 2: イメージ保存・復元

- `.dcimg` v1
- Zstandard圧縮
- SHA-256
- 全体検証
- WinPE復元
- 破損拒否

完了条件: バックアップ→別ディスク復元→起動まで成功する。

### Phase 3: MBR→MBR

- MBR解析・作成
- ディスク署名再生成
- BIOS起動修復
- Activeパーティション処理

完了条件: BIOS仮想環境と実機で起動する。

### Phase 4: MBR→GPT

- WinPEジョブ
- MBR2GPT署名・パス検証
- validate／convert
- BCDBoot UEFI
- 失敗処理

完了条件: 対応条件内の実機でUEFI起動する。

### Phase 5: Windows VSSイメージ作成

- VSS Requester
- Writer検証
- Snapshot device読取
- Windows版GUI

完了条件: 稼働中Windowsから一貫性のあるイメージを作成し、WinPEで復元・起動できる。

### Phase 6: Windows直接クローン・WinPE引継ぎ

- Windows上GPT→GPT／MBR→MBR
- PEジョブ引継ぎ
- 復元予約
- 自動再識別

完了条件: Windows版で開始したジョブが安全にPEで完了する。

### Phase 7: メディア作成・公開品質

- ADK検出
- 利用者側WinPE生成
- インストーラー
- マニュアル
- アクセシビリティ
- 署名準備
- リリース監査

完了条件: 配布物監査に合格し、WinPEを一切含まずに一般公開できる。

---

## 22. Codex作業規則

Codexは各タスク開始時に次を実行する。

1. 本仕様書と`AGENTS.md`を読む。
2. 変更対象と非対象を明示する。
3. 追加依存が必要か判定する。
4. 破壊的ディスクI/Oが含まれる場合、まずモック／仮想ディスクで実装する。
5. テストを先に追加または同時追加する。
6. 実装後にビルド、単体テスト、静的解析、ライセンススキャンを実行する。
7. 実機でしか検証できない項目を明記する。

### 22.1 Codexが人間へ確認すべき変更

- 新しい外部依存
- ライセンス条件の変更
- WinPE／ADK配布方式の変更
- 対応ファイルシステム追加
- 小容量ディスク対応
- BitLocker対応
- ネットワーク機能
- テレメトリ
- カーネルドライバ
- 独自MBR→GPT変換
- イメージ形式のメジャーバージョン変更

### 22.2 Codexが確認なしで変更してよい範囲

- 単体テスト追加
- ドキュメント改善
- ログ改善（機密情報を増やさない）
- エラーメッセージ明確化
- 内部リファクタリング
- 警告修正
- 性能改善（安全性・形式互換性を変えない）

---

## 23. Codexへ最初に渡すプロンプト

以下をCodexへそのまま渡す。

```text
このリポジトリでは、添付の「Windowsディスククローン・イメージバックアップツール 開発仕様書」を最上位要件として扱ってください。

最初からクローン機能を実装せず、Phase 0のみを実施してください。

Phase 0の作業内容:
1. C++20 / CMake / MSVC x64のリポジトリ構成を作成する。
2. WindowsApp、WinPEApp、CloneCore、DiskModel、ImageFormat、VssRequester、BootRepair、MediaBuilder、CliTools、tests、docsの空プロジェクトまたはライブラリを作成する。
3. 物理ディスクとパーティションを読み取り専用で列挙するCLIを実装する。
4. ディスク番号、モデル、容量、論理/物理セクターサイズ、BusType、シリアル末尾、GPT/MBR、パーティション一覧をJSONと人間向けテキストで出力する。
5. コピー元・コピー先への書き込み処理は一切実装しない。
6. エラー型、ログ基盤、RAIIハンドルラッパーを実装する。
7. 単体テストと、実ディスクを使わないモックテストを追加する。
8. THIRD-PARTY-NOTICES.txt、licenses/、SBOM生成用の基盤を作成する。
9. GPL、AGPL、SSPL、LGPL、MPL、ライセンス不明の依存を追加しない。
10. 新しい外部依存を追加する前に理由、ライセンス、代替案を提示し、実装を止める。
11. ビルド、テスト、静的解析結果を報告する。

重要:
- Microsoft製EXE、DLL、WIM、ISO、ADKファイルをリポジトリへコピーしないでください。
- Acronis、HD革命、Macrium等の実装やUIを模倣しないでください。
- 管理者権限がない場合の読み取り専用診断も、可能な範囲で安全に失敗させてください。
- 不明点があっても仕様を拡大解釈せず、安全側に実装してください。

作業完了後、変更ファイル、設計判断、未検証事項、次のPhase 1に入る前の確認事項をまとめてください。
```

---

## 24. AGENTS.mdに記載する短縮ルール

```text
# Project Safety and Licensing Rules

- The specification document is the highest-priority project requirement.
- Never write to a source disk.
- Never implement destructive disk I/O without explicit task scope and tests.
- Fail closed on unknown disk layouts, filesystems, encryption, or identifiers.
- Do not add kernel drivers, telemetry, networking, license-bypass, or BitLocker-bypass features.
- Do not copy or redistribute Microsoft WinPE/ADK/WIM/ISO/EXE/DLL files.
- Rescue media must be built locally from the user's installed ADK/WinPE add-on.
- Do not add GPL, AGPL, SSPL, LGPL, MPL, Commons Clause, BSL, or unknown-license dependencies.
- Any new dependency requires human approval and license documentation.
- Do not imitate proprietary cloning products or copy third-party code without provenance.
- Maintain THIRD-PARTY-NOTICES.txt and SBOM.spdx.json.
- Use stable disk identity checks; never trust disk number alone.
- All image inputs are untrusted and require bounds checking.
- Run build, tests, static analysis, and license checks before reporting completion.
```

---

## 25. リリース判定チェックリスト

### 機能

- [ ] GPT→GPTクローン実機合格
- [ ] MBR→MBRクローン実機合格
- [ ] MBR→GPT実機合格
- [ ] Windows VSSバックアップ合格
- [ ] WinPE復元合格
- [ ] 同容量ディスク合格
- [ ] 大容量ディスク合格
- [ ] イメージ破損拒否合格

### 安全

- [ ] コピー元書き込み経路なし
- [ ] 二段階消去確認
- [ ] ディスク再識別
- [ ] BitLocker拒否
- [ ] 動的ディスク拒否
- [ ] セクターサイズ不一致拒否
- [ ] 未完了マーカー
- [ ] エラー時に成功表示しない

### ライセンス

- [ ] WinPE ISOなし
- [ ] WIMなし
- [ ] ADKファイルなし
- [ ] Microsoft標準EXE／DLLなし
- [ ] 禁止ライセンスなし
- [ ] THIRD-PARTY-NOTICES最新
- [ ] SBOM最新
- [ ] 依存承認記録あり
- [ ] Visual Studio／MSVC使用資格確認
- [ ] Windowsライセンス注意文あり
- [ ] Microsoft／競合ロゴなし

### 公開

- [ ] 利用規約
- [ ] プライバシーポリシー
- [ ] 操作マニュアル
- [ ] 既知の制限
- [ ] 復旧不能時の免責とサポート範囲
- [ ] セキュリティ報告窓口
- [ ] バージョン情報
- [ ] 配布物ハッシュ

---

## 26. リリース停止条件

以下のいずれかに該当する場合は一般公開しない。

1. 完成済みWinPE、WIM、ADK由来ファイルが含まれる。
2. Microsoft標準ツールを自社配布している。
3. 依存ライセンスが不明または禁止方針に該当する。
4. SBOMと実際の配布物が一致しない。
5. コピー元への書き込み可能性が残る。
6. 誤ディスク消去を防ぐ再識別がない。
7. 破損イメージを復元できてしまう。
8. MBR→GPT失敗を成功扱いにする。
9. 実機でコピー元を外した起動試験を行っていない。
10. Windows／競合製品のライセンス認証回避を示唆する説明がある。
11. Visual Studio／MSVCの使用資格が確認できない。
12. 重大なセキュリティ警告・静的解析エラーが未解決である。

---

## 27. 参考資料（2026年7月29日確認）

本節のURLは実装根拠およびライセンス再確認用である。MicrosoftやOpenAIの条件は変更され得るため、公開直前に再確認する。

1. Microsoft Learn, Download and install the Windows ADK  
   https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-install
2. Microsoft Learn, Download WinPE  
   https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/download-winpe--windows-pe?view=windows-11
3. Microsoft Learn, Volume Shadow Copy Service  
   https://learn.microsoft.com/en-us/windows-server/storage/file-server/volume-shadow-copy-service
4. Microsoft Learn, VSS Requesters  
   https://learn.microsoft.com/en-us/windows/win32/vss/requestors
5. Microsoft Learn, FSCTL_GET_VOLUME_BITMAP  
   https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_volume_bitmap
6. Microsoft Learn, MBR2GPT  
   https://learn.microsoft.com/en-us/windows/deployment/mbr-to-gpt
7. Microsoft Learn, BCDBoot command-line options  
   https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/bcdboot-command-line-options-techref-di?view=windows-11
8. Microsoft Learn, Visual Studio 2022 Redistribution  
   https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution
9. Microsoft Learn, Latest supported Visual C++ Redistributable downloads  
   https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170
10. Visual Studio Community license information  
    https://visualstudio.microsoft.com/vs/community/
11. Zstandard reference implementation and license  
    https://github.com/facebook/zstd
12. OpenAI Service Terms - Codex and code generation  
    https://openai.com/ja-JP/policies/service-terms/

### 27.1 法的注意

本文書は公開情報に基づく技術・運用上の安全方針であり、弁護士の法律意見またはMicrosoft、OpenAIその他権利者による正式な許諾ではない。一般公開前に、実際に使用するADK／WinPE／Visual Studioのライセンス条項、組織の契約、配布地域、販売形態を再確認すること。

---

## 28. 変更管理

本仕様の以下の項目を変更する場合、版数を更新し、人間の承認を得る。

- 対応OS
- 対応ファイルシステム
- ディスク容量条件
- BitLocker方針
- MBR→GPT方式
- WinPE配布方式
- 外部依存方針
- イメージ形式
- ネットワーク・テレメトリ
- カーネルドライバ
- リリース停止条件

変更履歴には、変更理由、影響範囲、ライセンス影響、移行方法、テスト追加を記録する。
