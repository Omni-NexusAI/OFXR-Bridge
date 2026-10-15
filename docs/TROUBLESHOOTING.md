# OFXR Bridge troubleshooting

> [!IMPORTANT]
> **First, make sure you are on the latest version: 0.2.13.4.** Every
> release fixes problems reported here, so check the
> [releases page](https://github.com/djules75/OFXR-Bridge/releases) before
> anything else. The tray's menu shows the version you are running.

> [!TIP]
> **Pimax headsets:** OFXR uses less VRAM through SteamVR with
> [sboys3's native SteamVR driver](https://store.pimax.com/blogs/blogs/sboys3-native-steamvr-driver-setup-guide)
> than through Pimax OpenXR, so we recommend it, especially in MSFS 2024.
> [Details](#how-much-vram-ofxr-uses).

## Step 1: is OFXR running in your game?

**Performance issues? Try Reset to default first.** If you have changed any
of OFXR's settings, right-click the tray icon, click **Reset to
default...** near the bottom of the menu, confirm, and restart the game. It
puts every OFXR setting back to its factory default, including the ones
that cost the most performance when changed: optical flow **NVIDIA
Medium** at **50%** resolution, **NVIDIA bidirectional consistency** off,
**Prefer FPS over latency** on and **Lower VRAM** on. Many stutter and low
frame rate reports come from these settings alone
([why](#optical-flow-settings)). If the problem is still there after the
restart, carry on below.

Turn on the FPS overlay: right-click the tray icon, **FPS overlay**, pick a
corner. Start the game and look at that corner in the headset.

| You see | It means | Go to |
|---|---|---|
| No number at all | OFXR is not loaded in the game | [Table A](#a-ofxr-is-not-loaded) |
| A **red** number | OFXR is loaded but not generating frames | [Table B](#b-ofxr-is-loaded-but-not-generating) |
| A **green** number | OFXR is generating frames | **Reset to default** (above) if you have not yet, then [Table C](#c-ofxr-works-but-it-does-not-feel-better) |
| **Two orange bars** before the number | Frame generation is paused | Choose **Resume frame generation** in the tray menu, or press the pause key (Ctrl + Alt + F7 unless you changed it) |

Vulkan games (No Man's Sky, for example) show no FPS number. For those, use
the flight recorder instead: tick **Bridge flight recorder** in the tray, play
for a minute, then **Open bridge logs**. If no new log file appears, OFXR is
not loaded (Table A).

## A. OFXR is not loaded

| Cause | What to do |
|---|---|
| The game uses **OpenVR**, not OpenXR | OFXR only works with OpenXR games. Some OpenVR games can run through OpenXR with [OpenComposite](https://gitlab.com/znixian/OpenOVR). |
| The game runs **as administrator** | Run the game, and Steam or its launcher, normally. Never as administrator. |
| The tray was not armed when the game started | Start the tray first; it arms by itself. Then start the game. If you disarm and re-arm, restart the game. |
| The game runs under another Windows user | Run the tray and the game under the same Windows account. |
| The game is 32-bit | Not supported. |
| The game's anti-cheat blocks it | Not supported. |

## B. OFXR is loaded but not generating

| Cause | What to do |
|---|---|
| You are in a menu or loading screen | Normal. Generation starts once you are in the 3D world. |
| The game uses OpenGL | Not supported. |
| Too many swapchains for SteamVR (some UEVR games, often with depth submission on) | Turn off **Prefer FPS over latency**, or turn off depth submission in UEVR. |
| You disarmed OFXR from the tray | Arm it again, then restart the game. |
| You paused frame generation, from the tray or with the pause key (the FPS number shows two orange bars) | Choose **Resume frame generation** in the tray menu, or press the key again. No restart needed. |
| Generation stops and starts by itself while you play | The pause key (Ctrl + Alt + F7 by default) is also a key your game or a mod uses. Choose another under **Current key binding** in the tray menu. |
| Something else | Send a flight log (see below). |

## C. OFXR works, but it does not feel better

| Cause | What to do |
|---|---|
| The game already runs near your headset's refresh rate | Nothing to gain: OFXR helps games that can't hold full rate. Raise resolution or settings until the game sits around half your refresh rate. |
| The game runs **below half** your refresh rate (under 45 FPS at 90 Hz) | Turn on **3X Frame Gen** in the tray (for games around 30 FPS at 90 Hz), lower the refresh rate (72 Hz needs 36 FPS), or lower settings until the game holds half. |
| **3X Frame Gen** is on for a game that can hold half your refresh rate | Turn 3X off: it holds the game to a third of the refresh rate, and 2X looks better. |
| Your VR software's own frame smoothing is on | Turn it off, it fights OFXR: **SteamVR** Motion Smoothing and "fixed frame rate at half" (per-application video settings), **Virtual Desktop** SSW, **Pimax Play** Smart Smoothing, **Pico Streaming Assistant** Frame Interpolation. |
| Your GPU runs out of VRAM | OFXR needs extra video memory on top of the game (see [How much VRAM OFXR uses](#how-much-vram-ofxr-uses)). Signs: stutter, sudden drops, or a crash, often after a few minutes. Lower the game's settings or the per-eye resolution first. |
| 3X doesn't switch while you play | The game was started with **Prefer FPS over latency** off. Restart the game. |
| The number stops short of the refresh rate (110 to 115 at 120 Hz, for example), although the game runs above half of it without OFXR | Generating frames costs GPU time too, so the game needs some headroom above half the refresh rate. Lower the refresh rate, the per-eye resolution or the optical-flow resolution. Example: No Man's Sky ran at 94 FPS without OFXR; OFXR gave an even 100 at 100 Hz and 113 to 115 at 120 Hz, because a real frame plus a generated one took about 17.7 ms and 120 Hz allows 16.7. |
| Choppy or below half your refresh rate with **Optical flow resolution** above 50%, **NVIDIA Slow**, or **NVIDIA bidirectional consistency** on | These cost a lot of GPU time (see [Optical flow settings](#optical-flow-settings)). Click **Reset to default** in the tray and restart the game. |
| Dips only in heavy scenes | Disarm OFXR and play the same scene. If the dips are still there, lower the per-eye resolution: OFXR can't make up frames the game doesn't render. |
| One eye flickers or shimmers and the other is fine (MSFS 2024 with 3X Frame Gen) | Fixed in 0.2.12.2. Update. |
| One eye stutters in a D3D11 game, and the stutter goes away with OpenXR Toolkit on (IL-2 Great Battles) | Fixed in 0.2.13.2. Update. |
| On SteamVR, lower frame rates on 0.2.13.1 than on earlier versions, with SteamVR at half your refresh rate (MSFS 2024, often with 3X Frame Gen) | Fixed in 0.2.13.2. Update. |
| The overlay shows your full refresh rate in green, but the headset looks no smoother than without OFXR, on Pimax OpenXR or Virtual Desktop since 0.2.13.1 (MSFS 2024 with 3X Frame Gen at a high resolution) | Fixed in 0.2.13.3. Update. At a very high resolution a few frames can still be skipped: pick **NVIDIA Fast** or one step down in your headset's render resolution for more headroom. |
| On 0.2.13.3, stutter and a frame rate below half your refresh rate as soon as a scene gets heavier, on SteamVR (any headset), or on Virtual Desktop and Pimax OpenXR with **Prefer FPS over latency** on; or the overlay stuck at half your refresh rate (The Callisto Protocol) | Fixed in 0.2.13.4. Update. |
| The frame rate drops when a window or HUD panel appears in the headset, and on Virtual Desktop stays low after it closes (CheekyFoveatedDLSS's F8 window, Skyrim VR's HUD on a Steam Frame) | Fixed in 0.2.13.2. Update. If it still happens, send a flight log. |
| Stutter or drop-outs that 0.2.9.1 did not have, with VRAM to spare (seen on an AMD RX 9070 XT in AMS2) | Untick **Lower VRAM (may cause stuttering)** in the tray and restart the game. It restores the layout 0.2.9.1 used, at about 15% more VRAM. |
| You want to know whether a problem comes from OFXR | **Disarm** and restart the game. **Pause frame generation** is not enough: a paused game still runs through OFXR, it only stops making extra frames. |

## Optical flow settings

To make an in-between frame, OFXR first works out how every part of the
image moved between two game frames: the optical flow. Two tray options
change how much GPU time that takes, and both are best left at their
defaults.

- **Optical flow resolution (default 50%)**: the resolution the motion is
  worked out at, as a share of your per-eye resolution on each side. 100%
  is four times as many pixels as 50% and costs about that much more, for
  an image that looks almost the same. NVIDIA Slow at 100% on a Pimax
  Crystal Light at 120 Hz took about 26 ms per frame pair, three times a
  refresh, and played choppier than at 50%.
- **NVIDIA bidirectional consistency (default off)**: works the motion out
  forward and backward and checks one against the other. It slightly
  improves the edges of moving objects and doubles the optical-flow cost.

If the game has little headroom, either one can cost more than the extra
frames give back. **Reset to default** in the tray puts both back to their
defaults, along with the other settings that matter most for performance.

## How much VRAM OFXR uses

The bridge needs extra video memory on top of what the game uses. How much
depends on your per-eye resolution, the game's graphics API, and on which
OpenXR runtime the headset runs through. Since 0.2.10.1 the mode no longer
matters: 2X, 2X with **Prefer FPS over latency** and **3X Frame Gen** cost
the same, on every graphics API. The optical-flow backend makes no real
difference either: NVIDIA and FidelityFX are within 0.01 GB of each other.

Total extra VRAM, both eyes, on SteamVR, Virtual Desktop and the Meta runtime:

| Per-eye resolution | D3D12 | D3D11 | Vulkan |
|---|---|---|---|
| 2064×2208 (4.6 Mpx, Quest 3 class) | 0.45 GB | 0.55 GB | 0.66 GB |
| 3030×2971 (9.0 Mpx) | 0.90 GB | 1.11 GB | 1.33 GB |
| 4172×3268 (13.6 Mpx, Crystal Super) | 1.36 GB | 1.67 GB | 2.00 GB |
| 5040×3948 (19.9 Mpx) | 1.99 GB | 2.44 GB | 2.93 GB |
| 5884×4608 (27.1 Mpx) | 2.72 GB | 3.32 GB | 3.98 GB |

D3D11 and Vulkan games run through a bridge, on by default, that hands the
runtime a D3D12 session; it costs one extra set of images, plus a little
more on Vulkan, where the shared images take more memory than their size.
The figures are worked out from what the bridge allocates and assume a game
rendering 8-bit colour; a game rendering 16-bit colour roughly doubles them.
Measured on SteamVR they hold within 10%: 3.1 GB in MSFS 2024 in 3X at
29 megapixels per eye, and in No Man's Sky about 2.0 GB per swapchain at
27 megapixels. Above the table, count about **0.10 GB per megapixel of
per-eye resolution** on D3D12, 0.12 GB on D3D11 and 0.15 GB on Vulkan.

The table assumes **Lower VRAM** on in the tray, which it is by default.
With it off, the bridge keeps two working images per output as 0.2.9.1 and
earlier did: about 15% more on D3D12 and D3D11 with Prefer FPS over latency
or 3X on. 0.2.9.1 also used about twice as much on Vulkan, which then
mirrored every image instead of sharing it; that part is not affected by
the switch.

> [!WARNING]
> **On Pimax OpenXR, add about half again.** Pimax Play's runtime keeps an
> extra copy of every image it is handed, made the first time the image is
> shown and kept until the game closes, so OFXR's private images cost twice
> on it: measured **4.6 GB against 3.1 GB on SteamVR** at the same
> 29 megapixels per eye in 3X with 0.2.10.1 (7.0 against 4.2 GB with 0.2.9.1,
> which held twice as many images). At the Crystal Super row that is about
> 2.0 GB instead of 1.36 on D3D12 and 2.5 instead of 1.67 on D3D11. SteamVR
> does not do this; the other runtimes have not been measured yet. We have
> reported it to Pimax so they can fix their runtime, and there is nothing
> OFXR can do about it for now. If VRAM is tight, we recommend running your
> Pimax through SteamVR with
> [sboys3's native SteamVR driver](https://store.pimax.com/blogs/blogs/sboys3-native-steamvr-driver-setup-guide),
> especially for MSFS 2024.

**Know your real per-eye resolution.** It is what your VR software asks the
game to render, not the headset's panel and not the "100%" figure. In
SteamVR the global resolution and a per-application resolution **multiply**:
200% global with 199% for the game gave 8192×6412 per eye on a Crystal
Super, four times the 100% figure, and OFXR paid for all of it. The flight
log says what was really asked: the `view_configuration` record near the
top holds the per-eye size.

If you run out of VRAM, lower the per-eye resolution first; it moves every
column of this table and the game's own usage with it.
Since 0.2.10.1, OFXR stays out of **Pimax Home**, which takes the headset
whenever a game leaves VR and used to hold OFXR's buffers at full resolution
the whole time the game was out of VR; earlier versions let it.

## Still stuck? Send a flight log

1. Tick **Bridge flight recorder** in the tray.
2. Start the game, reproduce the problem, play for a minute or two.
3. Use **Open bridge logs** and send the newest file, with your headset, VR
   software (SteamVR, Pimax Play, Virtual Desktop...) and the game's name, on
   [GitHub Issues](https://github.com/djules75/OFXR-Bridge/issues) or Discord.

You can also tick the recorder while the game is already running: it starts
within a moment, and the log still begins with the game's start-up
information. Untick it to close the file. The game hitches once each time.

The recorder keeps only the last part of a long session, so switch it on
shortly before the problem, or reproduce the problem early.
