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
`src/fwrules.c`(ファイアウォールの、この exe の規則を数える・消す)も同じく先頭の `#include` だけ違う。
`src/filexfer.c`(ファイルのコピー＆貼り付け)も同じく先頭の `#include` だけ違う。

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

### サービス(v2、svc.c)

構成は 4 つの動き方(同じ exe): `-service`(SCM、セッション 0、SYSTEM)が、コンソールのセッションへ
`-agent`(SYSTEM。取り込み・通信の本体)を `CreateProcessAsUser`(自分のトークンを複製して
TokenSessionId を書き換え)で起動し、ログインしたユーザーには `-tray`(ユーザーの権限、
`WTSQueryUserToken`)を起動する。設定は `-svcsettings`(管理者)。状態は共有メモリ `Global\iivnc-server-status`。

- **検証のときは利用者の ini を使わず `build/svctest/svc.ini`(ポート 5999・パスワード s3cret)で登録する。**
  `svc_installed()` は「この ini で登録されているか」を見るので、利用者のトレイ常駐(既定の ini)とは干渉しない。
  サービス名は 1 つ(`iivnc-server`)なので、検証の後は必ず `-uninstall-service` で消す。
- 登録・解除: `Start-Process iivnc-server.exe -ArgumentList '-install-service','-ini','"<ini>"' -Verb RunAs -Wait -PassThru`。
  この PC の UAC は確認なしで昇格する。
- **サービスが動いている間は exe を上書きできない。** ビルドの前に管理者で `sc stop iivnc-server`、
  トレイ(ユーザーの権限)は `taskkill /F /IM iivnc-server.exe`。ビルドの後に `sc start`。
- 管理者のプロセスのコマンドラインは一般の権限から読めない(Win32_Process の CommandLine が空)。
  設定画面(管理者)は窓(`#32770`、題「iivnc-server」)から探し、止めるのも管理者の taskkill で。
- 設定画面のボタンからの登録は、ボタンへ `WM_COMMAND`(IDC_SERVICE = 1020)、確認の画面(タスク ダイアログ)へ
  `TDM_CLICK_BUTTON`(WM_USER+102、IDYES)を送って通した。
- `python tools/svccheck.py [--lock]`: 取り込みと GDI の一致、Ctrl+Alt+Del(SendSAS)で安全なデスクトップを
  取り込めて Esc で戻ること、ロック。**--lock は画面をロックするので、利用者に解除してもらうことになる。**
- 再起動の検証: ini に `selftest=1` を書くと、分身が起動 20 秒後に入力デスクトップを撮って
  `[selftest] 入力デスクトップ …、平均の明るさ …、ログインしているユーザー …` をログに書く。

実測で分かったこと(2026-10-04):

- **Windows 11 の Ctrl+Alt+Del の画面は、黒い背景に白い文字のメニューだけ。** 平均の明るさ 0.1 でも
  取り込めている(明るい画素 0.1〜0.4%)。一度「真っ黒 = 取り込めていない」と誤って見立て、GDI に切り替える
  修正を入れかけた。絵を見て誤りと分かり、戻した。DXGI のままで、メニューも右下のアイコンも取り込める。
- 安全なデスクトップへ切り替わったとき、DXGI は ACCESS_LOST(0x887A0026)を返す。入力デスクトップへ
  スレッドを移して開き直せば取り込める。入力(Esc)も Winlogon デスクトップで効き、元の画面へ戻る。
- **Windows 10/11 のロック画面(時計の画面)は Default デスクトップ上のアプリ**(LockApp)。キーを押して出る
  資格情報の画面から Winlogon デスクトップになる。
- 登録前の SoftwareSASGeneration は無かった。登録で 1、解除で値ごと消えて元どおり。ファイアウォールの規則も消える。
- 解除すると、トレイ(ユーザーの権限)が 3 秒以内に気づき、ふだんのトレイ常駐で起動し直す。

### ファイルのコピー＆貼り付け(2026-10-05、server v6 / client v8、filexfer.c)

利用者の要望: input-mouser のファイルのコピー＆貼り付けを iivnc にも。決めたこと: コピー＆貼り付け(エクスプローラー)、
両方向、サービスでも(ログイン後)、見るだけの接続では使わない。

- 仕組みは input-mouser の filecopy.c と同じ(一覧だけ先に、中身は貼り付けたときに 512KB ずつ・6 つ先読み。
  貼り付ける側は CFSTR_FILEDESCRIPTORW / CFSTR_FILECONTENTS の IDataObject を専用の STA スレッドで OleSetClipboard)。
  違い: 運ぶのは VNC の接続の中の **メッセージ 105**(両向き、`[105][sub][0][0][u32 長さ BE][中身]`。
  sub 1 HELLO・2 FILES・3 READ・4 DATA。中身はリトル エンディアンで input-mouser と同じ形)。
  クライアントは疑似エンコーディング `0x69467831` を送り、サーバーはそれを見て HELLO(`u32 1 = 使える`、
  見るだけなら 0)を返す。iivnc 以外の相手には 105 を送らない。1 つの一覧は複数の相手に渡せる(最大 8)。
