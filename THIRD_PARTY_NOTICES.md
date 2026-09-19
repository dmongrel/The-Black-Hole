# Third-party notices

The Black Hole builds three pieces of third-party work into `the-black-hole.scr`: the Michroma
typeface, two NASA images of the Earth, and the volk Vulkan loader.

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

## NASA Blue Marble and Black Marble

The Earth that one credit brings on is textured with two NASA images, which are in the public
domain as works of the United States government. NASA does not endorse this project.

- Surface: *Blue Marble: Next Generation*, December 2004, with topography and bathymetry
  (NASA Earth Observatory, Reto Stöckli), from
  https://eoimages.gsfc.nasa.gov/images/imagerecords/73000/73909/world.topo.bathy.200412.3x5400x2700.jpg
- City lights: *Black Marble 2016* (NASA Earth Observatory, Joshua Stevens, from Suomi NPP VIIRS
  data by Miguel Román, NASA GSFC), from
  https://eoimages.gsfc.nasa.gov/images/imagerecords/144000/144898/BlackMarble_2016_01deg.jpg

Both are scaled to 2048x1024. The city lights are converted to grey and thresholded, so that only
the lights remain and the dark ground is black. They are embedded in the `.scr` as resources:

    file    assets/earth/earth-day.jpg
    sha256  a68b3b7f718b2611639c8da8c552fff123a7214cd1a10e462ac1ddce54f71b1c
    bytes   325288

    file    assets/earth/earth-night.jpg
    sha256  444d9d2bfe258d38863d2885f820f8766c0db64f568cf4f5ec3b1c0c5b909055
    bytes   70905

## volk

Copyright (c) 2018-2026 Arseny Kapoulkine (https://github.com/zeux/volk). MIT License; the
licence text is `third_party/volk/LICENSE.md`. volk is vendored unmodified and compiled into the
`.scr`. It loads `vulkan-1.dll` at run time.
