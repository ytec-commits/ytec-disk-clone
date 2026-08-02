# Packaging

ポータブル配布物は、静的MSVC x64でビルドしたY-TEC製EXE、媒体作成
スクリプト、説明、ライセンス表示、SBOM、SHA-256一覧だけを含みます。
Microsoft製EXE、DLL、CAB、WIM、ISO、ADK/WinPE本体は同梱しません。

Phase 7の製品WinPE媒体は利用者PCへ導入済みのADK/WinPE Add-onから
リポジトリ外へ生成します。自作の静的`ytec-winpe-app.exe`と
`ytec-winpe-gui.exe`だけをWIMへ追加し、日本語表示は同じローカルADKの
`WinPE-FontSupport-JA-JP.cab`をDISMで適用します。CAB、WIM、ISO、
ADKツールを配布ZIPまたはリポジトリへ含めません。

リポジトリ外の新規パスへ配布候補を作る例:

```powershell
./scripts/New-PortablePackage.ps1 `
  -OutputRoot C:\TsumugiRelease\Y-TEC-Tsumugi-Drive-0.2.0 `
  -BuildPackage
```

既存フォルダー/ZIP、リポジトリ内、ドライブ直下、reparse pointを拒否します。
生成物は次の実体監査を通してから扱います。

```powershell
./scripts/Test-PortablePackageArtifact.ps1 `
  -PackageRoot C:\TsumugiRelease\Y-TEC-Tsumugi-Drive-0.2.0 `
  -ZipPath C:\TsumugiRelease\Y-TEC-Tsumugi-Drive-0.2.0.zip
```

CIは一意な一時フォルダーで実ZIPを作成し、日本語ファイル名を保持する
UTF-8の`SHA256SUMS.txt`、全ハッシュ、ZIP内の正確な15ファイル、
Microsoft媒体/ツール名の不在を検証してから、その一時成果物だけを削除します。
