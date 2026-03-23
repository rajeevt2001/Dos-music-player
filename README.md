# Dos-music-player
Dos-music-player is an audio/music player inspired by MPX Play and Windows Media Player, built entirely from scratch in C++, compiled with DJGPP, and with the help of Google Gemini for classic MS-DOS environments. It brings a modern, Windows-like GUI experience into a pure 80x25 text-mode terminal. Yes, I know that the whole program is made with the help of AI. Because I'm not good at coding at this level, I only know how to design the user interface, and I love a DOS and retro-futuristic aesthetic.


Main Screen with debug visualizer, running under DOSBox

![Main Screen, running under DOSBox](screenshots/Main_view_with_visualizer_1.png)

Main Screen with Bars visualizer, running under DOSBox

![Main Screen with bars visualizer, running under DOSBox](screenshots/Main_view_with_visualizer_2.png)

Main Screen with VU Meter visualizer, running under DOSBox

![Main Screen with vu meter visualizer, running under DOSBox](screenshots/Main_view_with_visualizer_3.png)

Menus preview

![Menus](screenshots/file_menu.png)  .  ![Menus](screenshots/audio_menu.png)  .  ![Menus](screenshots/visual_menu.png)

# Features

Core Playback Engine

 - Dual-format audio support for MP3 (MPEG Audio) streaming and native WAV (Uncompressed PCM) playback.

 - Hardware-level Sound Blaster routing with support for SB16 and SB Pro 2.0 DSPs.

 - Motherboard PC Speaker overdrive engine using custom PWM timing for systems without dedicated sound cards.

 - 486 Transcoder Cache to dynamically decode MP3s into WAV files for playback on slower legacy CPUs.

 - Global playback controls including play, pause, stop, 5-second seeking, looping, and volume adjustment.

Custom VRAM Interface

 - Custom graphical window manager running natively in standard DOS 80x25 text mode.

 - Full mouse support for interacting with tabs, drop-down menus, and scrollbars.

 - Customizable UI themes with adjustable background, foreground, selection, and critical colors.
 
 - Toggleable ANSI and Extended ASCII drawing modes for maximum hardware compatibility.

Audio Visualization & Telemetry

 - Real-time "Frequency Bars" visualizer powered by live Fast Fourier Transform (FFT) math.

 - Dynamic VU Meter with customizable peak falloff speeds and multi-color warning zones.

 - Hardware Subsystem Telemetry dashboard displaying live IRQ hits, DMA buffer states, frame counts, and active sample rates.

Media & Queue Management

 - Integrated File Browser with directory navigation, text-based search filtering, and quick drive selection.

 - Comprehensive M3U playlist support allowing users to create, load, save, and modify track queues.

 - Advanced Media Info view with extensive ID3 tag parsing for Artist, Title, Album, Year, Genre, BPM, Composer, and Encoding data.


# Settings page

Audio Settings

![Settings](screenshots/audio_settings.png)

Graphics settings with theme settings

![Settings](screenshots/graphics_setting.png)

Visual settings

![Settings](screenshots/visual_settings.png)

Other settings will be available in the later versions

![Settings](screenshots/other_settings.png)

Display settings will be available in the later versions


# Some preview screenshots with different theme setting applied

![Settings](screenshots/graphics_settings_with_theme_applied.png)

Compatibility Ansi-only Mode

![ansi_mode](screenshots/ansi_mode2.png)
