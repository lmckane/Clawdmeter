# S3 pet animation library

The Waveshare ESP32-S3-Touch-AMOLED-2.16 and ESP32-S3-Touch-LCD-4 builds add nine small, authored
animation scenes to Clawdmeter's 17 existing pixel-art scenes. They share the
existing full-size performer, stage geometry, theme recoloring, and PSRAM
image buffer. The C6 build and all other boards retain their original art.

| Scene | Local source | Used for |
| --- | --- | --- |
| pet breathe, pet look around | `claude-desktop-pet/assets/idle-*.svg` | gentle idle and calm scenes |
| pet thinking, pet typing | `mascot-studio/public/mascot/*.svg` | active scenes; thinking on Codex prompt |
| pet waiting, pet greeting | same | heavy / normal scenes; greeting on shake |
| pet asleep | same | calm screensaver dip |
| pet done | same | heavy scene; Codex stop event |
| pet coffee | same | normal scene; alternate shake reaction |

`tools/convert_pet_svg.py` samples the SVG's discrete rectangle frames into
the fixed 55 × 37 Clawdmeter pixel stage and crops each scene's transparent
bounds. It uses only the Python standard library. To regenerate from local
checkouts, run `python3 tools/convert_pet_svg.py`; `--desktop`, `--mascot`, and
`--output` override the default paths. The generated
`firmware/src/pet_animations.h` is checked in for reproducible builds.

On either S3 board, a Codex prompt can start the thinking scene and a stop
event can start the done scene when Clawd is free and the meter is live. An
intentional three-peak shake starts greeting or coffee on the AMOLED-2.16.
The LCD-4 has no IMU; its serial command `clawd pet breathe` can preview a
scene even before BLE data arrives (replace `breathe` with any scene name in
the table). Existing touches still open the curious face or dismiss a show;
rotation follows the IMU on the AMOLED-2.16. Alerts
still take priority. The usual usage-based shows and calm screensaver dips can
also select the new scenes. The pet scenes are part of the splash-style
full-size performer, not an additional simultaneous Clawd.

Requested pet scenes skip the meter show's pointing preamble, walk on and off
at 250% of the authored walking pace, and play activity frames at 135%; calm
breathing, look-around, and sleep frames keep their source timing. On the
LCD-4, the RGB driver uses 20-line DMA bounce buffers and the corner sprite
invalidates its previous and current screen regions. Both changes address
stale rows seen on the physical LCD-4; a full LVGL screenshot alone did not
show those panel artifacts.

The selected SVGs are from the local `claude-desktop-pet` and
`mascot-studio` checkouts. Their NOTICE files say that redistribution rights
for the character artwork have not been established. Keep this derived art
private until permission is verified. The converter code can be reused
separately from the artwork.

This provides an animation and interaction foundation. Persistent pet needs,
feeding, and care progression are not implemented here.
