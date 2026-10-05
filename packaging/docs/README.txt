SPAStrip - Silverplatter Audio
==============================
Version @VERSION@ (@DATE@)

Thank you for buying SPAStrip! SPAStrip is a channel-strip-style effects
chain from Silverplatter Audio: twelve effects (filter, distortion, chorus,
modulation, tremolo/vibrato, glitter, delay, reverb, convolution, EQ,
multiband compressor and limiter) in a chain you can reorder, a sidechain-driven
modulation matrix, a Randomize All button, and nineteen factory impulse
responses for the convolution module.

WHAT'S IN THIS PACKAGE
----------------------
  SPAStrip-@VERSION@-macOS.pkg        macOS installer (Audio Unit and VST3)
  SPAStrip-@VERSION@-Windows.exe      Windows installer (VST3)
  QUICKSTART.txt                      Five minutes to your first sound
  EULA.txt                            The license
  README.txt                          This file

The installers also put CREDITS.txt (the factory impulse response credits,
see below), EULA.txt, README.txt and QUICKSTART.txt on your computer.

SYSTEM REQUIREMENTS
-------------------
  macOS 11 or later (Apple silicon and Intel, universal binary)
  Windows 10 or later (64-bit)
  A DAW that hosts Audio Unit effects (macOS) or VST3 effects.
  There is no standalone application; SPAStrip runs inside your DAW.

INSTALLING THE PLUG-IN
----------------------
macOS:   open the .pkg and follow the installer. You can choose to install
         the Audio Unit, the Audio Unit (MIDI), the VST3, or any of them.
         It installs:
           /Library/Audio/Plug-Ins/Components/SPAStrip.component
           /Library/Audio/Plug-Ins/Components/SPAStrip MIDI.component
           /Library/Audio/Plug-Ins/VST3/SPAStrip.vst3
           /Library/Application Support/Silverplatter Audio/SPAStrip/
               (README.txt, QUICKSTART.txt, EULA.txt, CREDITS.txt)
Windows: run the setup .exe (it asks for administrator permission). It
         installs:
           C:\Program Files\Common Files\VST3\SPAStrip.vst3
           C:\Program Files\Silverplatter Audio\SPAStrip\
               (README.txt, QUICKSTART.txt, EULA.txt, CREDITS.txt)

Quit your DAW before installing, then start it again. Most hosts pick up a
new plug-in on their next start or plug-in scan. If SPAStrip does not show
up, rescan your plug-ins from your DAW's plug-in manager (see your DAW's
manual for where that is).

Uninstalling: on Windows use "Apps" in Settings. On macOS there is no
uninstaller; delete the files listed above.

MIDI LEARN
----------
Right-click any knob, switch or menu and choose MIDI Learn, then move a
control on your MIDI controller: from then on it moves that parameter.
Right-click again to remove the assignment. Your assignments are saved with
your DAW session (not in presets). On an EQ point, the right-click menu has a
MIDI Learn submenu for its frequency, gain and Q.

MIDI Learn needs MIDI to reach the plug-in:
  VST3 (Ableton Live, Cubase, Bitwig, Reaper, FL Studio ...): route a MIDI
    track or your controller to SPAStrip, as your DAW's manual describes.
  Logic Pro: use "SPAStrip MIDI", found under Audio Units > MIDI-controlled
    Effects (Silverplatter Audio), then pick your MIDI source in the
    side-chain MIDI menu at the top of the plug-in window. The regular
    SPAStrip Audio Unit cannot receive MIDI in Logic, so it has no MIDI
    Learn; your DAW's own controller mapping works on every parameter of
    both.

FACTORY IMPULSE RESPONSES AND CREDITS
-------------------------------------
The convolution module comes with nineteen factory impulse responses
(springs, plates, rooms, halls, churches, tanks, tunnels and more). They were
made by other people and are used under their own licences, several of
them Creative Commons Attribution 4.0 (CC BY 4.0), which requires that we
credit the authors. Every author, source, licence and the changes we made are
listed in CREDITS.txt, installed in the folder shown above, and also shown
inside the plugin: open the SPAStrip menu (the logo, top left) and choose
"About SPAStrip...".

