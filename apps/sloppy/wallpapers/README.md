# Wallpaper sources

- `orion.jpg`: [Orion Nebula photograph by Bryan Goff](https://unsplash.com/photos/orion-nebula-astrophotography-f7YQo-eYHdM), used under the [Unsplash License](https://unsplash.com/license). Cropped to a portrait view for the 480×800 screen.
- `pillars.png`: [Pillars of Creation (NIRCam) in the Eagle Nebula](https://science.nasa.gov/asset/webb/pillars-of-creation-nircam-image/). Credit: NASA, ESA, CSA, STScI; image processing by Joseph DePasquale, Anton Koekemoer, and Alyssa Pagan (STScI). Cropped to fit 480×800.
- `red_roots.png`: original generated artwork for this project.
- `nexus_background.png`: the original [AOSP Nexus Live Wallpaper pyramid background](https://android.googlesource.com/platform/packages/wallpapers/Basic/+/refs/heads/eclair-passion-release/res/drawable-hdpi/pyramid_background.png), cropped to the screen. The four-color moving pulse effect follows the [original AOSP implementation](https://android.googlesource.com/platform/packages/wallpapers/Basic/+/refs/heads/eclair-passion-release/res/raw/nexus.rs), adapted for GabagoolOS and LVGL. AOSP's license text is in `NEXUS-NOTICE`.

Run `python3 make_embedded.py orion pillars red_roots nexus_background` on macOS to regenerate the RGB565 arrays and picker previews from these source images. The included arrays are the assets rendered by GabagoolOS; source images are retained for future crops.
