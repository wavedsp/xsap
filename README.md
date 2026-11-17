# Xsap 3.1.0

Xsap is a C++17 GTK4 waveform and speech-analysis application for Linux. It
retains the analysis and playback behavior of the original DOS, Solaris, and
Linux SAP lineage while matching the WinSap 3.1.0 layout and commands as
closely as practical on Ubuntu.

## Features

- Two independent analysis panels with up to six tracks per panel
- Full-memory loading for signed 16-bit mono raw PCM and for mono or stereo
  WAV files (8/16/24/32-bit integer PCM or 32/64-bit float)
- Stereo WAV files split across the panels (left up, right down) with the
  panels kept in sync, and stereo playback
- Parameter-file display and 2048-point FFT amplitude analysis
- Line spectrum for views up to 1024 samples and a colour-mapped spectrogram
  for wider views, backed by a background whole-file analysis cache
- Connected waveform views with large-file envelope rendering
- Frame navigation, zoom, amplitude scaling, and marked ranges
- WinSap-compatible arrow, Page Up/Page Down, Home, End, and function-key navigation
- Lower-waveform overlay on the upper panel
- Marked-range playback and export, with a file-name context menu for closing
  a file or playing its marked area or one stereo channel
- Runtime-loaded PulseAudio playback with ALSA fallback
- WinSap-matched menu ordering, labels, mnemonics, toolbar icons, and six-pane status bar
- Fixed signed 16-bit waveform scale of -32768 to 32768, matching WinSap
- Native GTK file chooser with WinSap-compatible Mode choices
- Help > Help Topics (F1): a scrollable guide to opening, navigating, marking,
  playing, spectrograms, shortcut keys and settings (Esc closes it)
- HiDPI-aware Cairo rendering with supplied multi-resolution toolbar icons

## Requirements

- A C++17 compiler
- CMake 3.16 or newer
- gtkmm 4.10 (`libgtkmm-4.0-dev`)
- POSIX threads and `libdl`
- At runtime, `libpulse-simple.so.0` or `libasound.so.2` for playback

Ubuntu 24.04 is the baseline (the same packages work in WSLg):

```sh
sudo apt update
sudo apt install build-essential cmake libgtkmm-4.0-dev libpulse0 libasound2t64
```

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

Run Xsap with a graphical Linux display (including WSLg):

```sh
./build/xsap [audio-file ...]
```

Install Xsap to register its launcher and application icon with the Ubuntu
desktop:

```sh
sudo cmake --install build
```

Command-line files are opened as waveform tracks, alternating between upper
and down panels and advancing to the next slot after each pair. Use `--help`
for command-line help or `--version` for the version number.

Launching Xsap again forwards file-open requests to the existing window. This
supports the sibling XPlayer's **Open in XSap** command without creating another
analyzer instance. Each request starts at upper slot 0 and uses the same
upper/down ordering; corresponding occupied slots are replaced. XPlayer sends
the original file unchanged, and Xsap applies its normal format restrictions
and its own raw sample-rate setting. A new file-open request stops active Xsap
playback before replacing tracks. Failed loads preserve the previous track in
the destination slot.

## Opening files

`Open Upper…` and `Open Down…` use the GTK file chooser and expose the same
mode choices as WinSap: Binary, Parameter, or FFT Amplitude. Waveforms use
connected display. The first track in a panel opens in View All. Adding a
longer track expands the panel to show its full duration, while adding a
shorter track returns to frame zero while preserving the current zoom span.
This keeps newly opened tracks aligned at the beginning, and every waveform is
drawn against the shared panel timeline rather than stretched to its own file
length. Filters cover WAV, raw PCM, `.par`, `res/exc/cod`, and all files.
Parameter mode and the `.par` filter select each other, and a `.par` file
always opens as parameters, from the chooser or the command line. WAV files
are loaded as waveform/binary data. Xsap warns before proceeding when
Parameter mode is used without `.par`, or Binary/FFT mode is used without
`.wav`, `.pcm`, `.dat`, `.raw`, `.exc`, `.res`, or `.cod`.
Continuing disables that warning category in `xsap.conf`; cancelling leaves it
enabled for the next mismatch.