PRESETS
-------
SPAStrip does not ship with factory presets yet. Your own presets are saved
in:
  macOS:    ~/Library/Application Support/Silverplatter Audio/SPAStrip/Presets/User
  Windows:  %APPDATA%\Silverplatter Audio\SPAStrip\Presets\User
Presets are single .spastrip files (a user impulse response travels inside
the preset file). Each preset can be given a type: Drums, Bass, Vocals,
Guitar, Keys, Synth, FX, Mixbus, Mastering or Creative. Presets without a type
are listed under "Other". You can also organise presets into your own
folders from the Save dialog, mark favourites, and import presets, a folder
of presets or a .zip of presets with the IMPORT button in the preset
browser. A preset does not store the lock settings, the WILD amount or the
oversampling setting; those stay as you set them for your session.

SIDECHAIN
---------
SPAStrip's modulation matrix is driven by a sidechain envelope follower. In
the SIDECHAIN panel, SOURCE chooses what the follower listens to:
  Input     SPAStrip's own input signal. Nothing needs routing.
  External  the plug-in's sidechain input, which your DAW has to feed.
The sidechain input is off until your DAW enables it, so with External
selected you will see "NO SIDECHAIN INPUT ROUTED" in the panel until you set
up the routing. The LISTEN switch replaces the output with the detector
signal so you can hear what the follower hears.

Setting up an external sidechain:
  Logic Pro   Insert SPAStrip on the track you want to process. At the
              top of the plug-in window header, open the "Side Chain" menu
              and choose the source track or bus.
  Reaper      Add a send from the track you want to listen to, to the
              track holding SPAStrip, and send it to the destination's
              channels 3/4. Make sure the destination track has at least
              four channels. SPAStrip's sidechain bus appears as auxiliary inputs and
              reads channels 3/4.
  Ableton Live, Cubase, FL Studio
              Use the host's sidechain (side-chain) input routing for VST3
              plug-ins, and see your DAW's manual for the steps.
In every host, SOURCE in the SIDECHAIN panel must be set to External for the
routed signal to be used. Input mode keys from the track's own signal and
needs no routing. If your host is hard to route, set SOURCE to Input and put
the signal you want to follow into SPAStrip itself.

OVERSAMPLING AND CPU
--------------------
The OVERSAMPLING menu (1x, 2x, 4x) in the header runs the whole effect chain
at a higher internal sample rate. It is 1x by default. Higher settings can
reduce aliasing in distortion and other nonlinear effects, but the CPU cost
rises with the factor. The convolution module with a long impulse response
at 4x is the heaviest combination: if your CPU meter climbs, lower
oversampling, choose a shorter impulse response, or freeze/bounce the track.
2x and 4x add a few samples of latency, which SPAStrip reports to your DAW.

UNDO
----
The undo and redo buttons in the plug-in header always work. Cmd+Z and
Shift+Cmd+Z (macOS), or Ctrl+Z, Shift+Ctrl+Z and Ctrl+Y (Windows), work when
the plug-in window has keyboard focus, though some hosts keep these shortcuts
for their own undo.

TRIAL, SERIAL AND ACTIVATION
----------------------------
SPAStrip runs as a full 14-day trial, starting the first time you open the
plugin window. After that it switches to demo mode until you activate a
serial: it goes quiet for a moment about once a minute, and saving and
exporting presets are turned off. Loading presets still works, and your
sessions always open either way.

Your serial activates SPAStrip on up to 3 computers. To activate, open the
plugin, click the license badge and enter your serial. Activation takes one
trip online; if the computer is offline, the same panel walks you through
offline activation from another device. Once activated, SPAStrip never needs
to check in with us again.

Moving to a new computer? Deactivate the old one anytime, from the plugin or
from the Licenses view in SPAStation, and activate the new one.

SUPPORT
-------
  support@silverplatteraudio.com
  https://www.silverplatteraudio.com

When you write to us, open the SPAStrip menu, choose "About SPAStrip..." and
press "Copy Info"; paste that into your message. It contains the version,
your operating system and your host, which helps us help you faster.

Made with care by Silverplatter Audio, a boutique sound-effects library
company. Please don't share it around; your support is what keeps products
like this possible.
