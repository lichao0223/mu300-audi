# mu300-audio

Call audio for the ZTE F50 (MU300) and ZTE U30 Air running [mu300-linux](https://github.com/dikeckaan/mu300-linux),
installed on the device as the extra **mu300-audio**.

Supported kernels: the vendor 5.4 and the mainline 6.18, 7.2 and 7.3 that mu300-linux ships.

## Install

From a computer, over ssh (the latest release, and the DSP firmware checked against its pinned sha256):

    ./install.sh --host ubuntu@192.168.77.1      # or root@... for OpenWrt; ./uninstall.sh to remove

On the device, with `mu300-audio.tar.gz` and `SHA256SUMS` of a release in DIR:

    sudo mu300-extra install mu300-audio --from DIR
    sudo mu300-audio-dsp install --download      # the DSP firmware
    sudo mu300-extra remove mu300-audio

One tarball serves every kernel; a reboot after installing makes sure the DSP's memory is free.

## Bluetooth headset

Pair and trust the headset with `bluetoothctl`, put its address in `/etc/mu300/voice.conf` (`BT_HEADSET=...`; see
`/etc/mu300/voice.conf.example`) and restart the `mu300-voice` service.

Reports go to the [mu300-linux issues](https://github.com/dikeckaan/mu300-linux/issues).

## Building

    sh kernel/build.sh 6.18                                   # out/modules/6.18/
    for t in pcmhost ctlset hfp; do sh src/$t/build.sh; done  # out/bin/
    sh tools/make-extra.sh out/release v1.0.0                 # out/release/mu300-audio.tar.gz
    cd tests && python3 -m unittest discover -s .

For MU300 Linux **v2026.11.02.1**, push branch `ci/audio-v2026.11.02.1` to run
**Audio for Linux v2026.11.02.1** in GitHub Actions. It rebuilds the release's 5.4, 6.18 and 7.3 kernels on arm64,
checks each kit against the published kernel, then builds the audio drivers and tools. Verified kits are cached.
Download the artifact **mu300-audio-v2026.11.02.1** for `mu300-audio.tar.gz`, `SHA256SUMS`, `VERSIONS`, the generated
`kits.lock` and source commit IDs (`BUILD_INFO`); artifacts are kept for 14 days. Use the on-device `--from DIR`
installation above with the extracted files. Kernel and module compilation happens entirely in CI.

## Credits

* **kano (kanoqwq)** and [e5-linux](https://github.com/kanoqwq/e5-linux) (MIT, © 2026 Enceka): parts of this work
  are ported from there; each ported piece names its e5-linux commit.
* **Enceka**: [linux-lts-e5](https://github.com/Enceka/linux-lts-e5), which the mainline port drew on.
* Unisoc wrote the audio drivers (GPL-2.0).

## Licence

Two licences, see [LICENSE](LICENSE): **GPL-2.0** ([kernel/COPYING](kernel/COPYING)) for the drivers - their sources,
patches and builds - and the build kits; **MIT** for the scripts, tools and tests. No firmware or vendor parameter
file is in this repository or its releases, and none may be added.