## File formats and limits

- Raw files are little-endian signed 16-bit mono PCM. Their sample rate is set
  in Xsap and defaults to 16 kHz.
- Like WinSap, Xsap decides whether a file is a WAV from the RIFF/WAVE magic
  number, not from its extension. A `.pcm` file that really is a WAV is read as
  one, and a headerless `.wav` file is read as raw PCM.
- WAV files may be mono or stereo, in 8-bit unsigned, 16, 24 or 32-bit
  integer PCM or 32/64-bit IEEE float, including WAVE_FORMAT_EXTENSIBLE
  headers. They are converted to 16-bit samples in memory on load with
  WinSap's rounding and clipping, and their embedded sample rate is used for
  playback. Other formats and files with more than two channels are refused
  with a message naming the problem. Saved/exported WAV files are 16-bit mono
  and are written to a temporary file that replaces the destination only once
  complete, so a failed save leaves an existing file intact.
- Parameter files use `FRAME=` sections followed by two display lines. As in
  WinSap the tag is matched only in upper case and only at the start of a line,
  and the frame count comes from the highest `FRAME=` number in the last 1000
  bytes of the file. There is no frame limit: only the frames present are
  kept in memory, so long or sparsely numbered files load quickly. `FRAME=`
  numbers of 1,000,000,000 or more are ignored.
- Audio files are intentionally loaded completely into memory. Available
  address space and RAM therefore limit the maximum file size.
- Xsap requires a GTK display. It works with Wayland/X11 GTK backends and with
  WSLg on Windows 11.
- Under WSLg, Xsap defaults to GTK's X11 backend and Cairo software renderer
  to avoid invisible Wayland windows and Mesa EGL/Zink initialization failures.
  It also disables Mesa hardware probing. Set `GDK_BACKEND`, `GSK_RENDERER`, or
  `LIBGL_ALWAYS_SOFTWARE` explicitly to override these defaults.

## Stereo files and playback

- A stereo WAV opened from Open Upper, Open Down or the command line puts its
  left channel in an upper-panel slot and its right channel in the same
  lower-panel slot (labelled [L] and [R]). While a stereo file is open the two
  panels stay in sync: frame size, frame number, navigation/zoom, limits and
  marks apply to both (limits only when both panels are FFT or neither is).
  If the lower panel already holds files, Xsap asks whether to close them
  first. Several stereo and mono files can share the upper panel, but mono
  files cannot be opened in the lower panel while a stereo file is open.
  Closing either channel closes both.
- Waveform and FFT mode never mix with stereo: a stereo file opened in FFT
  Amplitude mode must be the only open file (Xsap offers to close the rest),
  and while a stereo file is open in Binary mode no file may be opened in FFT
  Amplitude mode.
- Left-clicking a file name plays the whole file; a stereo file plays in
  stereo with the cursor in both panels. Play Up/Down plays the marked area
  of the first file in that panel that has data there (a stereo file in
  stereo). Play All queues every open binary-short or FFT-amplitude slot in
  upper-then-lower order, playing each stereo file once. Stop or right-click
  cancels the active item and clears the queue.
- Right-clicking a file name opens a menu: Close file and Play marked area
  (this file only). For a stereo file it also offers Play this channel (the
  whole left or right channel) and Play marked area in this channel; its Play
  marked area plays both channels. FFT-amplitude files play with the cursor
  running over the spectrogram (it is hidden in the single-frame spectrum-line
  view), and a mark on a spectrogram is drawn as white lines at its edges.

## Shared analysis core

Everything that is not UI or platform code is a direct port of the WinSap x64
sources rather than a parallel reimplementation. It lives in `src/core/`:

| File | Ported from |
| --- | --- |
| `core/SapFft.*` | `Panel::FFT` and the numeric half of `Panel::DispSpectrum` |
| `core/SapWaveFile.*` | `ReadWavFile`, `FileLength`, `PathName` and the RIFF probes in `WinsapApp::SetupFileUp()` / `SetupFileDown()` |
| `core/SapParameterFile.*` | `Panel::ParFileTotal`, `Panel::BuildParameterIndex` and the text half of `Panel::DispID` |
| `core/SapSpectrogram.*` | `dispSpectrum`'s spectrogram branch in the original DOS `wsap` |
| `core/SapSpectrogramCache.*` | new; since ported back into WinSap as `common/SpectrogramCache.*` |
| `core/SapSpectrogramPalette.*` | new; shared verbatim with WinSap's `common/SpectrogramPalette.h` |

