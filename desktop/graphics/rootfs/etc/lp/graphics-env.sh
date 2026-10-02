# graphics-env.sh, installed as /etc/lp/graphics-env.sh - the graphics
# half of the session's environment: which GPU draws, which one is left
# asleep, and which toolkits are told to talk Wayland. Sourced
# (". /etc/lp/graphics-env.sh") by session-run after its own exports and
# before it starts the compositor, so that everything the compositor
# launches inherits it.
#
# Why it looks at the hardware instead of exporting fixed values.
#
# session-run exports WLR_RENDERER=pixman, LIBGL_ALWAYS_SOFTWARE=1 and
# GSK_RENDERER=cairo, because the machines it has been tested on are
# virtual and have no GPU: left to discover that, wlroots tries EGL on
# virtio_gpu and crashes. On the XPS 15 those same three lines would draw
# a 3840x2160 desktop on the CPU: every window animation, every scrolled
# page, every frame of video composited by pixman at 8.3 million pixels a
# frame, while an Intel HD 530 that does this for a living sits idle. So
# the choice is made per machine, here: a GPU with a real 3D driver (i915,
# amdgpu, radeon, nouveau) gets the GL renderer, anything else (virtio_gpu
# without virgl, bochs, simpledrm) keeps what session-run set.
#
# Why WLR_DRM_DEVICES names one card.
#
# wlroots opens EVERY GPU with a KMS driver and sets up a renderer on each
# one for multi-GPU output. On the XPS that includes the GTX 960M, which
# has no outputs at all (the panel, HDMI and Thunderbolt are wired to the
# Intel GPU), and a compositor holding it open with a GL context keeps it
# out of runtime suspend - the dGPU would sit in D0 drawing watts for as
# long as the desktop runs. Naming only the card the firmware lit the
# screen with (boot_vga) keeps the compositor off the NVIDIA GPU entirely,
# so nouveau can put it in D3cold five seconds after boot. lp-gpu-run is
# the way to use it on purpose.
#
# Constructs. /bin/sh on this system is our own shell, and the build of it
# this was tested with has no `return`, no `command -v`, no bracket
# expressions in globs, does not glob a word that is partly quoted, runs
# the assignment in "false && x=1" anyway, and splits a comment at a
# semicolon inside a loop. So: comments here have no semicolons, every
# conditional assignment is an if, globs are unquoted (sysfs paths have
# no spaces), and nothing else is used but for, case, test and $( ). It
# was run under that shell, dash and bash with the same results. Every helper variable starts with lp_gfx_ and is unset at
# the end, because this file runs inside session-run's own shell.

# ── Native Wayland everywhere ──────────────────────────────────────
#
# On a scale-2 output an Xwayland window is rendered at 1x and stretched,
# so it is blurry. A native Wayland window is rendered at 2x. These make
# each toolkit pick Wayland first. The X11 fallbacks after the comma only
# matter if Xwayland is ever turned on (it is off in wayfire.ini).
export MOZ_ENABLE_WAYLAND=1                 # Firefox (the default since 121, kept explicit)
export GDK_BACKEND=wayland,x11              # GTK 3 and 4
export QT_QPA_PLATFORM='wayland;xcb'        # Qt 5 needs qtwayland5 for this, Qt 6 qt6-wayland
export SDL_VIDEODRIVER=wayland              # SDL 2 games
export CLUTTER_BACKEND=wayland
export ELECTRON_OZONE_PLATFORM_HINT=auto    # Electron 28+: Wayland when WAYLAND_DISPLAY is set
export _JAVA_AWT_WM_NONREPARENTING=1        # Java/AWT draws blank windows without it

# The pointer. 24 logical pixels = 48 on the panel at scale 2, and the
# Adwaita theme has a hand-drawn 48px size, so nothing is resampled. The
# same pair is in wayfire.ini / sway (compositor), settings.ini and the
# GSettings override (GTK), and here (Xwayland and everything else that
# loads a cursor itself), so the pointer does not change size between
# windows.
export XCURSOR_THEME=Adwaita
export XCURSOR_SIZE=16

