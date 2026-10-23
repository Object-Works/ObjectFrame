# ObjectFrame

ObjectFrame は Windows 10 / 11 向けのタイル型ウィンドウマネージャーです。
開いたウィンドウを自動で画面いっぱいに並べ、キーボードだけでフォーカス移動・配置換え・ワークスペース切り替えができます。

- C++20 / Win32 API のみで書かれた単体の `ofwm.exe`(ランタイム不要)
- 設定はファイル (`ofwm.conf`) と GUI の設定画面の両方から編集可能
- 4 種類のレイアウト、ワークスペース、マルチモニター対応
- 角丸のフォーカス枠とグロー、移動・切り替えのアニメーション

---

## 目次

1. [ビルド](#ビルド)
2. [起動と終了](#起動と終了)
3. [基本の使い方](#基本の使い方)
4. [既定のキーバインド](#既定のキーバインド)
5. [レイアウト](#レイアウト)
6. [ワークスペースとモニター](#ワークスペースとモニター)
7. [見た目 (枠・グロー・アニメーション)](#見た目-枠グローアニメーション)
8. [設定画面](#設定画面)
9. [設定ファイル](#設定ファイル)
10. [コマンド一覧](#コマンド一覧)
11. [コマンドラインから操作する](#コマンドラインから操作する)
12. [ファイルの場所](#ファイルの場所)
13. [注意点・制限](#注意点制限)

---

## ビルド

必要なもの:

- Visual Studio 2022 (「C++ によるデスクトップ開発」ワークロード)
- CMake 3.20 以上

```powershell
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

`build\Release\ofwm.exe` ができます。

> 実行中の `ofwm.exe` があるとリンクに失敗します。先に `.\build\Release\ofwm.exe dispatch quit` で終了してください。

### インストーラー

GitHub Actions ([.github/workflows/build.yml](../.github/workflows/build.yml)) が push のたびに exe をビルドし、
[Inno Setup](https://jrsoftware.org/isinfo.php) でインストーラー (`ObjectFrame-<version>-setup.exe`) とポータブル版 zip を作ります。
どちらも Actions の実行結果の「Artifacts」からダウンロードできます。

バージョンはリポジトリ直下の `VERSION` ファイルで管理しています。リリースするときは `VERSION` を更新して、同じ番号のタグを push します。

```powershell
git tag v0.1.0
git push origin v0.1.0
```

タグを push すると、GitHub の Releases にインストーラーと zip が公開されます。
ローカルでインストーラーを作る場合は Inno Setup 6 を入れて次を実行します。

```powershell
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" /DAppVersion=0.1.0 installer\ObjectFrame.iss
```

---

## 起動と終了

| 操作 | 方法 |
| --- | --- |
| 起動 | `ofwm.exe` を実行 (タスクトレイにアイコンが出ます) |
| 設定画面を開く | `Win + F1`、またはトレイアイコンをダブルクリック |
| 一時停止 / 再開 | `Win + Shift + P` |
| 終了 | `Win + Shift + E`、またはトレイメニューの「終了」 |
| サインイン時に自動起動 | 設定画面の「起動」→「Windows へのサインイン時に ObjectFrame を起動する」 |

終了・一時停止すると、隠していたウィンドウはすべて元に戻ります。
異常終了した場合も、次回起動時に前回隠したままのウィンドウを復元します。

---

## 基本の使い方

- ウィンドウを開くと自動的にタイル配置されます。
- `Win + 矢印` で隣のウィンドウへフォーカス、`Win + Shift + 矢印` でウィンドウを入れ替えます。
- `Win + 1〜9` でワークスペースを切り替え、`Win + Shift + 1〜9` でウィンドウを別のワークスペースへ送ります。
- `Win + F` でフローティング (タイルから外して自由に動かせる状態) に切り替えます。
- ダイアログ、タスクマネージャー、電卓、ピクチャー イン ピクチャーなどは最初からフローティングになります ([ウィンドウルール](#ウィンドウルール))。

---

## 既定のキーバインド

`$mod` は既定で **Windows キー** です (設定画面の「メインモディファイア」で変更できます)。

### ウィンドウ

| キー | 動作 |
| --- | --- |
| `$mod + Enter` | ターミナル (`wt.exe`) を起動 |
| `$mod + Q` | ウィンドウを閉じる |
| `$mod + ← ↓ ↑ →` | フォーカスを移動 |
| `$mod + Shift + ← ↓ ↑ →` | ウィンドウを移動 (入れ替え) |
| `$mod + Shift + Enter` | フォーカス中のウィンドウをメイン位置へ |
| `$mod + F` | フローティング切り替え |
| `$mod + Shift + F` | 全画面切り替え |

### レイアウト

| キー | 動作 |
| --- | --- |
| `$mod + N` | 次のレイアウトへ (dwindle → master → monocle → scroll) |
| `$mod + D` | dwindle |
| `$mod + T` | master |
| `$mod + M` | monocle |
| `$mod + ,` / `$mod + .` | 比率 (scroll では列幅) を 5% ずつ増減 |
| `$mod + Shift + T` | 再タイル |

### ワークスペース・モニター

| キー | 動作 |
| --- | --- |
| `$mod + 1〜9` | ワークスペースを切り替え |
| `$mod + Shift + 1〜9` | ウィンドウをワークスペースへ移動して追従 |
| `$mod + Ctrl + ← / →` | 隣のモニターへフォーカス |
| `$mod + Ctrl + Shift + ← / →` | ウィンドウを隣のモニターへ移動 |

### その他

| キー | 動作 |
| --- | --- |
| `$mod + F1` | 設定画面 |
| `$mod + Shift + R` | 設定を再読み込み |
| `$mod + Shift + P` | 一時停止 / 再開 |
| `$mod + Shift + E` | 終了 |

> `Win + L` は Windows のロック画面に予約されているため割り当てできません。
> それ以外の Windows 標準のショートカット (`Win + D`、`Win + E` など) は、ObjectFrame に割り当てたものが優先されます。

---

## レイアウト

| 名前 | 並び方 |
| --- | --- |
| `dwindle` | 新しいウィンドウを置くたびに残りの領域を縦横交互に分割します。比率は `dwindle_ratio`。 |
| `master` | 左にメインのウィンドウ 1 枚、右に残りを縦に積みます。メインの幅は `master_ratio`。 |
| `monocle` | すべてのウィンドウを全画面で重ねます。 |
| `scroll` | ウィンドウを横一列の列として並べ、フォーカスに合わせて横にスクロールします。列が画面に収まるうちは画面幅を等分し、収まらなくなると `column_width` の幅で並べます。画面外の列は隣のモニターに映りません。 |

レイアウトはワークスペースごとに持ちます。設定の「デフォルトのレイアウト」を変えて保存すると、開いているすべてのワークスペースに反映されます。

---

## ワークスペースとモニター

### モニターごとに独立 (`workspace_mode = independent`)

各モニターがそれぞれ `workspaces` 個のワークスペースを持ち、キー操作はフォーカス中のモニターに対して働きます。

### 全モニター連動 (`workspace_mode = linked`)

左から並んだモニターに連続したワークスペースを表示し、すべてのモニターが一斉に切り替わります。

- 起動時はモニター 1 / 2 / 3 にワークスペース 1 / 2 / 3 が表示されます。
- **マウスがあるモニター** が指定したワークスペースになり、ほかのモニターはその前後の番号になります。
  例: マウスがモニター 2 にあるとき `$mod + 3` → モニターの表示は 2 / 3 / 4。
- 最後のワークスペースの次は 1 に戻ります。
- 切り替えはキー操作のみです (マウスを画面端に動かしても切り替わりません)。

### 切り替えのアニメーション

`workspace_slide` で、次のワークスペースへ進むときにウィンドウが動く方向を選べます (`auto` / `up` / `down` / `left` / `right`)。
`auto` は隣にモニターがない方向を選びます。左右を選んでも、スライド中のウィンドウはそのモニターの範囲で切り取って描くので、隣のモニターには映りません。

---

## 見た目 (枠・グロー・アニメーション)

| 設定 | 内容 |
| --- | --- |
| `border` / `border_width` | フォーカス枠の表示と太さ。0 にすると Windows 11 標準の 1px 枠に色を付けるだけになります。 |
| `border_radius` | 枠の角丸の半径 (0〜30px)。ウィンドウ自体の角は 0 / 4 / 8px の近いものに揃えます。 |
| `border_active` / `border_inactive` | アクティブ / 非アクティブ枠の色 (`0xRRGGBB`)。 |
| `glow` / `glow_size` / `glow_intensity` | アクティブなウィンドウの枠の周りを枠の色で光らせます。 |
| `animate_move` | タイル配置が変わったときにウィンドウを滑らかに移動します。 |
| `animate_workspace` | ワークスペース切り替え時にウィンドウをスライドさせます。 |
| `animation_duration` | アニメーションの長さ (50〜1000ms)。 |

### フォーカスの動き

| 設定 | 内容 |
| --- | --- |
| `focus_follows_mouse` | マウスが乗ったウィンドウをアクティブにします。カーソルが動いてウィンドウの上で少し止まったときだけ切り替わり、メニュー表示中やドラッグ中は切り替えません。 |
| `cursor_follows_focus` | キーボードでフォーカスを移したとき、マウスカーソルもそのウィンドウの中央へ移動します。 |

---

## 設定画面

`$mod + F1` またはトレイアイコンのダブルクリックで開きます。「保存して適用」を押すと設定ファイルに書き込まれ、すぐに反映されます (ワークスペース数のみ再起動後に反映)。

| ページ | 内容 |
| --- | --- |
| 一般 | メインモディファイア、デフォルトのレイアウト、隙間、比率、列幅、ワークスペース数と割り当て方式 |
| 動作 | マウスによるフォーカス、カーソルの追従、アニメーションの有無・長さ・スライド方向 |
| 枠とグロー | 枠の太さ・角丸・色、グローの広がり・明るさ (右側にプレビュー) |
| キーバインド | キーの追加・変更・削除、既定に戻す。「アプリを選択...」で exe やショートカットを選ぶと、そのアプリを起動するキーを作れます |
| ウィンドウルール | フローティング / 管理しない / 強制タイル のルール。「開いている窓から」で実行中のウィンドウから条件を作れます |
| 起動 | ObjectFrame 起動時に実行するコマンド、サインイン時の自動起動 |

左下の「設定ファイルを開く」でメモ帳を使って `ofwm.conf` を直接編集できます。

---

## 設定ファイル

場所: `%USERPROFILE%\.config\ofwm\ofwm.conf` (初回起動時に既定の内容で作られます)

```ini
# key = value 形式。'#' 以降はコメント
$mod = SUPER

gaps_in = 8
gaps_out = 12
default_layout = dwindle      # dwindle | master | monocle | scroll
workspace_mode = independent  # independent | linked

exec-once = explorer.exe

windowrule = float, exe:^Taskmgr\.exe$
bind = $mod, RETURN, exec, wt.exe
bind = $mod SHIFT, 1, movetoworkspace, 1
```

手で編集したら `$mod + Shift + R` で再読み込みします。問題のある行は無視され、内容がログに書き出されます。

### キーバインド

```ini
bind = MODS, KEY, command[, args]
```

- `MODS`: `SUPER` `ALT` `CTRL` `SHIFT` をスペース区切りで組み合わせ。`$mod` も使えます。
- `KEY`: `A`〜`Z`、`0`〜`9`、`F1`〜`F24`、`LEFT` `RIGHT` `UP` `DOWN`、`RETURN` `SPACE` `TAB` `COMMA` `PERIOD` など。
- アプリを起動する例:

```ini
bind = $mod, B, exec, "C:\Program Files\Mozilla Firefox\firefox.exe"
bind = $mod, E, exec, explorer.exe
```

### ウィンドウルール

```ini
windowrule = float|ignore|tile, class:|exe:|title:<正規表現>
```

| 動作 | 意味 |
| --- | --- |
| `float` | フローティングにする |
| `ignore` | ObjectFrame で管理しない |
| `tile` | 通常は管理しない種類のウィンドウでもタイルする |

上から順に評価され、最初に一致したルールが使われます。

---

## コマンド一覧

キーバインドの `command` や `ofwm.exe dispatch` で使えるコマンドです。

| コマンド | 引数 | 動作 |
| --- | --- | --- |
| `focus` | `left` `right` `up` `down` | フォーカス移動 |
| `move` | `left` `right` `up` `down` | ウィンドウ移動 |
| `workspace` | `1`〜`9` / `next` / `prev` | ワークスペース切り替え |
| `movetoworkspace` | 番号 | ワークスペースへ移動して追従 |
| `movetoworkspacesilent` | 番号 | ワークスペースへ送る (追従しない) |
| `focusmonitor` | `left` `right` / 番号 | モニターへフォーカス |
| `movetomonitor` | `left` `right` | ウィンドウをモニターへ移動 |
| `layout` | `next` `dwindle` `master` `monocle` `scroll` | レイアウト変更 |
| `togglefloat` | | フローティング切り替え |
| `fullscreen` | | 全画面切り替え |
| `resize` | `+0.05` / `-0.05` など | 比率 (scroll では列幅) の調整 |
| `promote` | | メイン位置へ移動 |
| `close` | | ウィンドウを閉じる |
| `exec` | コマンド / パス | アプリ・コマンドを起動 |
| `retile` | | 再タイル |
| `pause` | | 一時停止 / 再開 |
| `reload` | | 設定を再読み込み |
| `settings` | | 設定画面を開く |
| `quit` | | 終了 |

---

## コマンドラインから操作する

起動中の ObjectFrame にコマンドを送れます。スクリプトや他のツールとの連携に使えます。

```powershell
ofwm.exe dispatch workspace 3
ofwm.exe dispatch layout scroll
ofwm.exe dispatch quit
ofwm.exe settings                 # 設定画面を開く
ofwm.exe --config D:\my\ofwm.conf # 別の設定ファイルで起動
ofwm.exe --help
```

---

## ファイルの場所

すべて `%USERPROFILE%\.config\ofwm\` にあります。

| ファイル | 内容 |
| --- | --- |
| `ofwm.conf` | 設定ファイル |
| `ofwm.log` | ログ (管理したウィンドウ、受け取ったコマンド、設定の警告など) |
| `hidden.txt` | 非表示ワークスペースに隠しているウィンドウの一覧。異常終了後の復元に使います |

---

## 注意点・制限

- **管理者権限のウィンドウ**: ObjectFrame を通常権限で起動している場合、管理者として実行中のアプリは Windows の制限により操作できません。管理する必要がある場合は ObjectFrame も管理者として起動してください。
- **ワークスペース数** の変更は再起動後に反映されます。
- **角丸**: Windows が描くウィンドウ自体の角は 0 / 4 / 8px のいずれかです。それ以外の半径は枠の描画で表現します。