Spectra therefore use WinSap's constants and scaling: 2048-point transform,
1024 displayed bins, a Hamming window over the frame length, and dB referenced
so that a full-scale sine wave reads 0 dB with a -120 dB floor. Earlier Xsap
releases normalised each frame against its own peak, so every frame's loudest
bin read 0 dB regardless of level; that is no longer the case.

`SapFft` offers three transforms with the same output.

- `ComputeSpectrumDb` is the verbatim WinSap routine, kept as the reference.
  WinSap regenerates the twiddle factors with `cos()`/`sin()` once per butterfly
  group, which is 4094 libm calls per 2048-point transform and costs more than
  the butterflies themselves.
- `ComputeSpectrumDbFast` is the same algorithm with the twiddle factors and the
  Hamming window precomputed. About 2.4x faster.
- `ComputeSpectrumDbReal` additionally exploits the fact that the input is real:
  the 2048 real samples are packed into a 1024-point complex transform and the
  spectrum is recovered from the Hermitian structure. About 3.3x faster than
  WinSap's original, and this is what the application calls.

Precomputing the tables also improves accuracy, because WinSap advanced the
stage angle by repeated `float` addition and so accumulated phase drift: worst
error against an exact double-precision DFT is 0.062 dB for the original versus
0.001 dB for both optimised paths.

WinSap contained a `RealFFT` (`LoadSinCos` + `ComplexFFT` + `AdjustFFT`) that
was attempting the same idea, but it was dead code - nothing called it, and
`ComplexFFT` derived its twiddle index from the block number instead of the
butterfly number, so it indexed a 512-entry table at up to 523520 and crashed at
any useful size. Those four routines were removed from both projects and
replaced by the working real-input path.

`tests/ModelTests.cpp` pins all three transforms together, and
`tests/FftBench.cpp` (built as `xsap_fft_bench`) reports throughput and the
error of each against an exact double-precision DFT.

## Spectrum display

SAP has always had two spectrum displays, and which one you get depends on how
much audio is in view:

- **View up to 1024 samples**: the familiar line plot, amplitude in dB against
  frequency. The analysis window is every frame the frame count asks for, not
  just the first one, so raising the frame count lengthens the window and buys
  frequency resolution at the cost of time resolution. At 16 kHz with a frame
  size of 160, one frame is a 10 ms window and six frames is 60 ms.
- **Longer view**: a spectrogram, frequency against time.

The second mode came from the original DOS `wsap` and was lost in every later
port, WinSap included. It is restored here in `core/SapSpectrogram.*`, and has
now been ported back into WinSap as well.

The vertical axis follows the mode: dB for the line spectrum, Hz from zero to
the top of the plot for the spectrogram.

The status bar readout follows it too, since a sample index against an amplitude
means nothing on either spectrum:

| Display | Readout |
| --- | --- |
| Waveform | `(sample, amplitude)` |
| Line spectrum | `(3520 Hz, -42 dB)` |
| Spectrogram | `(00:00:01.234, 3520 Hz)` |

Spectrogram time is measured from the start of the file, not the start of the
view, because that is what you are usually trying to locate.

Both spectrum displays reach Nyquist. `kFftCutOff` used to be 900 bins of a
2048-point transform, which stopped the axis at 0.44 x sample rate - 7031 Hz
instead of 8000 at 16 kHz. That limit dated from the anti-aliasing filters of
period A/D hardware, whose passband ended well below Nyquist, so the top of the
band was filter roll-off rather than signal. Modern converters are flat much
closer to Nyquist, and the transform measures clean up there (peak level within
0.1 dB of the rest of the band, spurious content below -64 dB), so `kFftCutOff`
is now `kFftLengthHalf` and the line spectrum and the cached spectrogram share
one ceiling. The original DOS wsap drew the full half-spectrum too; the cut-off
was introduced in the Windows port.

