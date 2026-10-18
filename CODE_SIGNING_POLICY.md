# Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io/),
certificate by [SignPath Foundation](https://signpath.org/).

## What is signed

- `chiripy.dll`, the OBS Studio plugin for Windows.
- `chiripy-<version>-windows-x64-installer.exe`, the Windows installer.

Nothing else is signed under this subscription. The plugin links against
`libcurl.dll` shipped by OBS Studio itself; no upstream binary is bundled or
signed by this project. macOS packages are signed and notarized separately
with an Apple Developer ID when available.

## Where signed builds come from

- Source: <https://github.com/coffeerunhobby/chiripy>, the only official
  repository.
- Builds: the GitHub Actions workflow in this repository, run on a release
  tag (`X.Y.Z` or `X.Y.Z-betaN`), from the tagged source and the build
  definitions committed with it. Every release page lists the SHA-256 of
  each file.
- Signing: each release is submitted through SignPath's GitHub Actions
  integration and approved by hand before publication. Locally built or
  modified binaries are never signed.

## Team roles

| Role | Member |
|---|---|
| Author and committer | [coffeerunhobby](https://github.com/coffeerunhobby) |
| Reviewer | [coffeerunhobby](https://github.com/coffeerunhobby) |
| Approver | [coffeerunhobby](https://github.com/coffeerunhobby) |

All members use multi-factor authentication on GitHub and on SignPath.

## Verifying a download

On Windows, right-click the installer or `chiripy.dll`, open **Properties
-> Digital Signatures**, and check that the signer is **SignPath
Foundation**. Compare the file's SHA-256 with the checksum on the release
page.

## Privacy

This program will not transfer any information to other networked systems
unless specifically requested by the user or the person installing or
operating it. When connected, Chiripy sends the API key and the video ID the
user entered to `youtube.googleapis.com` and receives chat messages; nothing
goes anywhere else. Use of the YouTube API is subject to YouTube's Terms of
Service (<https://www.youtube.com/t/terms>) and Google's Privacy Policy
(<https://policies.google.com/privacy>). The full statement is in the
[README](README.md#privacy).
