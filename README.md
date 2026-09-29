# Chiripy

Low-latency YouTube live chat overlay for OBS Studio.

Chiripy is an OBS plugin that shows your YouTube live chat on the canvas in the
style of the chat window from classic online games: a fixed number of text
rows on a translucent panel, `Name: message`, names coloured by role. Messages are
**pushed** from YouTube's streaming API the moment they exist -- there is no
polling and nothing to host. It uses your own YouTube API key, so it costs
nothing, needs no sign-in, and never talks to anyone but Google.

- Works on OBS Studio 31 and later, macOS (Intel and Apple silicon) and Windows.
- Adds a source, **Chiripy Chat**, and a dock, **Chiripy**.
- No cloud service, no account, no telemetry. No support either: this README
  and the plugin's own messages are the documentation.

## Install

Download the file for your system from the
[latest release](https://github.com/coffeerunhobby/chiripy/releases), close
OBS, install, start OBS again.

- **macOS**: open `chiripy-<version>-macos-universal.pkg`. While the package
  is not yet signed, macOS refuses it the first time: open System Settings ->
  Privacy & Security, scroll down and press **Open Anyway**, then run it
  again. It installs into `~/Library/Application Support/obs-studio/plugins/`.
- **Windows**: extract `chiripy-<version>-windows-x64.zip` into
  `C:\ProgramData\obs-studio\plugins\`, so that you end up with
  `C:\ProgramData\obs-studio\plugins\chiripy\bin\64bit\chiripy.dll`.
- **Linux (Ubuntu 24.04)**: `sudo apt install ./chiripy-<version>-x86_64-linux-gnu.deb`.

Then **Docks -> Chiripy** should be in the OBS menu.

## Setup (once, about five minutes)

You need a YouTube Data API key from your own Google Cloud project. This is
free; the daily quota Google gives every project is enough for roughly six
hours of chat per day (see [Quota](#quota)).

1. Open <https://console.cloud.google.com/projectcreate> and create a project
   (any name, e.g. `chiripy`). No billing account is needed.
2. Enable the API: <https://console.cloud.google.com/apis/library/youtube.googleapis.com>
   -- make sure your new project is selected at the top, then **Enable**.
3. Create the key: <https://console.cloud.google.com/apis/credentials> ->
   **Create credentials** -> **API key**. Copy it.
4. Restrict it (recommended): click the key -> **API restrictions** ->
   **Restrict key** -> tick only *YouTube Data API v3* -> Save. Then the key
   can do nothing except read public YouTube data, even if it leaks.
5. In OBS: **Docks -> Chiripy**, paste the key, press **Test key**. It should
   say the key works.

## Using it

1. Add the source: **Sources -> + -> Chiripy Chat**. Drag it where you want
   the chat; dragging a handle resizes the panel (not just scales it), so the
   text stays sharp and re-wraps.
2. Tell Chiripy which stream:
   - If you are **signed in to YouTube inside OBS** (Settings -> Stream ->
     Connect Account) and use *Manage Broadcast*, there is nothing to do:
     Chiripy picks up the broadcast when you press Start Streaming.
   - With a plain **stream key**, paste the video ID or the YouTube Studio URL
     (`studio.youtube.com/video/<ID>/livestreaming`) into the dock and press
     **Save & connect**. YouTube gives every stream a new ID, so this is once
     per stream.
3. Start streaming. The dock says *Connected* and lines appear on the panel
   as viewers type. Stopping the stream disconnects.

Right-click the source -> **Properties** for the look: size, rows (0 = as many
as fit), font size, panel opacity and background, text outline, a colour per
role (owner, moderators, members, viewers) and for message text, and the
greeting shown on the first row.

## FAQ

**Where is the chat? The dock says Connected but the panel is empty.**
Nobody has typed since you connected. On connect, Chiripy replays the last
messages after about ten seconds; new ones appear as they arrive.

**"That video has no active live chat yet. Waiting for it to go live."**
The video exists but YouTube has not marked it live. Chiripy retries every
five seconds; once the stream is live it connects by itself.

**"No video with that ID."** Check the ID against the Studio URL. If you used
an ID from a previous stream, it no longer exists -- every stream gets a new
one.

**"The stream has ended, so its chat is closed."** Start a new stream and
connect again.

**"Live chat is turned off for this stream."** YouTube Studio -> Stream
settings -> enable Live chat.

**"This API key is not allowed to use the YouTube Data API v3."** Step 2 of
the setup was skipped, or the key's API restrictions exclude YouTube Data API
v3.

**"The API key is not valid."** It was mistyped or deleted. Create a new one
(setup step 3) and paste it again.

**"Your YouTube API quota for today is used up."** See [Quota](#quota). It
resets at midnight Pacific time.

**Emoji show as a smiley (U+263A).** YouTube's own emoji and channel emoji
reach the public API as `:shortcode:` text with no image, so Chiripy draws a
placeholder. Regular Unicode emoji (from a phone keyboard) render normally.

**The chat on the overlay appears before viewers see it.** That is the
player's live-stream delay (15-60 s in YouTube's *Normal latency* mode, 2-5 s
in *Ultra low*). Chiripy receives a message at the same moment OBS's YouTube
chat dock does; the overlay is part of the video, so viewers get both
together.

**Where are the logs?** In OBS's normal log (Help -> Log Files), prefixed
`[chiripy]`. Each received message is logged with its delay after YouTube
published it.

**Where is my key stored?** In the plugin's own file --
`~/Library/Application Support/obs-studio/plugin_config/chiripy/config.json`
on macOS, `%APPDATA%\obs-studio\plugin_config\chiripy\config.json` on
Windows -- sealed with AES-256-GCM under a key built into the plugin. That
keeps it out of scene collections and profiles (which people export and
paste) and stops casual reading, but anyone with the plugin and the file can
recover it. The API restriction in setup step 4 is what limits the damage.

## Quota

Google gives every project 10,000 YouTube API units per day. Chiripy's
streaming connection is closed by YouTube every ~10 seconds and reopened,
and each connection costs 5 units, so a connected overlay spends about 1,730
units per hour: **roughly 5.8 hours of chat per day**. Looking up a stream
costs 1 unit. The dock shows an estimate for the current OBS session.

If you stream longer, request a quota increase from Google
(<https://console.cloud.google.com/apis/api/youtube.googleapis.com/quotas>).
Chiripy spends nothing while you are not streaming.

## Privacy

Chiripy sends your API key and the stream's video ID to
`youtube.googleapis.com` and receives chat messages. Nothing is sent
anywhere else, nothing is stored except your settings, and no data leaves
your machine other than to Google. Use of the YouTube API is subject to
YouTube's Terms of Service (<https://www.youtube.com/t/terms>) and Google's
Privacy Policy (<https://policies.google.com/privacy>).

## Building from source

The project is built with [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate):

```bash
cmake --preset macos          # or windows-x64
cmake --build --preset macos
```

The result is in `build_macos/rundir/RelWithDebInfo/chiripy.plugin` (copy it
to `~/Library/Application Support/obs-studio/plugins/`). The only dependency
beyond libobs and Qt is libcurl, taken from the system on macOS and from
obs-deps on Windows. The AES-GCM used for the key store is in-tree and
verified against the GCM specification's test vectors; `tests/` builds
without OBS:

```bash
clang++ -std=c++17 -I src tests/crypto_test.cpp src/crypto/*.cpp src/secret_store.cpp -o crypto_test && ./crypto_test
```

## License

GPL-2.0-or-later. Chiripy is not affiliated with YouTube or Google.
