/**************************************************************************
 *
 *  fcitx-tablet : graphics tablet input for fcitx input method framework
 *  Copyright 2012  Oliver Giles
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 **************************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <X11/Xlib.h>
#include <fcitx/module.h>
#include <fcitx-utils/log.h>
#include <fcitx/context.h>
#include <fcntl.h>
#include "pen.h"
#include "config.h"
#include "driver.h"
#include "ime.h"

// pen.c
// This file contains the fcitx event module. It's needed to add an fd to
// the main select() loop to catch events from the tablet device. It also
// contains routines for drawing on the X display and implements a timeout
// to instruct the IME to commit the most likely candidate

// Persistent storage for data relating to X drawing
typedef struct {
	Display* dpy;
	GC gc;
	XGCValues gcv;
} TabletX;

// Persistent storage for data relating to the tablet driver
typedef struct {
	FcitxTabletDriver* drv;
	void* userdata; // the driver's persistent data
	char* packet; // buffer for a packet from the driver
	int fd;
} TabletDriver;

// Persistent storage for the actual strokes. The points should be
// scaled to screen resolution boundaries before being stored here
typedef struct {
	pt_t* buffer; // start of buffer
	pt_t* ptr; // moving pointer
	unsigned n;
} TabletStrokes;

// Wrapper struct to hold all the above
typedef struct {
	FcitxTabletConfig conf;
	TabletX x;
	TabletDriver driver;
	TabletStrokes strokes;
	FcitxInstance* fcitx;
} FcitxTabletPen;

// An interface for the IME, which needs the strokes in order to call into
// the recognition code
pt_t** GetStrokeBufferLocation(FcitxTabletPen* tablet, FcitxModuleFunctionArg args) {
	return &tablet->strokes.buffer;
}

// Drivers, see driver.h
extern FcitxTabletDriver lxbi;


void* FcitxTabletCreate(FcitxInstance* instance) {
	FcitxTabletPen* tablet = fcitx_utils_new(FcitxTabletPen);
	FcitxTabletLoadConfig(&tablet->conf);
	// TODO select driver from config, currently using lxbi

	{ // Initialise the driver
		TabletDriver* d = &tablet->driver;
		d->drv = &lxbi;
		d->userdata = d->drv->Create();
		d->packet = (char*) malloc(d->drv->packet_size);

		d->fd = open(tablet->conf.devicePath, O_RDONLY);
		if(d->fd < 0) {
			FcitxLog(ERROR, "Unable to open device %s", tablet->conf.devicePath);
			return NULL;
		}
	}

	{ // Initialise the X display
		TabletX* x = &tablet->x;
		if(!(x->dpy = XOpenDisplay(NULL)))  {
			FcitxLog(ERROR, "Unable to open X display");
			return NULL;
		}
		x->gcv.function = GXcopy;
		x->gcv.subwindow_mode = IncludeInferiors;
		x->gcv.line_width = 8;
		x->gcv.cap_style = CapRound;
		x->gcv.join_style = JoinRound;
		x->gc = XCreateGC(x->dpy, DefaultRootWindow(x->dpy), GCFunction | GCSubwindowMode | GCLineWidth | GCCapStyle | GCJoinStyle, &x->gcv);

	}

	{ // Initialise the stroke buffer
		TabletStrokes* s = &tablet->strokes;
		s->n = 2048;
		s->buffer = (pt_t*) malloc(sizeof(pt_t) * s->n);
		s->ptr = s->buffer;
	}

	tablet->fcitx = instance;

	// Expose the GetStrokeBufferLocation function so the IME can call it
	FcitxAddon* tablet_addon = FcitxAddonsGetAddonByName(FcitxInstanceGetAddons(instance), FCITX_TABLET_NAME);
	AddFunction(tablet_addon, GetStrokeBufferLocation);

	return tablet;
}

void FcitxTabletSetFd(void* arg) {
	FcitxTabletPen* tablet = (FcitxTabletPen*) arg;
	FD_SET(tablet->driver.fd, FcitxInstanceGetReadFDSet(tablet->fcitx));
	if(FcitxInstanceGetMaxFD(tablet->fcitx) < tablet->driver.fd)
		FcitxInstanceSetMaxFD(tablet->fcitx, tablet->driver.fd);
}

void PushCoordinate(TabletStrokes* s, pt_t newpt) {
	*s->ptr++ = newpt;
	if(s->ptr > &s->buffer[s->n]) { // if we overflow the buffer, increase it
		int newsize = s->n + 1024;
		s->buffer = (pt_t*) realloc(s->buffer, newsize);
		s->ptr = &s->buffer[s->n+1];
		s->n = newsize;
	}
}

void FcitxTabletProcess(void* arg) {
	FcitxTabletPen* tablet = (FcitxTabletPen*) arg;
	TabletDriver* d = &tablet->driver;
	if(FD_ISSET(d->fd, FcitxInstanceGetReadFDSet(tablet->fcitx))) {

		{ // first read a packet from the raw device
			int n = 0;
			const int pktsize = d->drv->packet_size;
			do {
				n += read(d->fd, &d->packet[n], pktsize - n);
			} while(n < pktsize);
		}

		{ // then send it to the driver to convert into events
			FcitxTabletDriverEvent e;
			pt_t pt;
			while((e = d->drv->GetEvent(d->userdata, d->packet, &pt)) != EV_NONE) {
				switch(e) {
				case EV_PENDOWN:
					break; // nothing
				case EV_PENUP:
					{ pt_t p = PT_INVALID; PushCoordinate(&tablet->strokes, p); }
					FcitxInstanceProcessKey(tablet->fcitx, FCITX_PRESS_KEY, 0, FcitxKey_VoidSymbol, IME_RECOGNISE);
					break;
				case EV_POINT:
					PushCoordinate(&tablet->strokes, pt);
					break;
				default:
					FcitxLog(ERROR, "Driver returned unknown event: %d", e);
					break;
				}
			}
		}

		{ // draw the stroke on the screen
			TabletX* x = &tablet->x;
			TabletStrokes* s = &tablet->strokes;
			// We can only draw lines if we have at least 2 points
			if(s->ptr >= &s->buffer[2]) {
				// draw the background stroke
				x->gcv.line_width = 15;
				XChangeGC(x->dpy, x->gc, GCLineWidth, &x->gcv);
				XSetForeground(x->dpy, x->gc, WhitePixel(x->dpy, DefaultScreen(x->dpy)));
				for(pt_t*p = s->buffer; &p[1] != s->ptr; ++p) {
					XDrawLine(x->dpy, DefaultRootWindow(x->dpy), x->gc, p[0].x, p[0].y, p[1].x, p[1].y);
				}
				// draw the foreground stroke
				x->gcv.line_width = 8;
				XChangeGC(x->dpy, x->gc, GCLineWidth, &x->gcv);
				XSetForeground(x->dpy, x->gc, BlackPixel(x->dpy, DefaultScreen(x->dpy)));
				for(pt_t*p = s->buffer; &p[1] != s->ptr; ++p) {
					XDrawLine(x->dpy, DefaultRootWindow(x->dpy), x->gc, p[0].x, p[0].y, p[1].x, p[1].y);
				}
			}
			XSync(x->dpy, 0);
		}

		FD_CLR(d->fd, FcitxInstanceGetReadFDSet(tablet->fcitx));
	}
}

void FcitxTabletDestroy(void* arg) {
	FcitxTabletPen* tablet = (FcitxTabletPen*) arg;
	XFreeGC(tablet->x.dpy, tablet->x.gc);
	XCloseDisplay(tablet->x.dpy);
	tablet->driver.drv->Destroy(tablet->driver.userdata);
	free(tablet->driver.packet);
	free(tablet->strokes.buffer);
}

// Instantiate the event module
FCITX_EXPORT_API
FcitxModule module = {
	FcitxTabletCreate,
	FcitxTabletSetFd,
	FcitxTabletProcess,
	FcitxTabletDestroy,
	NULL
};
