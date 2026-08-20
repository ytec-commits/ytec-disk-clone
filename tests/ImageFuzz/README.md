# ImageFuzz

現在の製品形式である `.tsumugi` v1、その認証対象manifest、埋込みpartition snapshotを対象にした、依存追加なしの決定論的ファズ・スモークです。

`ytec-image-fuzz-tests` は有効な合成seedと不正・境界入力をメモリ上だけで変異させ、各inspect APIへ渡します。manifestとpartition snapshotが受理された場合は、再エンコード結果が入力と完全一致することも検査します。

実行境界は次のとおり固定しています。

- seed: `0x5954454346555A5A`
- 変異: 4,096回（合成seed 8件を含めて合計4,104件）
- 1入力の上限: 64 KiB
- CTest timeout: 120秒
- ファイル、物理ディスク、USB、VM、ネットワークへのI/O: なし

通常版では次を実行します。

```powershell
. .\scripts\Enter-MsvcEnvironment.ps1
cmake --preset msvc-x64
cmake --build --preset msvc-x64 --target ytec-image-fuzz-tests
ctest --test-dir out\build\msvc-x64 -R '^ytec-image-fuzz-tests$' --output-on-failure
```

AddressSanitizer版ではpresetを `msvc-x64-asan` に置き換えます。これは長時間のcoverage-guided fuzzingやlibFuzzerの代替ではなく、CTest、`/analyze /WX`、AddressSanitizerで毎回再現できる最小基盤です。旧 `.dcimg` / `.dcmig` 形式は製品経路から隔離されているため対象外です。
