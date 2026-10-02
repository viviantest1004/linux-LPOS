// lp-graphics.js - how Firefox ESR draws, scrolls, takes touch and picks
// fonts on this desktop. Installed as /etc/firefox-esr/lp-graphics.js.
//
// Debian's firefox-esr reads every *.js file in /etc/firefox-esr/ as a
// default-preferences file (see firefox-esr.js in the same directory), so
// these are DEFAULTS: a person can still change any of them in
// about:config or Settings, and their choice wins and survives upgrades.
// Nothing here is locked.
//
// Written against Firefox ESR 140 (bookworm's firefox-esr). Every name
// below was checked to exist in that build's libxul; a pref Firefox does
// not know is silently ignored, which is how a file like this rots. Two
// names that older guides give are gone in 140 and deliberately absent:
// media.ffmpeg.vaapi.enabled (VA-API is now governed by
// media.hardware-video-decoding.enabled plus Firefox's per-GPU blocklist)
// and gfx.webrender.all (WebRender is the only renderer now - on the HD
// 530 it runs on the GPU, in a virtual machine Firefox picks its software
// WebRender, and forcing GPU WebRender there onto llvmpipe is slower).
//
// Colour management of the wide-gamut panel is set elsewhere, in
// /usr/lib/firefox-esr/lp-firefox.cfg, because it depends on whether the
// session found a panel profile (LP_DISPLAY_ICC) and a static file cannot
// ask that.

// ── Video decoding ────────────────────────────────────────────────
// Hardware decoding through VA-API (iHD on the HD 530: H.264, HEVC 8-bit,
// VP8, MPEG-2, VC-1). On by default in 140 for Intel with Mesa; stated so
// that a stray user.js from an older profile cannot leave it off.
// Skylake has no fixed-function VP9 or AV1 decoder, so YouTube (which
// prefers those) still decodes on the CPU - README.md, "Video", has the
// per-person switch that trades 4K for H.264 hardware decoding.
pref("media.hardware-video-decoding.enabled", true);

// ── Scrolling that moves like the rest of the desktop ─────────────
// COMMON Motion: springs, not fixed timelines, and interruptible.
// msdPhysics is exactly that - a critically damped mass-spring-damper
// that retargets from its current position and velocity when another
// wheel or key scroll arrives mid-flight, instead of restarting a fixed
// 150-400 ms curve. Its default spring constants (600 at the start of a
// motion, 1000 while it continues) sit next to the design system's
// window/slide spring (k=602, zeta 1.00, 340 ms), so they are left alone.
pref("general.smoothScroll", true);
pref("general.smoothScroll.msdPhysics.enabled", true);

// Touchpad: kinetic (fling) scrolling from GTK's pan gestures, and the
// rubber-band bounce at the end of a page (the design system's "rubber"
// spring does the same for our own lists).
pref("apz.gtk.kinetic_scroll.enabled", true);
pref("apz.gtk.pangesture.enabled", true);
pref("apz.overscroll.enabled", true);

// ── Touch screen ──────────────────────────────────────────────────
// 1 = touch events on, not "autodetect": detection looks for a touch
// device when Firefox starts, and on a convertible that can be the moment
// nothing is reported yet. Sites then get real touch events, and
// Firefox's own async pan/zoom (APZ) follows the finger 1:1 and flings
// on release.
pref("dom.w3c_touch_events.enabled", 1);
// Pinch to zoom, on the screen and on the touchpad.
pref("apz.allow_zooming", true);
pref("apz.gtk.touchpad_pinch.enabled", true);

// ── Dialogs ───────────────────────────────────────────────────────
// Firefox here is not sandboxed (no Flatpak), so the desktop portal would
// only add a D-Bus round trip and a dialog drawn by another process in
// another style. GTK's own file chooser runs in-process with our theme.
pref("widget.use-xdg-desktop-portal.file-picker", 0);
pref("widget.use-xdg-desktop-portal.mime-handler", 0);

// ── Fonts ─────────────────────────────────────────────────────────
// Family names are left at Firefox's Linux defaults (empty = ask
// fontconfig for the generic with the page's language), which is what
// makes /etc/fonts/local.conf apply to the web too: sans-serif is
// Pretendard, serif Noto Serif CJK KR, monospace D2Coding, and a Korean
// page gets Korean fallbacks. Emoji already default to Noto Color Emoji.
//
// What does need saying is which language untagged Han characters are.
// A page that does not declare its language and contains 漢字 gets the
// glyph shapes of the first CJK locale in this list; Firefox's default
// puts Simplified Chinese first, which draws Hanja in mainland Chinese
// forms. The person using this machine reads Korean.
pref("font.cjk_pref_fallback_order", "ko,ja,zh-cn,zh-hk,zh-tw");

// ── Autoconfig ────────────────────────────────────────────────────
// Loads /usr/lib/firefox-esr/lp-firefox.cfg (colour management, see the
// top). obscure_value 0 means the file is plain text, not byte-shifted.
pref("general.config.filename", "lp-firefox.cfg");
pref("general.config.obscure_value", 0);