- サーバーの送信: FX のメッセージは接続ごとの待ち行列に積み、書き手のスレッドが画面の合間に送る
  (`g_scr.lock` を持ったまま送ると取り込みが止まるため)。クライアントは `g_sendCs` の中で頭と中身を続けて送る。
- 貼り付け先の外へ出る名前(絶対パス、ドライブ、`.`/`..` の区切り)の一覧は受け付けない。`a..b.txt` は通す。
- 1 回のコピーでクリップボードの変更の知らせが 2 回来ることがあり、一覧が 2 回送られた。同じ相手へ同じ一覧を
  2 秒以内に重ねて送らない。
- **サービスの分身(SYSTEM)からは、利用者がコピーしたものが見えない**(同じクリップボードの番号なのに、分身からは
  形式 0 個・利用者からは 5 個。WinSta0\Default。利用者になりすましても 0 個。2026-10-05 実測)。置くことはできる。
  → サービスのトレイ(利用者の権限)がクリップボードを見張り、ファイルの一覧(`CD_CLIP_FILES`)と文字(`CD_CLIP_TEXT`)を
  `WM_COPYDATA` で分身へ渡す(分身は `ChangeWindowMessageFilterEx` で受け付ける)。**文字のクリップボードも、
  これまでサービスのときはサーバー → ビューアへ届いていなかった**(同じ理由)のが、これで届くようになった
  (rfbcheck の受け手で `svc-text-123` が届くのを確認)。「通知領域から消す」はアイコンだけ消してプロセスは残す。
- 分身がファイルを開く・フォルダをたどるときは、コンソールの利用者(`WTSQueryUserToken`)になりすます
  (利用者が読めないファイルを SYSTEM の権限で外へ出さないため)。
- 分身が置いたファイルを利用者が貼り付けると、**COM の呼び出しが SYSTEM のプロセスへ来て断られる**
  (`DV_E_FORMATETC`、1 件も貼れない。実測)。分身は最初に `CoInitializeSecurity` で、対話ログオンの利用者(IU)と
  中位の整合性レベル(`S:(ML;;NX;;;ME)`)からの呼び出しを受け付ける(`agent_com_security`)。
  記述子は**絶対形式**にする(文字列から作った自己相対形式だと `0x80070551`)。
- 検証: `python tools/fxcheck.py [c2s|s2c|viewonly|all]`(ポート 5993)。送る側だけが「つながったら今の
  クリップボードのファイルを渡す」(サーバーは ini の `fxoffer=1`、クライアントは `-fxoffer`。1 台なのでクリップボードは
  1 つ)。貼り付けは `tools/pastetest.exe`(`tools/build-tools.bat` で作る。貼り付け先フォルダの IDropTarget へ落とす =
  エクスプローラーの貼り付けと同じ)。**PowerShell の `Shell.Application` の `InvokeVerb("Paste")` は、ふつうのファイルでも
  何も貼り付けなかった**。`IFileOperation::CopyItems` にクリップボードのデータを渡すと `DV_E_FORMATETC`。
  サービス: `build/svctest/fx.ini`(ポート 5992、127.0.0.1)で登録し `--service c2s|s2c`(s2c はクライアントを
  `-fxnowatch` にして、つないだあとでコピーする)。
- 実測(この PC、127.0.0.1): 5 ファイル+2 フォルダ(50MB・0 バイト・日本語名・入れ子)が c2s・s2c とも SHA-256 で一致、
  0.3〜0.7 秒。サービスでも両方向一致。見るだけ(クライアント側・サーバー側)では一覧が渡らない。
- **登録したサービスの ini を試験の途中で書き直すと、`[service] sas_before` の控えが消え、解除しても
  `SoftwareSASGeneration` が戻らない**(実際に 1 のまま残った。登録前は無かったので手で消した)。登録中は ini を
  書き直さない(書くなら [service] を残す)。
- exe: server 395KB(+21KB)、client 318KB(+19KB)。

### サービスのトレイを左クリックすると設定画面が 2 枚開く、という報告(2026-10-04、v5)

- 原因: トレイは `NOTIFYICON_VERSION_4` にしてあり、左クリック 1 回で `WM_LBUTTONUP` と `NIN_SELECT` の
  **両方**が来る(右クリックは `WM_RBUTTONUP` と `WM_CONTEXTMENU`)。サービスのトレイは両方で管理者の
  `-svcsettings` を起動していたので、別プロセスが 2 つ立った。ふだんのトレイ常駐は同じプロセスの 1 枚を
  使い回すので 1 枚で済んでいた。
- 対策: 新しい形式にできたら `NIN_SELECT` / `WM_CONTEXTMENU` だけで動く(できなければ従来のマウスの通知)。
  `-svcsettings` は名前付きミューテックス `Local\iivnc-server-svcsettings` で 1 つだけにし、2 つ目は
  開いている画面を手前に出して終わる。
