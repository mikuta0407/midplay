# midplay

macOS と Linux のターミナルで動く Standard MIDI File プレイヤー。ソフトウェア音源（既定の音声出力で鳴らす）か
MIDI の出力先（MIDI 機器、他のアプリ）に演奏させ、16 パートの状態・ピアノロール・イベントリストを表示する。

| | ソフトウェア音源（`midplay list` の `AU` / `SF2`） | MIDI 出力先（`MIDI`） |
|---|---|---|
| macOS | Audio Unit 音源（`au:MANU/SUBT`） | CoreMIDI の出力先（MIDI 機器、IAC バス、他のアプリ） |
| Linux | FluidSynth + SoundFont（`sf:/path/to/file.sf2`） | ALSA シーケンサのポート（MIDI 機器、FluidSynth/TiMidity++ などのデーモン） |

## ビルド

```sh
make                 # build/midplay
make install         # ~/.local/bin/midplay（PREFIX=... で変更）
```

- macOS: Xcode か Command Line Tools が要る。依存は macOS の AudioToolbox / CoreAudio / CoreMIDI / CoreFoundation / libiconv のみ。
- Linux: C コンパイラ、pkg-config、ALSA と FluidSynth の開発パッケージが要る。
  Debian/Ubuntu なら `sudo apt install build-essential pkg-config libasound2-dev libfluidsynth-dev fluid-soundfont-gm`
  （Fedora なら `alsa-lib-devel fluidsynth-devel fluid-soundfont-gm`）。

## 使い方

```sh
midplay                      # ファイラ（前回のディレクトリ、なければカレント）
midplay ~/Music/midi         # そのディレクトリをファイラで開く
midplay song.mid             # すぐ開く
midplay -o midi:IAC song.mid # 出力を指定して起動
midplay list                 # 使える出力の一覧（番号は -o に使える）
midplay hash -o au:appl/dls song.mid    # ソフトウェア音源でオフライン描画し sha256 を表示（検証用）
```

オプション（その回だけ有効。保存はしない）:

| オプション | 意味 |
|---|---|
| `-o, --output OUT` | `au:MANU/SUBT`（四文字コード）、`au:名前の一部`（macOS）、`sf:SoundFontのパス`、`sf:名前の一部`（Linux）、`midi:名前の一部`、または `midplay list` の番号 |
| `--autoplay` / `--no-autoplay` | ファイルを開いたらすぐ再生する／Space を押すまで待つ |
| `--exit-at-end` / `--stay` | 曲の最後で停止してシェルに戻る／停止して画面に残る |
| `--ascii` | ASCII だけで描く（ブロック文字が全角幅になる端末向け） |
| `--null-audio` | （試験用）ソフトウェア音源をタイマーで駆動し音は捨てる |

曲の最後まで行くと必ず停止する。シェルに戻るかどうかは `--exit-at-end` か設定画面で選ぶ。

## キー

再生画面: `Space` 再生/停止（最後まで行った後は頭から）、`←` `→` 1小節、`,` `.` 8小節、`↑` `↓` パート選択、
`m` ミュート、`s` ソロ、`u` 解除、`r` 先頭、`o` ファイラ、`c` 設定、`q` 終了。

ファイラ: `↑` `↓` `PgUp` `PgDn` 移動、`Enter`/`→` 開く・再生、`←`/`BS` 親へ、`~` ホーム、`a` 全ファイル表示の切替、
`H` 隠しファイル、`Esc` 再生画面へ戻る、`c` 設定、`q` 終了。

設定画面（`c`）: 出力の選択、開いたらすぐ再生するか、曲の最後でシェルに戻るか、ASCII 描画。
変更はすぐ `~/.config/midplay/config`（`$MIDPLAY_CONFIG` で変更可）に保存される。出力を変えると、再生位置を保ったまま切り替わる。

再生中の出力名は画面2行目（ファイラでも2行目）に出る。

## Linux での出力

- SoundFont は `$MIDPLAY_SOUNDFONTS`（ディレクトリかファイルを `:` 区切り）、`~/.local/share/soundfonts`
  （`$XDG_DATA_HOME/soundfonts`）、`/usr/local/share/soundfonts`、`/usr/share/soundfonts`、`/usr/share/sounds/sf2`、
  `/usr/share/sounds/sf3` の `*.sf2` / `*.sf3` を探す。一覧にないファイルも `-o sf:/path/to/file.sf2` で使える。
  既定は FluidR3_GM があればそれ、なければ一覧の先頭。
- 音声は FluidSynth のドライバで出す。PipeWire、PulseAudio、ALSA の順に開けたものを使い、
  `$MIDPLAY_AUDIO_DRIVER`（`pipewire`、`pulseaudio`、`alsa`、`jack` など）で固定できる。
  サンプルレートは 44100 Hz（`$MIDPLAY_RATE` で変更）。
- MIDI 出力先は ALSA シーケンサで書き込みを受け付けるポート全部（「クライアント名: ポート名」で表示）。
  ハードウェアの MIDI 機器、`fluidsynth -a pipewire -s ...`、`timidity -iA` などのデーモンに送れる。

## 動作の詳細

- FluidSynth（Linux）: イベントは 64 フレーム（FluidSynth の処理単位）境界にそろえて送るので、`midplay hash` の
  結果は毎回同じになる。一時停止中は音源を動かさない。
- Audio Unit（macOS）: 既定の音声出力のサンプルレートで動かす。イベントは 128 フレーム境界にそろえて送るので、
  `midplay hash` の結果は毎回同じになる。一時停止中は音源を動かさないので、再開は途切れない。
- MIDI（CoreMIDI / ALSA シーケンサ）: 1 ms 周期のスレッドが時刻になったイベントを即時送信する。一時停止・停止時は全チャンネルに
  All Notes Off / All Sound Off、曲を開くときとシーク時はそれに Reset All Controllers を加える。
- シーク: 目標位置より前のノート以外のイベントを送り直す（同じコントローラの最後の値だけ。SysEx、RPN/NRPN、データエントリは全部）。
- 歌詞・テキストが UTF-8 でなければ Shift_JIS（CP932）として表示する。
- 音色名は GM 名（XG のバリエーション音色はバンク番号と GM 名）。ドラムは Bank MSB 127/126 で判定。

## 試験

```sh
python3 tests/tui_snapshot.py demo /tmp/demo.mid                 # 16 パートの試験曲
python3 tests/tui_snapshot.py --at 3 -- --null-audio /tmp/demo.mid   # 疑似端末で画面を取り込む（pyte が要る）
make build/midi_sink && python3 tests/check_midi.py /tmp/demo.mid    # 仮想 MIDI 出力先で順序と時刻を照合
```

`build/midi_sink` は macOS では CoreMIDI の仮想出力先、Linux では ALSA シーケンサのポート
（`tests/midi_sink_alsa.c`。`snd-seq` カーネルモジュールが要る）。
