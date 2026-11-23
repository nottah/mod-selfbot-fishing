# Playerbots compatibility patch

This patch is based on `mod-playerbots` commit `ecc4a537a3b43056e412fae63fb4a4271849b472` and modifies `CheckMountStateAction.cpp` and `CheckMountStateAction.h`. It prevents remounting while selfbot fishing is active and delays autonomous remounting for five seconds after becoming eligible.

Apply it from the Playerbots repository root:

```sh
git apply /path/to/mod-selfbot-fishing/patches/playerbots-mount-and-fishing.patch
```

The patch is distributed under GPL-2.0-only to match the upstream Playerbots source it modifies; see [`LICENSE`](LICENSE). It is provided separately and does not include or redistribute the full Playerbots source. Review compatibility against the Playerbots revision you use before applying it.
