# iivnc-server の作業方針

## リリース運用

**手順は共通の `~/.claude/CLAUDE.md`「修正が終わったら、リリースまで通す」に従う。**
ここにはこのリポジトリ固有の事情だけを書く。

- リモート: `https://github.com/iosxi/iivnc-server.git`(`iosxi/iivnc-server`)
- ブランチ: **`master`**
- 最新バージョンの確認: `git tag --sort=-v:refname | head -1`
- リリースの添付物: **`iivnc-server.exe`**。改名せず、そのまま `gh release create` に渡す。
- バージョン: タグの `vN` とは別に、`src/iivnc-server.rc` の VERSIONINFO、
  `src/iivnc-server.manifest` の `assemblyIdentity`、`src/iivnc.h` の `APP_VERSION` / `APP_VERSION_A` がある。
  機能が変わったら全部上げる。

### exe を変更したとき

ソースを直したら **`build.bat` で exe を作り直してからコミットする**。exe はリポジトリに追跡させている。
アイコンは `python tools/make-icon.py`(`src/iivnc-server.ico` と接続中の `-active.ico`)。

### iivnc-client と同じファイル

`src/zlite.h` `src/zdeflate.c` `src/zinflate.c` `src/vncdes.c` `src/vncdes.h` `src/theme.c`(theme.c は
先頭の `#include` だけ違う)は `../iivnc-client/src` と中身をそろえる。片方を直したらもう片方へ写し、
`diff` で確かめる。`theme.c` は kotemado のものを持ってきた。

## 動作確認について

**利用者の画面を写さず、入力も再現しない検証用の動かし方がある。** `-testsrc` は合成した絵
(1920×1080。スクロールする文字の欄、動く四角、写真のような欄)を出し、入力はログに書くだけ
(`-dryrun` と同じ)。検証用の ini(`build/test/test.ini`、127.0.0.1:5999)で動かすので、
利用者が普段使っている iivnc-server(既定の ini)とは干渉しない(多重起動の判定は ini ごと)。

- **自動検証は `python tools/test.py`。** 独立に書いた Python の受け手(`tools/rfbcheck.py`)で、
  認証、止まった絵(各エンコーディングを Raw の絵と画素単位で比べる)、動く絵 300 フレーム、
  途中の画面の大きさの変更(`-testresize`)、3 人同時、でたらめなデータ 200 回を確かめる。
- 入力の再現は `python tools/inputcheck.py`(ログの `[dryrun]` 行を期待値と比べる)。
- 共通部品: `tools/build-tools.bat` の後、`python tools/test_zlite.py`(本家 zlib と相互に)、
  `python tools/fuzz_zlite.py`、`python tools/test_jpeg.py`(PIL と比べ、WIC で警告が出ないこと)、
  `build/tools/test_des.exe`(DES の既知の値)。
- 速さは `python tools/bench.py --port <p>`(受け手は復号せず読み飛ばす)と、ログの 5 秒ごとの集計
  (`回/秒、1 回 符号化 Xms(送信まで Yms)`)。合成の絵の速さを上げるには `-testfps 0`、
  全面が毎フレーム変わる絵は `-testsrc video`。
- 実画面(DXGI)で測るときは `-dryrun` を付け、`listen=127.0.0.1` の ini で動かす。
  **iivnc-client の窓に実画面を映すと合わせ鏡になり、変化が止まらない**(CPU やメモリの測定が無意味になる)。
  受け手は Python(`bench.py`)にする。
- `-exit` は相手のプロセスが終わるまで待つ。終わらないうちに同じ ini で起動すると、新しい方は
  古い方へ「設定画面を開け」と頼んで終わる(検証の取り違えの元。2026-10-04 に一度起きた)。
- 画面の見た目は、その窓だけを撮る。撮影するプロセスは DPI 対応にする(しないと窓の一部しか撮れない)。
  PrintWindow はライト表示で下半分を描き落とすことがある(kotemado と同じ)ので、
  `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)` の矩形を `CopyFromScreen` で撮る。
- テキストの改行は LF。`.bat` だけ CRLF(goto のラベルを見失わないため。`.gitattributes` で固定)。
- PowerShell 5.1 は BOM なし UTF-8 を読み違えるので、日本語を含む `.ps1` は BOM 付きで保存する。

### 他社のビューアでの確認

TigerVNC 1.16.2 の Windows 版ビューア(署名付きの単体 exe。SourceForge の `vncviewer64-1.16.2.exe`)で、
VNC 認証・Tight + JPEG・CopyRect・カーソル・QEMU キー・拡張クリップボードの取り決めまで通り、
合成の絵が正しく描けることを確かめた(2026-10-04)。`-passwd <file>` に vncpasswd 形式(8 バイト)を渡せる。

### 実測で分かったこと(2026-10-04、i7-12700KF・RTX 3070 Ti・LG 4K 1 枚・拡大率 125%)

- **DXGI の複製を始めた直後の 1 枚目は中身が無い**(`LastPresentTime=0`、`AccumulatedFrames=0`、
  変化の情報 0 バイト)。それを写すと真っ黒な絵を送っていた。画面が止まっていれば 2 枚目も来ない。
  v1 から、開いた直後の絵とカーソルは GDI で撮って入れる(`gdi_seed`)。直した後、DXGI で取り込んだ絵は
  同時に GDI で撮った画面と 99.95% の画素で一致(残りは 2 つを撮る間の変化)。
- `DuplicateOutput1`(BGRA8 だけを指定)で、この PC の 4K(HDR 対応のモニター)を BGRA8 で取り込めた。
  HDR を有効にしたときの色は確かめていない。
- 4K 全体 1 枚の符号化: Tight 劣化なし 4.5ms、JPEG 95・4:4:4 9.1ms、JPEG 80・4:2:0 5.9ms、ZRLE 3.7ms
  (この PC の暗い単純な画面で)。
- 自前 deflate レベル 1: 642MB/s・6.7%、本家(Python の zlib-ng 1.3.1)レベル 1: 1166MB/s・9.9%(同じ絵)。
- 自前 JPEG は libjpeg(PIL)と同じ品質で大きさ・PSNR がほぼ同じ。1920×1080 を 1 スレッドで約 28ms。
  JPEG の終わりの端数ビットの埋め方を誤っていた(幅より広い 0x7F を入れていた)のを直した。
  WIC で展開すると「Corrupt JPEG data: premature end of data segment」が出る(60 枚中 41 枚)。PIL は黙って読む。
- 接続中で画面が止まっていれば、10 秒間の CPU 時間は 0ms(計測の最小単位未満)。
  専用メモリ: 1080p の合成の絵で 49MB、4K 実画面で全体の更新を続けた後 165MB(作業領域を手放すようにする前は 224MB)。
- 画面の大きさが変わる前に受けた「要求の範囲」を、縮んだ後の変化の表に当てて範囲外を読み、落ちていた
  (`-testresize` で見つけた)。要求の範囲は書き手のスレッドで今の画面に切り詰める。

### 確かめていないこと

- 2 台の PC の間で実際に使うこと(LAN 越し、ファイアウォールの確認画面、相手の IME の切り替わり)。
- 実際のキー・マウスの再現(検証は `-dryrun` のログまで。利用者の画面を動かすことになるため)。
- 複数のディスプレイ、回転した画面、HDR を有効にした画面、拡大率の違う画面。
- 管理者として動かしたときの、管理者のウィンドウへの入力(input-mouser では実測で届いた)。
- RealVNC・UltraVNC などほかのビューア。
