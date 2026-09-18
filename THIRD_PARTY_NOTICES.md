# Third-party notices

The Black Hole builds two pieces of third-party work into `the-black-hole.scr`: the Michroma
typeface and the volk Vulkan loader.

## Michroma

Copyright 2011 The Michroma Project Authors (https://github.com/googlefonts/Michroma-font)

Licensed under the SIL Open Font License, Version 1.1. The full licence is
`assets/fonts/Michroma-OFL.txt`. It is embedded in the `.scr` as a resource next to the font, and
the Settings dialog shows it with the **Font licence** button. OFL section 2 requires the licence
to travel with the font, and a licence left in the repository would not reach anyone who has only
the screen saver.

The font sets the credits played over the black hole. It is loaded from the `.scr` for the screen
saver's own process with `AddFontMemResourceEx`, and is never installed on the machine. The file
is version 1.100, taken unmodified from `google/fonts@main/ofl/michroma`, the same copy GoLLM
vendors:

    file    assets/fonts/Michroma-Regular.ttf
    sha256  b62301163788bc5b7f8fcac0b74b184e34e1827e577b499ecb724da065098f87
    bytes   64344

The font has not been subsetted, renamed or otherwise modified. It declares no Reserved Font Name.

## volk

Copyright (c) 2018-2026 Arseny Kapoulkine (https://github.com/zeux/volk). MIT License; the
licence text is `third_party/volk/LICENSE.md`. volk is vendored unmodified and compiled into the
`.scr`. It loads `vulkan-1.dll` at run time.
