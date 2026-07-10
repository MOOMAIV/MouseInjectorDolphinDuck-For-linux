# Mouse Injector Linux Makefile
# Build: make
# Build with X11 support (recommended): make HAVE_X11=1
# Build without X11 (Wayland/headless): make HAVE_X11=0

CC        = gcc
SRCDIR    = ./
MANYMOUSE = $(SRCDIR)manymouse/
GAMESDIR  = $(SRCDIR)games/
OBJDIR    = $(SRCDIR)obj/
EXENAME   = mouseinjector

# Compiler flags
CFLAGS    = -O2 -std=c99 -Wall -Wextra -pedantic -Wno-parentheses -D_GNU_SOURCE
LIBS      = -lm

# X11 support (enables global keyboard polling and cursor warp)
HAVE_X11 ?= 1
ifeq ($(HAVE_X11),1)
  CFLAGS += -DHAVE_X11
  LIBS   += -lX11
  $(info Building with X11 support)
else
  $(info Building without X11 (terminal-only keyboard, no cursor warp))
endif

# Source files
OBJS = $(OBJDIR)main.o \
       $(OBJDIR)memory.o \
       $(OBJDIR)mouse.o \
       $(OBJDIR)manymouse.o \
       $(OBJDIR)linux_evdev.o

# Conditionally add XInput2 backend
ifeq ($(HAVE_X11),1)
  OBJS += $(OBJDIR)x11_xinput2.o
  LIBS += -lXi
endif

GAMEOBJS = $(patsubst $(GAMESDIR)%.c, $(OBJDIR)%.o, $(wildcard $(GAMESDIR)*.c))

# Targets
.PHONY: all clean install

all: $(OBJDIR) $(EXENAME)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(EXENAME): $(OBJS) $(GAMEOBJS)
	$(CC) $(OBJS) $(GAMEOBJS) -o $(EXENAME) $(LIBS) -s

# Core objects
$(OBJDIR)main.o: $(SRCDIR)main.c $(SRCDIR)main.h $(SRCDIR)memory.h $(SRCDIR)mouse.h $(GAMESDIR)game.h
	$(CC) -c $(SRCDIR)main.c -o $@ $(CFLAGS)

$(OBJDIR)memory.o: $(SRCDIR)memory.c $(SRCDIR)memory.h $(SRCDIR)main.h
	$(CC) -c $(SRCDIR)memory.c -o $@ $(CFLAGS)

$(OBJDIR)mouse.o: $(SRCDIR)mouse.c $(SRCDIR)mouse.h $(MANYMOUSE)manymouse.h
	$(CC) -c $(SRCDIR)mouse.c -o $@ $(CFLAGS)

# ManyMouse
$(OBJDIR)manymouse.o: $(MANYMOUSE)manymouse.c $(MANYMOUSE)manymouse.h
	$(CC) -c $(MANYMOUSE)manymouse.c -o $@ $(CFLAGS)

$(OBJDIR)linux_evdev.o: $(MANYMOUSE)linux_evdev.c $(MANYMOUSE)manymouse.h
	$(CC) -c $(MANYMOUSE)linux_evdev.c -o $@ $(CFLAGS) -Wno-unused-parameter -Wno-format-truncation

$(OBJDIR)x11_xinput2.o: $(MANYMOUSE)x11_xinput2.c $(MANYMOUSE)manymouse.h
	$(CC) -c $(MANYMOUSE)x11_xinput2.c -o $@ $(CFLAGS) -Wno-sign-compare

# Game drivers (all .c files in games/)
# -include injects game_compat.h so upstream files don't need #include <math.h> etc.
$(OBJDIR)%.o: $(GAMESDIR)%.c $(SRCDIR)main.h $(SRCDIR)memory.h $(SRCDIR)mouse.h $(GAMESDIR)game.h $(GAMESDIR)game_compat.h
	$(CC) -c $< -o $@ $(CFLAGS) -include $(GAMESDIR)game_compat.h

clean:
	rm -f $(OBJDIR)*.o $(EXENAME)

install: $(EXENAME)
	install -m 755 $(EXENAME) ~/.local/bin/
