# Into the Dead – PS Vita port (work in progress)

A loader that runs the Android version of *Into the Dead* (PikPok, Unity
2020.3, IL2CPP, armeabi-v7a) on the PS Vita. It started from the
[Ninja Arashi port](https://github.com/RONIN0095/Ninja-Arashi-PS-Vita): the
same loader (so_util, FalsoJNI, vitaGL with the patches in `extras/patches`),
adapted to this game and its older Unity version.

No game files are in this repository. You need your own copy of the game.

## Status

First builds: getting the game to start. Not playable yet.

## Installing

1. Install `kubridge.skprx` and `libshacccg.suprx` as for other Android ports.
2. Copy your APK (version 2.7.1) to `ux0:data/intothedead/base.apk`.
3. Install the VPK from the `builds` branch.

The game's libraries and data are taken from the APK on the first start.
`ux0:data/intothedead/log.txt` is the log.

## Building

GitHub Actions builds the VPK on every push (`.github/workflows/build.yml`,
VitaSDK-softfp) and publishes it to the `builds` branch.
