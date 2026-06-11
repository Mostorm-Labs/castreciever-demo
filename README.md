# UsbCastReceiver

UsbCastReceiver is a Windows native proof-of-concept for a USB cast receiver device. It avoids Electron, Chromium, WebView, Qt, FFmpeg, ImGui, and third-party dependencies. The goal is to keep the render and audio paths as close as possible to Windows system multimedia components.

## Architecture

Video:

```text
UVC H.264 -> Media Foundation Capture Engine -> HWND preview
```

Audio:

```text
UAC PCM -> WASAPI capture -> WASAPI render
```

The default video path uses Media Foundation Capture Engine preview directly against a child `HWND`. The `SourceReaderD3D11Player` fallback can be selected for diagnostics; it reads frames through Source Reader, asks Media Foundation to decode to RGB32, and draws with a simple GDI blitter. That confirms frame delivery before replacing the diagnostic blitter with a D3D11 renderer.

The audio path captures PCM from the selected UAC endpoint and writes it to the default render endpoint. Muting does not stop or reopen the capture device; the relay keeps draining capture buffers and writes silence to render.

## Build

Use Windows 10/11 x64 with Visual Studio 2022:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

## Run

Run with default device selection:

```bat
build\Release\UsbCastReceiver.exe
```

Run with substring matching:

```bat
build\Release\UsbCastReceiver.exe --uvc-match "camera name" --uac-match "audio name"
```

`--uvc-match` is matched case-insensitively against the UVC friendly name and symbolic link. `--uac-match` is matched case-insensitively against the UAC friendly name and device id. If a match argument is omitted or empty, the first active endpoint of that type is selected.

Video diagnostics:

```bat
build\Release\UsbCastReceiver.exe --uvc-match "camera name" --preview-sink default
build\Release\UsbCastReceiver.exe --uvc-match "camera name" --video-format auto --preview-sink default
build\Release\UsbCastReceiver.exe --uvc-match "camera name" --preview-sink add-stream
build\Release\UsbCastReceiver.exe --uvc-match "camera name" --preview-sink rgb32
build\Release\UsbCastReceiver.exe --uvc-match "camera name" --video-backend source-reader
```

`--video-backend capture` is the default and uses Media Foundation Capture Engine preview. `--video-backend source-reader` bypasses Capture Engine preview and uses Source Reader to decode to RGB32 before drawing frames with a diagnostic GDI renderer. `--video-format h264` is the default and selects an H.264 native UVC type when present. `--video-format auto` leaves the current device media type untouched and lets the selected backend choose. `--preview-sink default` is the default Capture Engine mode and only calls `SetRenderHandle`; `add-stream` and `rgb32` are diagnostic modes for driver stacks that need explicit preview sink configuration.

## Implemented

- Win32 main window with a child video window.
- Three Win32 buttons: `Mute/Unmute`, `Stop`, and `Maximize/Restore`.
- ESC restores a maximized window, otherwise closes the application.
- UVC enumeration through Media Foundation device sources.
- UAC enumeration through MMDevice API.
- Media Foundation Capture Engine preview player.
- WASAPI PCM capture-to-render relay.
- Thread-safe mute state that keeps consuming capture data.
- OutputDebugStringW logging for device discovery, formats, and HRESULT failures.

## Current Limits

- If the UAC capture format differs from the default render mix format, the relay uses the Windows Audio Resampler DSP through Media Foundation. This covers common PCM/float sample-rate and channel-count differences, such as 16 kHz mono capture to 48 kHz stereo render.
- Some UVC H.264 devices may not preview directly through Capture Engine on every driver stack. In that case, complete and switch to the Source Reader + H.264 Decoder MFT + D3D11 fallback path.
- The current preview path relies on Capture Engine for decode, scheduling, and presentation. It does not expose frame-level statistics yet.

## Troubleshooting

- `CoCreateInstance(CLSID_MFCaptureEngine) failed: 0x80004002 (No such interface supported)` means the app could not obtain `IMFCaptureEngine` before opening the UVC device. The code now first tries `IMFCaptureEngineClassFactory::CreateInstance`, then falls back to direct `CLSID_MFCaptureEngine` creation and logs both HRESULT values.
- If Capture Engine creation still fails, verify the machine is a full Windows 10/11 desktop install with Media Foundation components available. Windows N/KN editions may require the Media Feature Pack.
- If Capture Engine creation succeeds but `IMFCaptureEngine::Initialize` fails, investigate device selection, camera privacy settings, device occupation by another process, UVC driver behavior, and supported media types.
- If `Capture Engine preview started` appears but the window stays black, first try `--video-format auto --preview-sink default`. Then compare `--preview-sink add-stream` and `--preview-sink rgb32`. If those stay blank, try `--video-backend source-reader` to verify whether frames can be read and decoded outside Capture Engine preview.
- For deeper Media Foundation diagnostics, run:

```bat
mftrace -log mftrace.txt build\Release\UsbCastReceiver.exe --uvc-match "your device"
```

## Performance Principles

- No Electron, Chromium, or WebView render chain.
- No per-frame CPU readback and GPU re-upload in the main path.
- Prefer Windows Media Foundation system H.264 decoding.
- Use only a few Win32 controls for UI.
- Avoid layered transparent windows to reduce extra DWM composition cost.
- Do not create D3D, Media Foundation, or COM resources per frame.
- Muting writes silence instead of restarting audio devices.
- Keep video and audio lifetimes independent and stoppable.

## TODO

- Complete the Source Reader + D3D11 fallback renderer.
- Harden audio resampling with drift handling and glitch metrics.
- Add audio/video sync and latency handling.
- Add FPS, dropped-frame, and end-to-end latency statistics.
- Replace plain buttons with a lightweight Direct2D control bar if needed.

## Validation Suggestions

- Run 1080p60 playback continuously for 30 minutes.
- Test 4K input if the USB device supports it.
- Monitor CPU and GPU utilization.
- Resize, maximize, and restore the window repeatedly.
- Toggle mute/unmute and listen for pops or glitches.
- Watch memory usage during long playback.