# ── Which GPU draws ────────────────────────────────────────────────
lp_gfx_card=
lp_gfx_render=
lp_gfx_driver=
lp_gfx_isboot=0
for lp_gfx_c in /sys/class/drm/card*; do
    # card* and not card[0-9]*: our shell's globbing has no bracket
    # expressions yet. Connectors (card0-eDP-1) are skipped by name, and
    # an unmatched glob stays literal and fails the test after it.
    case "${lp_gfx_c##*/}" in
    *-*) continue ;;
    esac
    test -e "$lp_gfx_c/device/driver" || continue
    lp_gfx_d=$(readlink "$lp_gfx_c/device/driver")
    lp_gfx_d=${lp_gfx_d##*/}
    case "$lp_gfx_d" in
    i915|xe|amdgpu|radeon|nouveau) ;;
    *) continue ;;
    esac
    lp_gfx_b=0
    if test -r "$lp_gfx_c/device/boot_vga"; then
        read -r lp_gfx_b < "$lp_gfx_c/device/boot_vga"
    fi
    # Take the first capable card, and let a boot_vga card replace a
    # non-boot one. Probe order decides card numbers, the firmware
    # decides which GPU the panel hangs off.
    lp_gfx_take=0
    if test -z "$lp_gfx_card"; then
        lp_gfx_take=1
    fi
    if test "$lp_gfx_b" = 1; then
        if test "$lp_gfx_isboot" != 1; then
            lp_gfx_take=1
        fi
    fi
    if test "$lp_gfx_take" = 1; then
        lp_gfx_card=${lp_gfx_c##*/}
        lp_gfx_driver=$lp_gfx_d
        lp_gfx_isboot=$lp_gfx_b
        lp_gfx_render=
        # Unquoted on purpose, see "Constructs" above.
        # shellcheck disable=SC2231
        for lp_gfx_r in $lp_gfx_c/device/drm/renderD*; do
            if test -e "$lp_gfx_r"; then
                lp_gfx_render=${lp_gfx_r##*/}
            fi
        done
    fi
done

if test -n "$lp_gfx_card"; then
    # wlroots' GLES2 renderer on the real GPU, and only that GPU.
    export WLR_RENDERER=gles2
    export WLR_DRM_DEVICES="/dev/dri/$lp_gfx_card"
    unset WLR_RENDERER_ALLOW_SOFTWARE
    unset LIBGL_ALWAYS_SOFTWARE
    # GTK 4 picks its GL renderer by itself when GL works and falls back
    # to cairo when it does not, so unsetting beats naming one.
    unset GSK_RENDERER
    if test -n "$lp_gfx_render"; then
        # Firefox's VA-API and dmabuf paths open this node rather than
        # whichever renderD128 happens to be (which could be nouveau).
        export MOZ_DRM_DEVICE="/dev/dri/$lp_gfx_render"
    fi
    # VA-API: iHD (intel-media-va-driver-non-free) is the one that decodes
    # HEVC on Skylake, i965 is the free fallback. libva would guess the
    # same order, but naming a driver that exists turns "no video
    # acceleration" into a clear error instead of a silent software path.
    if test "$lp_gfx_driver" = i915; then
        if test -e /usr/lib/x86_64-linux-gnu/dri/iHD_drv_video.so; then
            export LIBVA_DRIVER_NAME=iHD
        elif test -e /usr/lib/x86_64-linux-gnu/dri/i965_drv_video.so; then
            export LIBVA_DRIVER_NAME=i965
        fi
    fi
    export LP_GPU="$lp_gfx_driver"
else
    export LP_GPU=software
fi

# ── Colour ─────────────────────────────────────────────────────────
#
# The XPS panel is wide-gamut: sRGB colours sent to it untransformed look
# oversaturated. Nothing in wlroots 0.15 can correct that for the whole
# screen, but Firefox can for web content if it is given a profile of the
# panel. lp-display-icc builds one from the panel's own EDID (once - it is
# cached under ~/.local/share/icc and rebuilt only if the EDID changes)
# and prints its path, and /usr/lib/firefox-esr/lp-firefox.cfg reads it from
# LP_DISPLAY_ICC. No internal panel, no usable EDID: nothing is exported
# and Firefox stays as it is. See desktop/graphics/README.md, "Colour".
if test -x /usr/local/bin/lp-display-icc; then
    lp_gfx_icc=$(/usr/local/bin/lp-display-icc 2>/dev/null)
    if test -n "$lp_gfx_icc"; then
        if test -e "$lp_gfx_icc"; then
            export LP_DISPLAY_ICC="$lp_gfx_icc"
        fi
    fi
fi

unset lp_gfx_card lp_gfx_render lp_gfx_driver lp_gfx_isboot lp_gfx_c lp_gfx_d
unset lp_gfx_b lp_gfx_take lp_gfx_r lp_gfx_icc
