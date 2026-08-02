# ライセンス管理

## 現在の開発ツリーの状態

- 製品コードの外部ライブラリ依存: なし
- 再配布する第三者資産: LINE Seed JP `LINESeedJP_20241105` Regular/Bold
  （OFL-1.1、Windows版/WinPE版GUIへ埋込み）
- テストフレームワーク依存: なし（小さな自作テストランナーを使用）
- Microsoft 製 EXE / DLL / WIM / ISO / CAB の同梱: なし
- Windows SDK の公開ヘッダーとインポートライブラリ: ビルド時のみ使用し、リポジトリへコピーしない

プロジェクト自身の配布ライセンスは未決定です。一般公開または第三者配布の前に、人間が利用規約または `LICENSE` を決定してください。現在のポータブルZIPは開発用監査成果物であり、正式配布許諾を示すものではありません。

## 依存追加ゲート

新しい依存を追加する前に実装を止め、名称、バージョン、公式配布元、ライセンス識別子と本文、用途、リンク方式、配布物への含有、代替案、人間の承認を記録します。承認後に `third_party/dependencies.json`、`licenses/`、`THIRD-PARTY-NOTICES.txt`、SBOM を同時更新します。

`scripts/check-licenses.ps1` は依存台帳に禁止または不明ライセンスがないことを検査します。

## 承認済み依存: LINE Seed JP

- 理由: Windows版とWinPE版で読みやすく一貫した日本語UIを提供する
- 版: `LINESeedJP_20241105`
- 公式配布元: <https://seed.line.me/index_jp.html>
- ライセンス: SIL Open Font License 1.1 (`OFL-1.1`)
- 収録: 未改変のApp TTF Regular/Boldだけを各GUI実行ファイルへ埋込み
- 読込み方式: `AddFontMemResourceEx`によるプロセス内限定。OSへインストールしない
- 代替案: Windows/WinPE標準の`Yu Gothic UI`を継続する、または見出しだけに限定する
- 採用判断: 利用者が2026-08-01にWindows版・WinPE版の両方への追加を承認
- 失敗時: 両ウェイトを解除し、`Yu Gothic UI`へフォールバック

公式ZIP、収録ファイル、OFL本文のSHA-256は
`third_party/line-seed-jp/README.md`、依存台帳は
`third_party/dependencies.json`、配布通知は`THIRD-PARTY-NOTICES.txt`へ記録します。

## Windows ライセンス注意

本ソフトウェアは、Windows その他のソフトウェアに関するライセンスを付与または移転するものではありません。バックアップ、復元および別デバイスへの移行は、対象ソフトウェアの使用許諾条件に従い、適切にライセンスされた環境で実施してください。本ソフトウェアはライセンス認証の回避機能を提供しません。
