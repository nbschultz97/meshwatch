using Toybox.System;
using Toybox.Graphics;

// Per-device layout constants.
//
// Connect IQ has no per-product barrel-build system that doesn't add a lot of
// project weight, so for now Layout picks the right table at runtime by
// asking the device for its display dimensions. Constants that genuinely vary
// across the supported round-AMOLED matrix (row pitch, accent color) live
// here. Everything else in the source uses relative offsets (w/2, h/2) and
// auto-scales with the screen.
//
// Supported matrix (all round, all CIQ 5+):
//
//   280 x 280  64-color   fenix6xpro          (tactix Delta class)
//   260 x 260  64-color   fenix7x/xpro/xpronowifi
//   416 x 416  AMOLED     epix2pro51mm        (tactix 7 AMOLED)
//   454 x 454  AMOLED     fenix847mm          (tactix 8 / fenix 8 Pro 47)
//   416 x 416  AMOLED     fenix8solar51mm
//   416 x 416  AMOLED     descentmk351mm
//   416 x 416  AMOLED     marq2 / marq2aviator
//
// Add a new product by adding it to manifest.xml's <iq:products>. If the
// display falls outside the buckets below, Layout defaults to the AMOLED
// table — the watch will render, but row pitch may need a small tweak.
module Layout {

    // Detect 64-color (MIP) vs AMOLED by looking for a high-bit channel that
    // MIP panels can't render. The accent color in AMOLED uses 0xFF in the
    // red byte; MIP dithers it to orange. We don't actually care about the
    // pixel — we care whether to ask for COLOR_ORANGE (64-color) or a true
    // RGB (AMOLED).
    function isAmoled() {
        var w = System.getDeviceSettings().screenWidth;
        if (w == null) { return false; }
        return w >= 416;
    }

    // Row pitch in pixels. Bigger screens want bigger rows so each line is
    // still legible at arm's length. Picked to land near the original 52
    // for the AMOLED matrix; the 280 x 280 MIP uses a tighter row.
    function rowH() {
        return isAmoled() ? 52 : 44;
    }

    // Accent color: AMOLED gets the original amber; 64-color MIP falls back
    // to COLOR_ORANGE because high-channel RGB values dither to muddy
    // browns on a 64-color panel.
    function accentColor() {
        return isAmoled() ? 0xFFB020 : 0xFFAA00;
    }

    // Horizontal x-offset where the row text starts (where the short name
    // is drawn). Round displays have different safe areas; 70 is the value
    // the original AMOLED layout used and is fine for everything 416+; the
    // 280-pixel class needs to start a touch further in.
    function textX() {
        return isAmoled() ? 70 : 56;
    }

    // Top margin under the status line. AMOLED has more vertical room so
    // the gap can be larger; the smaller MIP wants a tighter top.
    function topMargin() {
        return isAmoled() ? 130 : 110;
    }

    // The font used for the headline on the card view. AMOLED has FONT_MEDIUM
    // and FONT_NUMBER_MEDIUM available; the smaller MIP only has up to
    // FONT_SMALL reliably. Caller passes its own font when it cares; this
    // helper exists for places where the original code picked a font based
    // on what's installed.
    function headlineFont() {
        return isAmoled() ? Graphics.FONT_NUMBER_MEDIUM : Graphics.FONT_SMALL;
    }
}
