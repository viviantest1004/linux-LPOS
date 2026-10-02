# desktop/branding/lp-splash-fade/Makefile - the session's first frame,
# which fades the boot splash into the desktop. See lp-splash-fade.c.
#
# Built in the Debian base chroot like every shell component (shell.mk).
# The picture itself is userland/splash/scene.h - the boot splash's own
# drawing code, shared so the two cannot disagree by a pixel - so that
# directory is on the include path and both headers are dependencies.
BIN          := lp-splash-fade
SRC          := lp-splash-fade.c
SPLASH       := ../../../userland/splash
EXTRA_LDLIBS := -lpthread
include ../../common/shell.mk
CFLAGS       += -I$(SPLASH)
$(BIN): $(SPLASH)/scene.h $(SPLASH)/logo.h