- `python tools/traycheck.py`: `-tray` を起動し、シェルと同じ通知(`WM_LBUTTONUP` → `NIN_SELECT`)を
  クリック 2 回分送って、設定画面の枚数を数える。v4 では 4 枚、直したあとは 1 枚(1 回だけでも 1 枚)。
  サービスは登録しない(`-tray` は 3 秒ごとに登録を確かめ、無ければふだんの常駐で動き直すので、その前に送る)。
  ふだんの常駐の窓は `iivnc.Server.Tray`(同じプロセスに IME の窓もあるので、クラス名で選ぶ)。

### マウスが無い PC でカーソルが見えない、という報告(2026-10-04、v4)

利用者の報告: Windows 10 のマウスをつないでいない PC をサーバーにすると、ビューアでカーソルが見えない。

- Windows はマウスが無いとカーソルを隠す(`GetCursorInfo` に `CURSOR_SHOWING` が無い、DXGI の
  `PointerPosition.Visible` が FALSE、DXGI は形を渡さない)。サーバーはそれをそのまま「透明なカーソル」として
  送っていた(`-testcursor hidden` + `showcursor=0` で 1×1・見える画素 0 が届くのを確認 = 前の版と同じ形)。
  クライアントは透明な形を受けるとカーソルを消す(形を一度も受けていなければ点を出す)。
- 対策 `showcursor=1`(既定): 隠れていても見えるものとして送る。形は `GetCursorInfo` の hCursor から追い、
  無ければ標準の矢印(`cursor_follow_hidden`、`cursor_ensure_shape`)。DXGI の形が来たら `g_lastCursor` を戻す。
- `python tools/cursorcheck.py`: 見える / 隠れている(形あり)/ 隠れている(形なし)× showcursor 0・1 の 6 通り。
- `log=1` で、起動時にマウスの装置の一覧・`SM_MOUSEPRESENT`・カーソルの flags を、見え方が変わるたびに
  「カーソル: Windows が隠している」を書く。この PC(マウスあり)では装置 6 個、flags 1。
- **マウスが無い PC での実測はしていない**(この PC のマウスを外せない)。隠れている間に `GetCursorInfo` が
  形を返すか、DXGI の取り込みが続くかは、利用者の Windows 10 の記録で確かめる。

### ネットワークの許可(fwrules.c、2026-10-04)

- Windows が確認の画面で作る規則は、レジストリ上の名前が `TCP Query User{GUID}<exe のパス>` だが、
  **COM(INetFwRule)から見える Name は表示名(FileDescription)** で、TCP と UDP、別の場所の同じ exe の規則とも同じ。
  `INetFwRules::Remove` は名前で消すので、**消す規則だけを一意な名前に付け替えてから消す**
  (仮の規則で実測: 同じ名前の A・B のうち A だけを付け替えて消し、B が残った)。
- 読むのは一般の権限で読めた(COM の Rules を列挙)。消すのは管理者が要るので、自分を `-remove-firewall` で
  管理者として起動し、終了コード(消した数、-1 = 失敗)を受け取る。
- `python ../iivnc-server/tools/fwcheck.py [exe]`: exe を `build/fwtest/` に写し、その写しの仮の規則
  (Windows と同じ表示名、無効にしたもの)を管理者の PowerShell で作り、画面のボタンを押して、写しの規則が 0 件、
  本物の規則の数が変わらないことを見る(利用者の本物の規則には触れない)。
- 2026-10-04 にこの PC のレジストリを `iivnc` で検索: サーバーの確認画面の規則 2 件(パブリックで許可)、
  通知領域のアイコンの記録(`NotifyIconSettings`、`NotifyIconGeneratedAumid_<番号>` の通知設定など)、
  Windows がアプリすべてに付ける記録(MuiCache、FeatureUsage、互換性アシスタント)。サービスの登録・
  `SoftwareSASGeneration` は残っていなかった(サービスをやめたとき戻っている)。

- **利用者が動かしている iivnc-server が exe を使っていると上書きできない**(管理者で動いているので止めにくい)。
  動いている exe は名前を変えられるので、`build/` へ移してから組む(動いているものはそのまま動き続ける)。

### 確かめていないこと

- 2 台の PC の間で実際に使うこと(LAN 越し、ファイアウォールの確認画面、相手の IME の切り替わり)。
- サービス: UAC の確認画面(この PC は確認なしで昇格するので出せない)、ユーザーの切り替え(別のセッション)、
  ログオフ、リモート デスクトップでつないでいるとき。
- 実際のキー・マウスの再現(検証は `-dryrun` のログまで。利用者の画面を動かすことになるため)。
- 複数のディスプレイ、回転した画面、HDR を有効にした画面、拡大率の違う画面。
- 管理者として動かしたときの、管理者のウィンドウへの入力(input-mouser では実測で届いた)。
- RealVNC・UltraVNC などほかのビューア。