A real tone within a bin or two of Nyquist will read a few dB high, because its
negative-frequency image folds back and overlaps the main lobe. That is inherent
to sampled real signals, not a defect in the analysis.

`Ctrl+U` and `Ctrl+D` narrow and widen the level ramp, which by default spans
60 dB below the file peak. `Ctrl+P` steps through the colour maps in
`core/SapSpectrogramPalette.h`, which is shared verbatim with WinSap's
`common/SpectrogramPalette.h`. Viridis is the default because it is
perceptually uniform, so equal steps in level look like equal steps in
brightness and it shows none of the false ridges jet produces at its cyan and
yellow transitions; it is also legible with the common forms of colour
blindness. Jet is offered because a good deal of commercial speech software uses
it, inferno because it suits a dark room, and grey because it is the original
`wsap` picture, entry for entry.

The selected dB range and colour map are saved immediately in the
`[Spectrogram]` group of `xsap.conf` and restored at startup.

Neither plot is drawn point by point. The line spectrum reduces its bins to
one vertical span per pixel column; the original drew a polyline segment per
bin, which rasterised a near-full-height zigzag into a plot only a few hundred
pixels wide and measured about 5 ms per repaint against 21 us for the transform
itself. The spectrogram is composed into an image and blitted once.

### Whole-file spectrogram cache

Analysing only what is on screen means that advancing the cursor by one frame
re-cuts every column and re-runs every transform, 1.5 to 10 ms per cursor move
on a 400 frame view, and the picture shimmers because the analysis grid is
anchored to the view and slides underneath it.

`core/SapSpectrogramCache.*` analyses the whole file once instead, on a grid
anchored to the file. Scrolling then reduces cached columns, 0.02 ms, and the
picture is stable.

- **Adaptive geometry.** The window is 20 ms of audio and the FFT is the next
  power of two at least that long, with a floor of 512 points. At 48 kHz that
  is a 960 sample window in a 1024-point transform; a 512-point transform
  cannot cover a 20 ms frame at that rate. The hop is 5 ms. Effective bandwidth
  is therefore about 200 Hz at every sample rate, between a wideband
  spectrogram (formants, glottal striations) and a narrowband one (resolved
  harmonics).
- **Bounded memory.** The hop widens until the matrix fits a 256 MB budget, so
  even a multi-hour file is cached.
- **Built in the background, view first.** The build starts when the file is
  opened, not when the spectrogram is first drawn. The file is split into
  chunks and workers claim whichever outstanding chunk is nearest the part of
  the file being displayed, so the visible screen finishes long before the rest.
  On a one-hour 48 kHz file the matrix becomes renderable in 250 ms, a screen
  landed 80% of the way in is complete 15 ms later at 1% overall progress, and
  the whole file is done in 650 ms. Scrolling into an unanalysed region
  re-prioritises the same build rather than starting a second one.
- **dB, not colour.** Columns store quantised dB, so changing the display
  dynamic range or palette only re-renders; it never re-analyses. The colour ramp spans a
  settable number of dB below the whole-file peak, 60 dB by default, adjusted
  with `Ctrl+U` and `Ctrl+D`. `wsap` instead mapped a fixed absolute 0-128
  dB range through a quadratic, which on 16-bit material wastes most of the 256
  levels and washes the picture out.
- **Reduction is by maximum** on both axes, so a brief loud event survives
  decimation rather than being averaged away.

Completed chunks are published with release/acquire ordering and workers write
disjoint ranges, so rendering needs no lock. The test suite covers the geometry
chooser, single- against multi-threaded parity, partial rendering, and that a
known tone lands on the row the frequency axis claims; it is also clean under
ThreadSanitizer, AddressSanitizer and UndefinedBehaviorSanitizer.

Copyright 2026 Tian Wang

Xsap is released under the MIT License. See `LICENSE`. The Viridis and Inferno
spectrogram palettes contain sampled colour data derived from Matplotlib; see
`THIRD_PARTY_NOTICES.md` for its copyright notice, license terms and a summary
of Xsap's changes.
