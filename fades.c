#include "compton.h"

fade *fades;

int
get_time_in_milliseconds() {
  struct timeval tv;

  gettimeofday(&tv, NULL);

  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

fade *
find_fade(win *w) {
  fade *f;

  for (f = fades; f; f = f->next) {
    if (f->w == w) return f;
  }

  return 0;
}

void
dequeue_fade(Display *dpy, fade *f) {
  fade **prev;

  for (prev = &fades; *prev; prev = &(*prev)->next) {
    if (*prev == f) {
      *prev = f->next;
      if (f->callback) {
        (*f->callback)(dpy, f->w);
      }
      free(f);
      break;
    }
  }
}

void
cleanup_fade(Display *dpy, win *w) {
  fade *f = find_fade (w);
  if (f) {
    dequeue_fade(dpy, f);
  }
}

void
enqueue_fade(Display *dpy, fade *f) {
  if (!fades) {
    fade_time = get_time_in_milliseconds() + fade_delta;
  }
  f->next = fades;
  fades = f;
}

void
set_fade(Display *dpy, win *w, double start,
         double finish, double step,
         void(*callback) (Display *dpy, win *w),
         Bool exec_callback, Bool override) {
  fade *f;

  f = find_fade(w);
  if (!f) {
    f = malloc(sizeof(fade));
    f->next = 0;
    f->w = w;
    f->cur = start;
    enqueue_fade(dpy, f);
  } else if (!override) {
    return;
  } else {
    if (exec_callback && f->callback) {
      (*f->callback)(dpy, f->w);
    }
  }

  if (finish < 0) finish = 0;
  if (finish > 1) finish = 1;
  f->finish = finish;

  if (f->cur < finish) {
    f->step = step;
  } else if (f->cur > finish) {
    f->step = -step;
  }

  f->callback = callback;
  w->opacity = f->cur * OPAQUE;

#if 0
  printf("set_fade start %g step %g\n", f->cur, f->step);
#endif

  determine_mode(dpy, w);

  if (w->shadow) {
    XRenderFreePicture(dpy, w->shadow);
    w->shadow = None;

    if (w->extents != None) {
      XFixesDestroyRegion(dpy, w->extents);
    }

    /* rebuild the shadow */
    w->extents = win_extents(dpy, w);
  }

  /* fading windows need to be drawn, mark
     them as damaged.  when a window maps,
     if it tries to fade in but it already
     at the right opacity (map/unmap/map fast)
     then it will never get drawn without this
     until it repaints */
  w->damaged = 1;
}

int
fade_timeout(void) {
  int now;
  int delta;

  if (!fades) return -1;

  now = get_time_in_milliseconds();
  delta = fade_time - now;

  if (delta < 0) delta = 0;
/* printf("timeout %d\n", delta); */

  return delta;
}

void
run_fades(Display *dpy) {
  int now = get_time_in_milliseconds();
  fade *next = fades;
  int steps;
  Bool need_dequeue;

#if 0
  printf("run fades\n");
#endif

  if (fade_time - now > 0) return;
  steps = 1 + (now - fade_time) / fade_delta;

  while (next) {
    fade *f = next;
    win *w = f->w;
    next = f->next;

    f->cur += f->step * steps;
    if (f->cur >= 1) {
      f->cur = 1;
    } else if (f->cur < 0) {
      f->cur = 0;
    }

#if 0
    printf("opacity now %g\n", f->cur);
#endif

    w->opacity = f->cur * OPAQUE;
    need_dequeue = False;
    if (f->step > 0) {
      if (f->cur >= f->finish) {
        w->opacity = f->finish * OPAQUE;
        need_dequeue = True;
      }
    } else {
      if (f->cur <= f->finish) {
        w->opacity = f->finish * OPAQUE;
        need_dequeue = True;
      }
    }

    determine_mode(dpy, w);

    if (w->shadow) {
      XRenderFreePicture(dpy, w->shadow);
      w->shadow = None;

      if (w->extents != None) {
        XFixesDestroyRegion(dpy, w->extents);
      }

      /* rebuild the shadow */
      w->extents = win_extents(dpy, w);
    }

    /* Must do this last as it might
       destroy f->w in callbacks */
    if (need_dequeue) dequeue_fade(dpy, f);
  }

  fade_time = now + fade_delta;
}
