/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE for more information.
 *
 */

#include "compton.h"

// === Global constants ===

/// Name strings for window types.
const char * const WINTYPES[NUM_WINTYPES] = {
  "unknown",
  "desktop",
  "dock",
  "toolbar",
  "menu",
  "utility",
  "splash",
  "dialog",
  "normal",
  "dropdown_menu",
  "popup_menu",
  "tooltip",
  "notify",
  "combo",
  "dnd",
};

/// Names of VSync modes
const char * const VSYNC_STRS[NUM_VSYNC] = {
  "none",   // VSYNC_NONE
  "drm",    // VSYNC_DRM
  "opengl", // VSYNC_OPENGL
};

/// Names of root window properties that could point to a pixmap of
/// background.
const static char *background_props_str[] = {
  "_XROOTPMAP_ID",
  "_XSETROOT_ID",
  0,
};

// === Global variables ===

/// Pointer to current session, as a global variable. Only used by
/// <code>error()</code> and <code>reset_enable()</code>, which could not
/// have a pointer to current session passed in.
session_t *ps_g = NULL;

// === Fading ===

/**
 * Get the time left before next fading point.
 *
 * In milliseconds.
 */
static int
fade_timeout(session_t *ps) {
  int diff = ps->o.fade_delta - get_time_ms() + ps->fade_time;

  diff = normalize_i_range(diff, 0, ps->o.fade_delta * 2);

  return diff;
}

/**
 * Run fading on a window.
 *
 * @param steps steps of fading
 */
static void
run_fade(session_t *ps, win *w, unsigned steps) {
  // If we have reached target opacity, return
  if (w->opacity == w->opacity_tgt) {
    return;
  }

  if (!w->fade)
    w->opacity = w->opacity_tgt;
  else if (steps) {
    // Use double below because opacity_t will probably overflow during
    // calculations
    if (w->opacity < w->opacity_tgt)
      w->opacity = normalize_d_range(
          (double) w->opacity + (double) ps->o.fade_in_step * steps,
          0.0, w->opacity_tgt);
    else
      w->opacity = normalize_d_range(
          (double) w->opacity - (double) ps->o.fade_out_step * steps,
          w->opacity_tgt, OPAQUE);
  }

  if (w->opacity != w->opacity_tgt) {
    ps->idling = false;
  }
}

/**
 * Set fade callback of a window, and possibly execute the previous
 * callback.
 *
 * @param exec_callback whether the previous callback is to be executed
 */
static void
set_fade_callback(session_t *ps, win *w,
    void (*callback) (session_t *ps, win *w), bool exec_callback) {
  void (*old_callback) (session_t *ps, win *w) = w->fade_callback;

  w->fade_callback = callback;
  // Must be the last line as the callback could destroy w!
  if (exec_callback && old_callback) {
    old_callback(ps, w);
    // Although currently no callback function affects window state on
    // next paint, it could, in the future
    ps->idling = false;
  }
}

// === Shadows ===

static double __attribute__((const))
gaussian(double r, double x, double y) {
  return ((1 / (sqrt(2 * M_PI * r))) *
    exp((- (x * x + y * y)) / (2 * r * r)));
}

static conv *
make_gaussian_map(double r) {
  conv *c;
  int size = ((int) ceil((r * 3)) + 1) & ~1;
  int center = size / 2;
  int x, y;
  double t;
  double g;

  c = malloc(sizeof(conv) + size * size * sizeof(double));
  c->size = size;
  c->data = (double *) (c + 1);
  t = 0.0;

  for (y = 0; y < size; y++) {
    for (x = 0; x < size; x++) {
      g = gaussian(r, x - center, y - center);
      t += g;
      c->data[y * size + x] = g;
    }
  }

  for (y = 0; y < size; y++) {
    for (x = 0; x < size; x++) {
      c->data[y * size + x] /= t;
    }
  }

  return c;
}

/*
 * A picture will help
 *
 *      -center   0                width  width+center
 *  -center +-----+-------------------+-----+
 *          |     |                   |     |
 *          |     |                   |     |
 *        0 +-----+-------------------+-----+
 *          |     |                   |     |
 *          |     |                   |     |
 *          |     |                   |     |
 *   height +-----+-------------------+-----+
 *          |     |                   |     |
 * height+  |     |                   |     |
 *  center  +-----+-------------------+-----+
 */

static unsigned char
sum_gaussian(conv *map, double opacity,
             int x, int y, int width, int height) {
  int fx, fy;
  double *g_data;
  double *g_line = map->data;
  int g_size = map->size;
  int center = g_size / 2;
  int fx_start, fx_end;
  int fy_start, fy_end;
  double v;

  /*
   * Compute set of filter values which are "in range",
   * that's the set with:
   *    0 <= x + (fx-center) && x + (fx-center) < width &&
   *  0 <= y + (fy-center) && y + (fy-center) < height
   *
   *  0 <= x + (fx - center)    x + fx - center < width
   *  center - x <= fx    fx < width + center - x
   */

  fx_start = center - x;
  if (fx_start < 0) fx_start = 0;
  fx_end = width + center - x;
  if (fx_end > g_size) fx_end = g_size;

  fy_start = center - y;
  if (fy_start < 0) fy_start = 0;
  fy_end = height + center - y;
  if (fy_end > g_size) fy_end = g_size;

  g_line = g_line + fy_start * g_size + fx_start;

  v = 0;

  for (fy = fy_start; fy < fy_end; fy++) {
    g_data = g_line;
    g_line += g_size;

    for (fx = fx_start; fx < fx_end; fx++) {
      v += *g_data++;
    }
  }

  if (v > 1) v = 1;

  return ((unsigned char) (v * opacity * 255.0));
}

/* precompute shadow corners and sides
   to save time for large windows */

static void
presum_gaussian(session_t *ps, conv *map) {
  int center = map->size / 2;
  int opacity, x, y;

  ps->cgsize = map->size;

  if (ps->shadow_corner)
    free(ps->shadow_corner);
  if (ps->shadow_top)
    free(ps->shadow_top);

  ps->shadow_corner = malloc((ps->cgsize + 1) * (ps->cgsize + 1) * 26);
  ps->shadow_top = malloc((ps->cgsize + 1) * 26);

  for (x = 0; x <= ps->cgsize; x++) {
    ps->shadow_top[25 * (ps->cgsize + 1) + x] =
      sum_gaussian(map, 1, x - center, center, ps->cgsize * 2, ps->cgsize * 2);

    for (opacity = 0; opacity < 25; opacity++) {
      ps->shadow_top[opacity * (ps->cgsize + 1) + x] =
        ps->shadow_top[25 * (ps->cgsize + 1) + x] * opacity / 25;
    }

    for (y = 0; y <= x; y++) {
      ps->shadow_corner[25 * (ps->cgsize + 1) * (ps->cgsize + 1) + y * (ps->cgsize + 1) + x]
        = sum_gaussian(map, 1, x - center, y - center, ps->cgsize * 2, ps->cgsize * 2);
      ps->shadow_corner[25 * (ps->cgsize + 1) * (ps->cgsize + 1) + x * (ps->cgsize + 1) + y]
        = ps->shadow_corner[25 * (ps->cgsize + 1) * (ps->cgsize + 1) + y * (ps->cgsize + 1) + x];

      for (opacity = 0; opacity < 25; opacity++) {
        ps->shadow_corner[opacity * (ps->cgsize + 1) * (ps->cgsize + 1)
                      + y * (ps->cgsize + 1) + x]
          = ps->shadow_corner[opacity * (ps->cgsize + 1) * (ps->cgsize + 1)
                          + x * (ps->cgsize + 1) + y]
          = ps->shadow_corner[25 * (ps->cgsize + 1) * (ps->cgsize + 1)
                          + y * (ps->cgsize + 1) + x] * opacity / 25;
      }
    }
  }
}

static XImage *
make_shadow(session_t *ps, double opacity,
            int width, int height) {
  XImage *ximage;
  unsigned char *data;
  int ylimit, xlimit;
  int swidth = width + ps->cgsize;
  int sheight = height + ps->cgsize;
  int center = ps->cgsize / 2;
  int x, y;
  unsigned char d;
  int x_diff;
  int opacity_int = (int)(opacity * 25);

  data = malloc(swidth * sheight * sizeof(unsigned char));
  if (!data) return 0;

  ximage = XCreateImage(ps->dpy, ps->vis, 8,
    ZPixmap, 0, (char *) data, swidth, sheight, 8, swidth * sizeof(char));

  if (!ximage) {
    free(data);
    return 0;
  }

  /*
   * Build the gaussian in sections
   */

  /*
   * center (fill the complete data array)
   */

  // If clear_shadow is enabled and the border & corner shadow (which
  // later will be filled) could entirely cover the area of the shadow
  // that will be displayed, do not bother filling other pixels. If it
  // can't, we must fill the other pixels here.
  /* if (!(clear_shadow && ps->o.shadow_offset_x <= 0 && ps->o.shadow_offset_x >= -ps->cgsize
        && ps->o.shadow_offset_y <= 0 && ps->o.shadow_offset_y >= -ps->cgsize)) { */
    if (ps->cgsize > 0) {
      d = ps->shadow_top[opacity_int * (ps->cgsize + 1) + ps->cgsize];
    } else {
      d = sum_gaussian(ps->gaussian_map,
        opacity, center, center, width, height);
    }
    memset(data, d, sheight * swidth);
  // }

  /*
   * corners
   */

  ylimit = ps->cgsize;
  if (ylimit > sheight / 2) ylimit = (sheight + 1) / 2;

  xlimit = ps->cgsize;
  if (xlimit > swidth / 2) xlimit = (swidth + 1) / 2;

  for (y = 0; y < ylimit; y++) {
    for (x = 0; x < xlimit; x++) {
      if (xlimit == ps->cgsize && ylimit == ps->cgsize) {
        d = ps->shadow_corner[opacity_int * (ps->cgsize + 1) * (ps->cgsize + 1)
                          + y * (ps->cgsize + 1) + x];
      } else {
        d = sum_gaussian(ps->gaussian_map,
          opacity, x - center, y - center, width, height);
      }
      data[y * swidth + x] = d;
      data[(sheight - y - 1) * swidth + x] = d;
      data[(sheight - y - 1) * swidth + (swidth - x - 1)] = d;
      data[y * swidth + (swidth - x - 1)] = d;
    }
  }

  /*
   * top/bottom
   */

  x_diff = swidth - (ps->cgsize * 2);
  if (x_diff > 0 && ylimit > 0) {
    for (y = 0; y < ylimit; y++) {
      if (ylimit == ps->cgsize) {
        d = ps->shadow_top[opacity_int * (ps->cgsize + 1) + y];
      } else {
        d = sum_gaussian(ps->gaussian_map,
          opacity, center, y - center, width, height);
      }
      memset(&data[y * swidth + ps->cgsize], d, x_diff);
      memset(&data[(sheight - y - 1) * swidth + ps->cgsize], d, x_diff);
    }
  }

  /*
   * sides
   */

  for (x = 0; x < xlimit; x++) {
    if (xlimit == ps->cgsize) {
      d = ps->shadow_top[opacity_int * (ps->cgsize + 1) + x];
    } else {
      d = sum_gaussian(ps->gaussian_map,
        opacity, x - center, center, width, height);
    }
    for (y = ps->cgsize; y < sheight - ps->cgsize; y++) {
      data[y * swidth + x] = d;
      data[y * swidth + (swidth - x - 1)] = d;
    }
  }

  /*
  if (clear_shadow) {
    // Clear the region in the shadow that the window would cover based
    // on shadow_offset_{x,y} user provides
    int xstart = normalize_i_range(- (int) ps->o.shadow_offset_x, 0, swidth);
    int xrange = normalize_i_range(width - (int) ps->o.shadow_offset_x,
        0, swidth) - xstart;
    int ystart = normalize_i_range(- (int) ps->o.shadow_offset_y, 0, sheight);
    int yend = normalize_i_range(height - (int) ps->o.shadow_offset_y,
        0, sheight);
    int y;

    for (y = ystart; y < yend; y++) {
      memset(&data[y * swidth + xstart], 0, xrange);
    }
  }
  */

  return ximage;
}

/**
 * Generate shadow <code>Picture</code> for a window.
 */
static Picture
shadow_picture(session_t *ps, double opacity, int width, int height) {
  XImage *shadow_image = NULL;
  Pixmap shadow_pixmap = None, shadow_pixmap_argb = None;
  Picture shadow_picture = None, shadow_picture_argb = None;
  GC gc = None;

  shadow_image = make_shadow(ps, opacity, width, height);
  if (!shadow_image)
    return None;

  shadow_pixmap = XCreatePixmap(ps->dpy, ps->root,
    shadow_image->width, shadow_image->height, 8);
  shadow_pixmap_argb = XCreatePixmap(ps->dpy, ps->root,
    shadow_image->width, shadow_image->height, 32);

  if (!shadow_pixmap || !shadow_pixmap_argb)
    goto shadow_picture_err;

  shadow_picture = XRenderCreatePicture(ps->dpy, shadow_pixmap,
    XRenderFindStandardFormat(ps->dpy, PictStandardA8), 0, 0);
  shadow_picture_argb = XRenderCreatePicture(ps->dpy, shadow_pixmap_argb,
    XRenderFindStandardFormat(ps->dpy, PictStandardARGB32), 0, 0);
  if (!shadow_picture || !shadow_picture_argb)
    goto shadow_picture_err;

  gc = XCreateGC(ps->dpy, shadow_pixmap, 0, 0);
  if (!gc)
    goto shadow_picture_err;

  XPutImage(ps->dpy, shadow_pixmap, gc, shadow_image, 0, 0, 0, 0,
    shadow_image->width, shadow_image->height);
  XRenderComposite(ps->dpy, PictOpSrc, ps->cshadow_picture, shadow_picture,
      shadow_picture_argb, 0, 0, 0, 0, 0, 0,
      shadow_image->width, shadow_image->height);

  XFreeGC(ps->dpy, gc);
  XDestroyImage(shadow_image);
  XFreePixmap(ps->dpy, shadow_pixmap);
  XFreePixmap(ps->dpy, shadow_pixmap_argb);
  XRenderFreePicture(ps->dpy, shadow_picture);

  return shadow_picture_argb;

shadow_picture_err:
  if (shadow_image)
    XDestroyImage(shadow_image);
  if (shadow_pixmap)
    XFreePixmap(ps->dpy, shadow_pixmap);
  if (shadow_pixmap_argb)
    XFreePixmap(ps->dpy, shadow_pixmap_argb);
  if (shadow_picture)
    XRenderFreePicture(ps->dpy, shadow_picture);
  if (shadow_picture_argb)
    XRenderFreePicture(ps->dpy, shadow_picture_argb);
  if (gc)
    XFreeGC(ps->dpy, gc);
  return None;
}

/**
 * Generate a 1x1 <code>Picture</code> of a particular color.
 */
static Picture
solid_picture(session_t *ps, bool argb, double a,
              double r, double g, double b) {
  Pixmap pixmap;
  Picture picture;
  XRenderPictureAttributes pa;
  XRenderColor c;

  pixmap = XCreatePixmap(ps->dpy, ps->root, 1, 1, argb ? 32 : 8);

  if (!pixmap) return None;

  pa.repeat = True;
  picture = XRenderCreatePicture(ps->dpy, pixmap,
    XRenderFindStandardFormat(ps->dpy, argb
      ? PictStandardARGB32 : PictStandardA8),
    CPRepeat,
    &pa);

  if (!picture) {
    XFreePixmap(ps->dpy, pixmap);
    return None;
  }

  c.alpha = a * 0xffff;
  c.red =   r * 0xffff;
  c.green = g * 0xffff;
  c.blue =  b * 0xffff;

  XRenderFillRectangle(ps->dpy, PictOpSrc, picture, &c, 0, 0, 1, 1);
  XFreePixmap(ps->dpy, pixmap);

  return picture;
}

// === Error handling ===

static void
discard_ignore(session_t *ps, unsigned long sequence) {
  while (ps->ignore_head) {
    if ((long) (sequence - ps->ignore_head->sequence) > 0) {
      ignore_t *next = ps->ignore_head->next;
      free(ps->ignore_head);
      ps->ignore_head = next;
      if (!ps->ignore_head) {
        ps->ignore_tail = &ps->ignore_head;
      }
    } else {
      break;
    }
  }
}

static void
set_ignore(session_t *ps, unsigned long sequence) {
  ignore_t *i = malloc(sizeof(ignore_t));
  if (!i) return;

  i->sequence = sequence;
  i->next = 0;
  *ps->ignore_tail = i;
  ps->ignore_tail = &i->next;
}

static int
should_ignore(session_t *ps, unsigned long sequence) {
  discard_ignore(ps, sequence);
  return ps->ignore_head && ps->ignore_head->sequence == sequence;
}

// === Windows ===

/**
 * Get a specific attribute of a window.
 *
 * Returns a blank structure if the returned type and format does not
 * match the requested type and format.
 *
 * @param ps current session
 * @param w window
 * @param atom atom of attribute to fetch
 * @param length length to read
 * @param rtype atom of the requested type
 * @param rformat requested format
 * @return a <code>winprop_t</code> structure containing the attribute
 *    and number of items. A blank one on failure.
 */
winprop_t
wid_get_prop_adv(const session_t *ps, Window w, Atom atom, long offset,
    long length, Atom rtype, int rformat) {
  Atom type = None;
  int format = 0;
  unsigned long nitems = 0, after = 0;
  unsigned char *data = NULL;

  if (Success == XGetWindowProperty(ps->dpy, w, atom, offset, length,
        False, rtype, &type, &format, &nitems, &after, &data)
      && nitems && (AnyPropertyType == type || type == rtype)
      && (!format || format == rformat)
      && (8 == format || 16 == format || 32 == format)) {
      return (winprop_t) {
        .data.p8 = data,
        .nitems = nitems,
        .type = type,
        .format = format,
      };
  }

  XFree(data);

  return (winprop_t) {
    .data.p8 = NULL,
    .nitems = 0,
    .type = AnyPropertyType,
    .format = 0
  };
}

/**
 * Check if a window has rounded corners.
 */
static void
win_rounded_corners(session_t *ps, win *w) {
  if (!w->bounding_shaped)
    return;

  // Fetch its bounding region
  if (!w->border_size)
    w->border_size = border_size(ps, w, true);

  // Quit if border_size() returns None
  if (!w->border_size)
    return;

  // Determine the minimum width/height of a rectangle that could mark
  // a window as having rounded corners
  unsigned short minwidth = max_i(w->widthb * (1 - ROUNDED_PERCENT),
      w->widthb - ROUNDED_PIXELS);
  unsigned short minheight = max_i(w->heightb * (1 - ROUNDED_PERCENT),
      w->heightb - ROUNDED_PIXELS);

  // Get the rectangles in the bounding region
  int nrects = 0, i;
  XRectangle *rects = XFixesFetchRegion(ps->dpy, w->border_size, &nrects);
  if (!rects)
    return;

  // Look for a rectangle large enough for this window be considered
  // having rounded corners
  for (i = 0; i < nrects; ++i)
    if (rects[i].width >= minwidth && rects[i].height >= minheight) {
      w->rounded_corners = true;
      XFree(rects);
      return;
    }

  w->rounded_corners = false;
  XFree(rects);
}

/**
 * Match a window against a single window condition.
 *
 * @return true if matched, false otherwise.
 */
static bool
win_match_once(win *w, const wincond_t *cond) {
  const char *target;
  bool matched = false;

#ifdef DEBUG_WINMATCH
  printf("win_match_once(%#010lx \"%s\"): cond = %p", w->id, w->name,
      cond);
#endif

  if (InputOnly == w->a.class) {
#ifdef DEBUG_WINMATCH
  printf(": InputOnly\n");
#endif
    return false;
  }

  // Determine the target
  target = NULL;
  switch (cond->target) {
    case CONDTGT_NAME:
      target = w->name;
      break;
    case CONDTGT_CLASSI:
      target = w->class_instance;
      break;
    case CONDTGT_CLASSG:
      target = w->class_general;
      break;
    case CONDTGT_ROLE:
      target = w->role;
      break;
  }

  if (!target) {
#ifdef DEBUG_WINMATCH
  printf(": Target not found\n");
#endif
    return false;
  }

  // Determine pattern type and match
  switch (cond->type) {
    case CONDTP_EXACT:
      if (cond->flags & CONDF_IGNORECASE)
        matched = !strcasecmp(target, cond->pattern);
      else
        matched = !strcmp(target, cond->pattern);
      break;
    case CONDTP_ANYWHERE:
      if (cond->flags & CONDF_IGNORECASE)
        matched = strcasestr(target, cond->pattern);
      else
        matched = strstr(target, cond->pattern);
      break;
    case CONDTP_FROMSTART:
      if (cond->flags & CONDF_IGNORECASE)
        matched = !strncasecmp(target, cond->pattern,
            strlen(cond->pattern));
      else
        matched = !strncmp(target, cond->pattern,
            strlen(cond->pattern));
      break;
    case CONDTP_WILDCARD:
      {
        int flags = 0;
        if (cond->flags & CONDF_IGNORECASE)
          flags = FNM_CASEFOLD;
        matched = !fnmatch(cond->pattern, target, flags);
      }
      break;
    case CONDTP_REGEX_PCRE:
#ifdef CONFIG_REGEX_PCRE
      matched = (pcre_exec(cond->regex_pcre, cond->regex_pcre_extra,
            target, strlen(target), 0, 0, NULL, 0) >= 0);
#endif
      break;
  }

#ifdef DEBUG_WINMATCH
  printf(", matched = %d\n", matched);
#endif

  return matched;
}

/**
 * Match a window against a condition linked list.
 *
 * @param cache a place to cache the last matched condition
 * @return true if matched, false otherwise.
 */
static bool
win_match(win *w, wincond_t *condlst, wincond_t **cache) {
  // Check if the cached entry matches firstly
  if (cache && *cache && win_match_once(w, *cache))
    return true;

  // Then go through the whole linked list
  for (; condlst; condlst = condlst->next) {
    if (win_match_once(w, condlst)) {
      if (cache)
        *cache = condlst;
      return true;
    }
  }

  return false;
}

/**
 * Add a pattern to a condition linked list.
 */
static bool
condlst_add(wincond_t **pcondlst, const char *pattern) {
  if (!pattern)
    return false;

  unsigned plen = strlen(pattern);
  wincond_t *cond;
  const char *pos;

  if (plen < 4 || ':' != pattern[1] || !strchr(pattern + 2, ':')) {
    printf("Pattern \"%s\": Format invalid.\n", pattern);
    return false;
  }

  // Allocate memory for the new condition
  cond = malloc(sizeof(wincond_t));

  // Determine the pattern target
  switch (pattern[0]) {
    case 'n':
      cond->target = CONDTGT_NAME;
      break;
    case 'i':
      cond->target = CONDTGT_CLASSI;
      break;
    case 'g':
      cond->target = CONDTGT_CLASSG;
      break;
    case 'r':
      cond->target = CONDTGT_ROLE;
      break;
    default:
      printf("Pattern \"%s\": Target \"%c\" invalid.\n",
          pattern, pattern[0]);
      free(cond);
      return false;
  }

  // Determine the pattern type
  switch (pattern[2]) {
    case 'e':
      cond->type = CONDTP_EXACT;
      break;
    case 'a':
      cond->type = CONDTP_ANYWHERE;
      break;
    case 's':
      cond->type = CONDTP_FROMSTART;
      break;
    case 'w':
      cond->type = CONDTP_WILDCARD;
      break;
#ifdef CONFIG_REGEX_PCRE
    case 'p':
      cond->type = CONDTP_REGEX_PCRE;
      break;
#endif
    default:
      printf("Pattern \"%s\": Type \"%c\" invalid.\n",
          pattern, pattern[2]);
      free(cond);
      return false;
  }

  // Determine the pattern flags
  pos = &pattern[3];
  cond->flags = 0;
  while (':' != *pos) {
    switch (*pos) {
      case 'i':
        cond->flags |= CONDF_IGNORECASE;
        break;
      default:
        printf("Pattern \"%s\": Flag \"%c\" invalid.\n",
            pattern, *pos);
        break;
    }
    ++pos;
  }

  // Copy the pattern
  ++pos;
  cond->pattern = NULL;
#ifdef CONFIG_REGEX_PCRE
  cond->regex_pcre = NULL;
  cond->regex_pcre_extra = NULL;
#endif
  if (CONDTP_REGEX_PCRE == cond->type) {
#ifdef CONFIG_REGEX_PCRE
    const char *error = NULL;
    int erroffset = 0;
    int options = 0;

    if (cond->flags & CONDF_IGNORECASE)
      options |= PCRE_CASELESS;

    cond->regex_pcre = pcre_compile(pos, options, &error, &erroffset,
        NULL);
    if (!cond->regex_pcre) {
      printf("Pattern \"%s\": PCRE regular expression parsing failed on "
          "offset %d: %s\n", pattern, erroffset, error);
      free(cond);
      return false;
    }
#ifdef CONFIG_REGEX_PCRE_JIT
    cond->regex_pcre_extra = pcre_study(cond->regex_pcre, PCRE_STUDY_JIT_COMPILE, &error);
    if (!cond->regex_pcre_extra) {
      printf("Pattern \"%s\": PCRE regular expression study failed: %s",
          pattern, error);
    }
#endif
#endif
  }
  else {
    cond->pattern = mstrcpy(pos);
  }

  // Insert it into the linked list
  cond->next = *pcondlst;
  *pcondlst = cond;

  return true;
}

/**
 * Determine the event mask for a window.
 */
static long
determine_evmask(session_t *ps, Window wid, win_evmode_t mode) {
  long evmask = NoEventMask;
  win *w = NULL;

  // Check if it's a mapped frame window
  if (WIN_EVMODE_FRAME == mode
      || ((w = find_win(ps, wid)) && IsViewable == w->a.map_state)) {
    evmask |= PropertyChangeMask;
    if (ps->o.track_focus && !ps->o.use_ewmh_active_win)
      evmask |= FocusChangeMask;
  }

  // Check if it's a mapped client window
  if (WIN_EVMODE_CLIENT == mode
      || ((w = find_toplevel(ps, wid)) && IsViewable == w->a.map_state)) {
    if (ps->o.frame_opacity || ps->o.track_wdata || ps->track_atom_lst
        || ps->o.detect_client_opacity)
      evmask |= PropertyChangeMask;
  }

  return evmask;
}

/**
 * Find out the WM frame of a client window by querying X.
 *
 * @param ps current session
 * @param w window ID
 * @return struct _win object of the found window, NULL if not found
 */
static win *
find_toplevel2(session_t *ps, Window wid) {
  win *w = NULL;

  // We traverse through its ancestors to find out the frame
  while (wid && wid != ps->root && !(w = find_win(ps, wid))) {
    Window troot;
    Window parent;
    Window *tchildren;
    unsigned tnchildren;

    // XQueryTree probably fails if you run compton when X is somehow
    // initializing (like add it in .xinitrc). In this case
    // just leave it alone.
    if (!XQueryTree(ps->dpy, wid, &troot, &parent, &tchildren,
          &tnchildren)) {
      parent = 0;
      break;
    }

    if (tchildren) XFree(tchildren);

    wid = parent;
  }

  return w;
}

/**
 * Recheck currently focused window and set its <code>w->focused</code>
 * to true.
 *
 * @param ps current session
 * @return struct _win of currently focused window, NULL if not found
 */
static win *
recheck_focus(session_t *ps) {
  // Use EWMH _NET_ACTIVE_WINDOW if enabled
  if (ps->o.use_ewmh_active_win) {
    update_ewmh_active_win(ps);
    return ps->active_win;
  }

  // Determine the currently focused window so we can apply appropriate
  // opacity on it
  Window wid = 0;
  int revert_to;
  win *w = NULL;

  XGetInputFocus(ps->dpy, &wid, &revert_to);

  if (!wid || PointerRoot == wid)
    return NULL;

  // Fallback to the old method if find_toplevel() fails
  if (!(w = find_toplevel(ps, wid))) {
    w = find_toplevel2(ps, wid);
  }

  // And we set the focus state and opacity here
  if (w) {
    win_set_focused(ps, w, true);
    return w;
  }

  return NULL;
}

static Picture
root_tile_f(session_t *ps) {
  /*
  if (ps->o.paint_on_overlay) {
    return ps->root_picture;
  } */

  Picture picture;
  Pixmap pixmap;
  bool fill = false;
  XRenderPictureAttributes pa;
  int p;

  pixmap = None;

  // Get the values of background attributes
  for (p = 0; background_props_str[p]; p++) {
    winprop_t prop = wid_get_prop(ps, ps->root,
        get_atom(ps, background_props_str[p]),
        1L, XA_PIXMAP, 32);
    if (prop.nitems) {
      pixmap = *prop.data.p32;
      fill = false;
      free_winprop(&prop);
      break;
    }
    free_winprop(&prop);
  }

  if (!pixmap) {
    pixmap = XCreatePixmap(ps->dpy, ps->root, 1, 1, ps->depth);
    fill = true;
  }

  pa.repeat = True;
  picture = XRenderCreatePicture(
    ps->dpy, pixmap, XRenderFindVisualFormat(ps->dpy, ps->vis),
    CPRepeat, &pa);

  if (fill) {
    XRenderColor  c;

    c.red = c.green = c.blue = 0x8080;
    c.alpha = 0xffff;
    XRenderFillRectangle(
      ps->dpy, PictOpSrc, picture, &c, 0, 0, 1, 1);

    free_pixmap(ps, &pixmap);
  }

  return picture;
}

/**
 * Paint root window content.
 */
static void
paint_root(session_t *ps, Picture tgt_buffer) {
  if (!ps->root_tile) {
    ps->root_tile = root_tile_f(ps);
  }

  XRenderComposite(
    ps->dpy, PictOpSrc, ps->root_tile, None,
    tgt_buffer, 0, 0, 0, 0, 0, 0,
    ps->root_width, ps->root_height);
}

/**
 * Get a rectangular region a window occupies, excluding shadow.
 */
static XserverRegion
win_get_region(session_t *ps, win *w, bool use_offset) {
  XRectangle r;

  r.x = (use_offset ? w->a.x: 0);
  r.y = (use_offset ? w->a.y: 0);
  r.width = w->widthb;
  r.height = w->heightb;

  return XFixesCreateRegion(ps->dpy, &r, 1);
}

/**
 * Get a rectangular region a window occupies, excluding frame and shadow.
 */
static XserverRegion
win_get_region_noframe(session_t *ps, win *w, bool use_offset) {
  XRectangle r;

  r.x = (use_offset ? w->a.x: 0) + w->a.border_width + w->left_width;
  r.y = (use_offset ? w->a.y: 0) + w->a.border_width + w->top_width;
  r.width = max_i(w->a.width - w->left_width - w->right_width, 0);
  r.height = max_i(w->a.height - w->top_width - w->bottom_width, 0);

  if (r.width > 0 && r.height > 0)
    return XFixesCreateRegion(ps->dpy, &r, 1);
  else
    return XFixesCreateRegion(ps->dpy, NULL, 0);
}

/**
 * Get a rectangular region a window (and possibly its shadow) occupies.
 *
 * Note w->shadow and shadow geometry must be correct before calling this
 * function.
 */
static XserverRegion
win_extents(session_t *ps, win *w) {
  XRectangle r;

  r.x = w->a.x;
  r.y = w->a.y;
  r.width = w->widthb;
  r.height = w->heightb;

  if (w->shadow) {
    XRectangle sr;

    sr.x = w->a.x + w->shadow_dx;
    sr.y = w->a.y + w->shadow_dy;
    sr.width = w->shadow_width;
    sr.height = w->shadow_height;

    if (sr.x < r.x) {
      r.width = (r.x + r.width) - sr.x;
      r.x = sr.x;
    }

    if (sr.y < r.y) {
      r.height = (r.y + r.height) - sr.y;
      r.y = sr.y;
    }

    if (sr.x + sr.width > r.x + r.width) {
      r.width = sr.x + sr.width - r.x;
    }

    if (sr.y + sr.height > r.y + r.height) {
      r.height = sr.y + sr.height - r.y;
    }
  }

  return XFixesCreateRegion(ps->dpy, &r, 1);
}

/**
 * Retrieve the bounding shape of a window.
 */
static XserverRegion
border_size(session_t *ps, win *w, bool use_offset) {
  // Start with the window rectangular region
  XserverRegion fin = win_get_region(ps, w, use_offset);

  // Only request for a bounding region if the window is shaped
  if (w->bounding_shaped) {
    /*
     * if window doesn't exist anymore,  this will generate an error
     * as well as not generate a region.  Perhaps a better XFixes
     * architecture would be to have a request that copies instead
     * of creates, that way you'd just end up with an empty region
     * instead of an invalid XID.
     */

    XserverRegion border = XFixesCreateRegionFromWindow(
      ps->dpy, w->id, WindowRegionBounding);

    if (!border)
      return fin;

    if (use_offset) {
      // Translate the region to the correct place
      XFixesTranslateRegion(ps->dpy, border,
        w->a.x + w->a.border_width,
        w->a.y + w->a.border_width);
    }

    // Intersect the bounding region we got with the window rectangle, to
    // make sure the bounding region is not bigger than the window
    // rectangle
    XFixesIntersectRegion(ps->dpy, fin, fin, border);
    XFixesDestroyRegion(ps->dpy, border);
  }

  return fin;
}

/**
 * Look for the client window of a particular window.
 */
static Window
find_client_win(session_t *ps, Window w) {
  if (wid_has_prop(ps, w, ps->atom_client)) {
    return w;
  }

  Window *children;
  unsigned int nchildren;
  unsigned int i;
  Window ret = 0;

  if (!wid_get_children(ps, w, &children, &nchildren)) {
    return 0;
  }

  for (i = 0; i < nchildren; ++i) {
    if ((ret = find_client_win(ps, children[i])))
      break;
  }

  XFree(children);

  return ret;
}

/**
 * Retrieve frame extents from a window.
 */
static void
get_frame_extents(session_t *ps, win *w, Window client) {
  w->left_width = 0;
  w->right_width = 0;
  w->top_width = 0;
  w->bottom_width = 0;

  winprop_t prop = wid_get_prop(ps, client, ps->atom_frame_extents,
    4L, XA_CARDINAL, 32);

  if (4 == prop.nitems) {
    const long * const extents = prop.data.p32;
    w->left_width = extents[0];
    w->right_width = extents[1];
    w->top_width = extents[2];
    w->bottom_width = extents[3];

    if (ps->o.frame_opacity)
      update_reg_ignore_expire(ps, w);
  }

#ifdef DEBUG_FRAME
  printf_dbgf("(%#010lx): %d, %d, %d, %d\n", w->id,
      w->left_width, w->right_width, w->top_width, w->bottom_width);
#endif

  free_winprop(&prop);
}

/**
 * Get alpha <code>Picture</code> for an opacity in <code>double</code>.
 */
static inline Picture
get_alpha_pict_d(session_t *ps, double o) {
  assert((round(normalize_d(o) / ps->o.alpha_step)) <= round(1.0 / ps->o.alpha_step));
  return ps->alpha_picts[(int) (round(normalize_d(o)
        / ps->o.alpha_step))];
}

/**
 * Get alpha <code>Picture</code> for an opacity in
 * <code>opacity_t</code>.
 */
static inline Picture
get_alpha_pict_o(session_t *ps, opacity_t o) {
  return get_alpha_pict_d(ps, (double) o / OPAQUE);
}

static win *
paint_preprocess(session_t *ps, win *list) {
  // Initialize unredir_possible
  ps->unredir_possible = false;

  win *w;
  win *t = NULL, *next = NULL;
  // Trace whether it's the highest window to paint
  bool is_highest = true;

  // Fading step calculation
  time_ms_t steps = 0L;
  if (ps->fade_time) {
    steps = ((get_time_ms() - ps->fade_time) + FADE_DELTA_TOLERANCE * ps->o.fade_delta) / ps->o.fade_delta;
  }
  // Reset fade_time if unset, or there appears to be a time disorder
  if (!ps->fade_time || steps < 0L) {
    ps->fade_time = get_time_ms();
    steps = 0L;
  }
  ps->fade_time += steps * ps->o.fade_delta;

  XserverRegion last_reg_ignore = None;

  for (w = list; w; w = next) {
    bool to_paint = true;
    const winmode_t mode_old = w->mode;

    // In case calling the fade callback function destroys this window
    next = w->next;
    opacity_t opacity_old = w->opacity;

    // Destroy reg_ignore on all windows if they should expire
    if (ps->reg_ignore_expire)
      free_region(ps, &w->reg_ignore);

    // Update window opacity target and dim state if asked
    if (WFLAG_OPCT_CHANGE & w->flags) {
      calc_opacity(ps, w);
      calc_dim(ps, w);
    }

    // Run fading
    run_fade(ps, w, steps);

    // Give up if it's not damaged or invisible, or it's unmapped and its
    // picture is gone (for example due to a ConfigureNotify)
    if (!w->damaged
        || w->a.x + w->a.width < 1 || w->a.y + w->a.height < 1
        || w->a.x >= ps->root_width || w->a.y >= ps->root_height
        || ((IsUnmapped == w->a.map_state || w->destroyed)
          && !w->picture)) {
      to_paint = false;
    }

    if (to_paint) {
      // If opacity changes
      if (w->opacity != opacity_old) {
        win_determine_mode(ps, w);
        add_damage_win(ps, w);
      }

      w->alpha_pict = get_alpha_pict_o(ps, w->opacity);

      // End the game if we are using the 0 opacity alpha_pict
      if (w->alpha_pict == ps->alpha_picts[0]) {
        to_paint = false;
      }
    }

    if (to_paint) {
      // Fetch bounding region
      if (!w->border_size) {
        w->border_size = border_size(ps, w, true);
      }

      // Fetch window extents
      if (!w->extents) {
        w->extents = win_extents(ps, w);
        // If w->extents does not exist, the previous add_damage_win()
        // call when opacity changes has no effect, so redo it here.
        if (w->opacity != opacity_old)
          add_damage_win(ps, w);
      }

      // Calculate frame_opacity
      {
        double frame_opacity_old = w->frame_opacity;

        if (ps->o.frame_opacity && 1.0 != ps->o.frame_opacity
            && win_has_frame(w))
          w->frame_opacity = get_opacity_percent(w) *
            ps->o.frame_opacity;
        else
          w->frame_opacity = 0.0;

        if (w->to_paint && WMODE_SOLID == mode_old
            && (0.0 == frame_opacity_old) != (0.0 == w->frame_opacity))
          ps->reg_ignore_expire = true;
      }

      w->frame_alpha_pict = get_alpha_pict_d(ps, w->frame_opacity);

      // Calculate shadow opacity
      if (w->frame_opacity)
        w->shadow_opacity = ps->o.shadow_opacity * w->frame_opacity;
      else
        w->shadow_opacity = ps->o.shadow_opacity * get_opacity_percent(w);

      // Rebuild shadow_pict if necessary
      if (w->flags & WFLAG_SIZE_CHANGE)
        free_picture(ps, &w->shadow_pict);

      if (w->shadow && !w->shadow_pict) {
        w->shadow_pict = shadow_picture(ps, 1, w->widthb, w->heightb);
      }

      w->shadow_alpha_pict = get_alpha_pict_d(ps, w->shadow_opacity);

      // Dimming preprocessing
      if (w->dim) {
        double dim_opacity = ps->o.inactive_dim;
        if (!ps->o.inactive_dim_fixed)
          dim_opacity *= get_opacity_percent(w);

        w->dim_alpha_pict = get_alpha_pict_d(ps, dim_opacity);
      }

    }

    if ((to_paint && WMODE_SOLID == w->mode)
        != (w->to_paint && WMODE_SOLID == mode_old))
      ps->reg_ignore_expire = true;

    // Add window to damaged area if its painting status changes
    if (to_paint != w->to_paint)
      add_damage_win(ps, w);

    if (to_paint) {
      // Generate ignore region for painting to reduce GPU load
      if (ps->reg_ignore_expire || !w->to_paint) {
        free_region(ps, &w->reg_ignore);

        // If the window is solid, we add the window region to the
        // ignored region
        if (WMODE_SOLID == w->mode) {
          if (!w->frame_opacity) {
            if (w->border_size)
              w->reg_ignore = copy_region(ps, w->border_size);
            else
              w->reg_ignore = win_get_region(ps, w, true);
          }
          else {
            w->reg_ignore = win_get_region_noframe(ps, w, true);
            if (w->border_size)
              XFixesIntersectRegion(ps->dpy, w->reg_ignore, w->reg_ignore,
                  w->border_size);
          }

          if (last_reg_ignore)
            XFixesUnionRegion(ps->dpy, w->reg_ignore, w->reg_ignore,
                last_reg_ignore);
        }
        // Otherwise we copy the last region over
        else if (last_reg_ignore)
          w->reg_ignore = copy_region(ps, last_reg_ignore);
        else
          w->reg_ignore = None;
      }

      last_reg_ignore = w->reg_ignore;

      if (is_highest && to_paint) {
        is_highest = false;
        // Disable unredirection for multi-screen setups
        if (WMODE_SOLID == w->mode
            && (!w->frame_opacity || !win_has_frame(w))
            && win_is_fullscreen(ps, w))
          ps->unredir_possible = true;
      }

      // Reset flags
      w->flags = 0;
    }


    // Avoid setting w->to_paint if w is to be freed
    bool destroyed = (w->opacity_tgt == w->opacity && w->destroyed);

    if (to_paint) {
      w->prev_trans = t;
      t = w;
    }
    else {
      check_fade_fin(ps, w);
    }

    if (!destroyed)
      w->to_paint = to_paint;
  }

  // If possible, unredirect all windows and stop painting
  if (ps->o.unredir_if_possible && ps->unredir_possible) {
    redir_stop(ps);
  }
  else {
    redir_start(ps);
  }

  // Fetch pictures only if windows are redirected
  if (ps->redirected) {
    for (w = t; w; w = w->prev_trans) {
      // Fetch the picture and pixmap if needed
      if (!w->picture) {
        XRenderPictureAttributes pa;
        XRenderPictFormat *format;
        Drawable draw = w->id;

        if (ps->has_name_pixmap && !w->pixmap) {
          set_ignore_next(ps);
          w->pixmap = XCompositeNameWindowPixmap(ps->dpy, w->id);
        }
        if (w->pixmap) draw = w->pixmap;

        format = XRenderFindVisualFormat(ps->dpy, w->a.visual);
        pa.subwindow_mode = IncludeInferiors;
        w->picture = XRenderCreatePicture(
          ps->dpy, draw, format, CPSubwindowMode, &pa);
      }
    }
  }

  return t;
}

/**
 * Paint the shadow of a window.
 */
static inline void
win_paint_shadow(session_t *ps, win *w, Picture tgt_buffer) {
  XRenderComposite(
    ps->dpy, PictOpOver, w->shadow_pict, w->shadow_alpha_pict,
    tgt_buffer, 0, 0, 0, 0,
    w->a.x + w->shadow_dx, w->a.y + w->shadow_dy,
    w->shadow_width, w->shadow_height);
}

/**
 * Create an alternative picture for a window.
 */
static inline Picture
win_build_picture(session_t *ps, win *w, Visual *visual) {
  const int wid = w->widthb;
  const int hei = w->heightb;
  int depth = 0;
  XRenderPictFormat *pictformat = NULL;

  if (visual && ps->vis != visual) {
    pictformat = XRenderFindVisualFormat(ps->dpy, visual);
    depth = pictformat->depth;
  }
  else {
    pictformat = XRenderFindVisualFormat(ps->dpy, ps->vis);
  }

  if (!depth)
    depth = ps->depth;

  Pixmap tmp_pixmap = XCreatePixmap(ps->dpy, ps->root, wid, hei, depth);
  if (!tmp_pixmap)
    return None;

  Picture tmp_picture = XRenderCreatePicture(ps->dpy, tmp_pixmap,
    pictformat, 0, 0);
  free_pixmap(ps, &tmp_pixmap);

  return tmp_picture;
}

/**
 * Blur the background of a window.
 */
static inline void
win_blur_background(session_t *ps, win *w, Picture tgt_buffer,
    XserverRegion reg_paint) {
  const static int convolution_blur_size = 3;
  // Convolution filter parameter (box blur)
  // gaussian or binomial filters are definitely superior, yet looks
  // like they aren't supported as of xorg-server-1.13.0
  XFixed convolution_blur[] = {
    // Must convert to XFixed with XDoubleToFixed()
    // Matrix size
    XDoubleToFixed(convolution_blur_size),
    XDoubleToFixed(convolution_blur_size),
    // Matrix
    XDoubleToFixed(1), XDoubleToFixed(1), XDoubleToFixed(1),
    XDoubleToFixed(1), XDoubleToFixed(1), XDoubleToFixed(1),
    XDoubleToFixed(1), XDoubleToFixed(1), XDoubleToFixed(1),
  };

  const int x = w->a.x;
  const int y = w->a.y;
  const int wid = w->widthb;
  const int hei = w->heightb;

  // Directly copying from tgt_buffer does not work, so we create a
  // Picture in the middle.
  Picture tmp_picture = win_build_picture(ps, w, NULL);

  if (!tmp_picture)
    return;

  // Adjust blur strength according to window opacity, to make it appear
  // better during fading
  if (!ps->o.blur_background_fixed) {
    double pct = 1.0 - get_opacity_percent(w) * (1.0 - 1.0 / 9.0);
    convolution_blur[2 + convolution_blur_size + ((convolution_blur_size - 1) / 2)] = XDoubleToFixed(pct * 8.0 / (1.1 - pct));
  }

  // Minimize the region we try to blur, if the window itself is not
  // opaque, only the frame is.
  if (WMODE_SOLID == w->mode && w->frame_opacity) {
    XserverRegion reg_all = border_size(ps, w, false);
    XserverRegion reg_noframe = win_get_region_noframe(ps, w, false);
    XFixesSubtractRegion(ps->dpy, reg_noframe, reg_all, reg_noframe);
    XFixesSetPictureClipRegion(ps->dpy, tmp_picture, reg_noframe, 0, 0);
    free_region(ps, &reg_all);
    free_region(ps, &reg_noframe);
  }

  // Copy the content to tmp_picture, then copy back. The filter must
  // be applied on tgt_buffer, to get the nearby pixels outside the
  // window.
  XRenderSetPictureFilter(ps->dpy, tgt_buffer, XRFILTER_CONVOLUTION, (XFixed *) convolution_blur, sizeof(convolution_blur) / sizeof(XFixed));
  XRenderComposite(ps->dpy, PictOpSrc, tgt_buffer, None, tmp_picture, x, y, 0, 0, 0, 0, wid, hei);
  xrfilter_reset(ps, tgt_buffer);
  XRenderComposite(ps->dpy, PictOpSrc, tmp_picture, None, tgt_buffer, 0, 0, 0, 0, x, y, wid, hei);

  free_picture(ps, &tmp_picture);
}

/**
 * Paint a window itself and dim it if asked.
 */
static inline void
win_paint_win(session_t *ps, win *w, Picture tgt_buffer) {
  int x = w->a.x;
  int y = w->a.y;
  int wid = w->widthb;
  int hei = w->heightb;

  Picture alpha_mask = (OPAQUE == w->opacity ? None: w->alpha_pict);
  int op = (w->mode == WMODE_SOLID ? PictOpSrc: PictOpOver);
  Picture pict = w->picture;

  // Invert window color, if required
  if (w->invert_color) {
    Picture newpict = win_build_picture(ps, w, w->a.visual);
    if (newpict) {
      XRenderComposite(ps->dpy, PictOpSrc, pict, None,
          newpict, 0, 0, 0, 0, 0, 0, wid, hei);
      XRenderComposite(ps->dpy, PictOpDifference, ps->white_picture, None,
          newpict, 0, 0, 0, 0, 0, 0, wid, hei);
      // We use an extra PictOpInReverse operation to get correct pixel
      // alpha. There could be a better solution.
      if (WMODE_ARGB == w->mode)
        XRenderComposite(ps->dpy, PictOpInReverse, pict, None,
            newpict, 0, 0, 0, 0, 0, 0, wid, hei);
      pict = newpict;
    }
  }

  if (!w->frame_opacity) {
    XRenderComposite(ps->dpy, op, pict, alpha_mask,
        tgt_buffer, 0, 0, 0, 0, x, y, wid, hei);
  }
  else {
    // Painting parameters
    const int t = w->a.border_width + w->top_width;
    const int l = w->a.border_width + w->left_width;
    const int b = w->a.border_width + w->bottom_width;
    const int r = w->a.border_width + w->right_width;

#define COMP_BDR(cx, cy, cwid, chei) \
    XRenderComposite(ps->dpy, PictOpOver, pict, w->frame_alpha_pict, \
        tgt_buffer, (cx), (cy), 0, 0, x + (cx), y + (cy), (cwid), (chei))

    // The following complicated logic is required because some broken
    // window managers (I'm talking about you, Openbox!) that makes
    // top_width + bottom_width > height in some cases.

    // top
    int phei = min_i(t, hei);
    if (phei > 0)
      COMP_BDR(0, 0, wid, phei);

    if (hei > t) {
      phei = min_i(hei - t, b);

      // bottom
      if (phei > 0)
        COMP_BDR(0, hei - phei, wid, phei);

      phei = hei - t - phei;
      if (phei > 0) {
        int pwid = min_i(l, wid);
        // left
        if (pwid > 0)
          COMP_BDR(0, t, pwid, phei);

        if (wid > l) {
          pwid = min_i(wid - l, r);

          // right
          if (pwid > 0)
            COMP_BDR(wid - pwid, t, pwid, phei);

          pwid = wid - l - pwid;
          if (pwid > 0) {
            // body
            XRenderComposite(ps->dpy, op, pict, alpha_mask,
                tgt_buffer, l, t, 0, 0, x + l, y + t, pwid, phei);
          }
        }
      }
    }
  }

#undef COMP_BDR

  if (pict != w->picture)
    free_picture(ps, &pict);

  // Dimming the window if needed
  if (w->dim && w->dim_alpha_pict != ps->alpha_picts[0]) {
    XRenderComposite(ps->dpy, PictOpOver, ps->black_picture,
        w->dim_alpha_pict, tgt_buffer, 0, 0, 0, 0, x, y, wid, hei);
  }
}

/**
 * Rebuild cached <code>screen_reg</code>.
 */
static void
rebuild_screen_reg(session_t *ps) {
  if (ps->screen_reg)
    XFixesDestroyRegion(ps->dpy, ps->screen_reg);
  ps->screen_reg = get_screen_region(ps);
}

static void
paint_all(session_t *ps, XserverRegion region, win *t) {
#ifdef DEBUG_REPAINT
  static struct timespec last_paint = { 0 };
#endif

  win *w;
  XserverRegion reg_paint = None, reg_tmp = None, reg_tmp2 = None;

  if (!region) {
    region = get_screen_region(ps);
  }
  else {
    // Remove the damaged area out of screen
    XFixesIntersectRegion(ps->dpy, region, region, ps->screen_reg);
  }

#ifdef MONITOR_REPAINT
  // Note: MONITOR_REPAINT cannot work with DBE right now.
  ps->tgt_buffer = ps->tgt_picture;
#else
  if (!ps->tgt_buffer) {
    // DBE painting mode: Directly paint to a Picture of the back buffer
    if (ps->o.dbe) {
      ps->tgt_buffer = XRenderCreatePicture(ps->dpy, ps->root_dbe,
          XRenderFindVisualFormat(ps->dpy, ps->vis),
          0, 0);
    }
    // No-DBE painting mode: Paint to an intermediate Picture then paint
    // the Picture to root window
    else {
      Pixmap root_pixmap = XCreatePixmap(
        ps->dpy, ps->root, ps->root_width, ps->root_height,
        ps->depth);

      ps->tgt_buffer = XRenderCreatePicture(ps->dpy, root_pixmap,
        XRenderFindVisualFormat(ps->dpy, ps->vis),
        0, 0);

      XFreePixmap(ps->dpy, root_pixmap);
    }
  }
#endif

  XFixesSetPictureClipRegion(ps->dpy, ps->tgt_picture, 0, 0, region);

#ifdef MONITOR_REPAINT
  XRenderComposite(
    ps->dpy, PictOpSrc, ps->black_picture, None,
    ps->tgt_picture, 0, 0, 0, 0, 0, 0,
    ps->root_width, ps->root_height);
#endif

  if (t && t->reg_ignore) {
    // Calculate the region upon which the root window is to be painted
    // based on the ignore region of the lowest window, if available
    reg_paint = reg_tmp = XFixesCreateRegion(ps->dpy, NULL, 0);
    XFixesSubtractRegion(ps->dpy, reg_paint, region, t->reg_ignore);
  }
  else {
    reg_paint = region;
  }

  XFixesSetPictureClipRegion(ps->dpy, ps->tgt_buffer, 0, 0, reg_paint);

  paint_root(ps, ps->tgt_buffer);

  // Create temporary regions for use during painting
  if (!reg_tmp)
    reg_tmp = XFixesCreateRegion(ps->dpy, NULL, 0);
  reg_tmp2 = XFixesCreateRegion(ps->dpy, NULL, 0);

  for (w = t; w; w = w->prev_trans) {
    // Painting shadow
    if (w->shadow) {
      // Shadow is to be painted based on the ignore region of current
      // window
      if (w->reg_ignore) {
        if (w == t) {
          // If it's the first cycle and reg_tmp2 is not ready, calculate
          // the paint region here
          reg_paint = reg_tmp;
          XFixesSubtractRegion(ps->dpy, reg_paint, region, w->reg_ignore);
        }
        else {
          // Otherwise, used the cached region during last cycle
          reg_paint = reg_tmp2;
        }
        XFixesIntersectRegion(ps->dpy, reg_paint, reg_paint, w->extents);
      }
      else {
        reg_paint = reg_tmp;
        XFixesIntersectRegion(ps->dpy, reg_paint, region, w->extents);
      }
      // Clear the shadow here instead of in make_shadow() for saving GPU
      // power and handling shaped windows
      if (ps->o.clear_shadow && w->border_size)
        XFixesSubtractRegion(ps->dpy, reg_paint, reg_paint, w->border_size);

      // Detect if the region is empty before painting
      if (region == reg_paint || !is_region_empty(ps, reg_paint)) {
        XFixesSetPictureClipRegion(ps->dpy, ps->tgt_buffer, 0, 0,
            reg_paint);

        win_paint_shadow(ps, w, ps->tgt_buffer);
      }
    }

    // Calculate the region based on the reg_ignore of the next (higher)
    // window and the bounding region
    reg_paint = reg_tmp;
    if (w->prev_trans && w->prev_trans->reg_ignore) {
      XFixesSubtractRegion(ps->dpy, reg_paint, region,
          w->prev_trans->reg_ignore);
      // Copy the subtracted region to be used for shadow painting in next
      // cycle
      XFixesCopyRegion(ps->dpy, reg_tmp2, reg_paint);

      if (w->border_size)
        XFixesIntersectRegion(ps->dpy, reg_paint, reg_paint, w->border_size);
    }
    else {
      if (w->border_size)
        XFixesIntersectRegion(ps->dpy, reg_paint, region, w->border_size);
      else
        reg_paint = region;
    }

    if (!is_region_empty(ps, reg_paint)) {
      XFixesSetPictureClipRegion(ps->dpy, ps->tgt_buffer, 0, 0, reg_paint);
      // Blur window background
      if ((ps->o.blur_background && WMODE_SOLID != w->mode)
          || (ps->o.blur_background_frame && w->frame_opacity)) {
        win_blur_background(ps, w, ps->tgt_buffer, reg_paint);
      }

      // Painting the window
      win_paint_win(ps, w, ps->tgt_buffer);
    }

    check_fade_fin(ps, w);
  }

  // Free up all temporary regions
  XFixesDestroyRegion(ps->dpy, region);
  XFixesDestroyRegion(ps->dpy, reg_tmp);
  XFixesDestroyRegion(ps->dpy, reg_tmp2);

  // Do this as early as possible
  if (!ps->o.dbe)
    XFixesSetPictureClipRegion(ps->dpy, ps->tgt_buffer, 0, 0, None);

  if (VSYNC_NONE != ps->o.vsync) {
    // Make sure all previous requests are processed to achieve best
    // effect
    XSync(ps->dpy, False);
  }

  // Wait for VBlank. We could do it aggressively (send the painting
  // request and XFlush() on VBlank) or conservatively (send the request
  // only on VBlank).
  if (!ps->o.vsync_aggressive)
    vsync_wait(ps);

  // DBE painting mode, only need to swap the buffer
  if (ps->o.dbe) {
    XdbeSwapInfo swap_info = {
      .swap_window = get_tgt_window(ps),
      // Is it safe to use XdbeUndefined?
      .swap_action = XdbeCopied
    };
    XdbeSwapBuffers(ps->dpy, &swap_info, 1);
  }
  // No-DBE painting mode
  else if (ps->tgt_buffer != ps->tgt_picture) {
    XRenderComposite(
      ps->dpy, PictOpSrc, ps->tgt_buffer, None,
      ps->tgt_picture, 0, 0, 0, 0,
      0, 0, ps->root_width, ps->root_height);
  }

  if (ps->o.vsync_aggressive)
    vsync_wait(ps);

  XFlush(ps->dpy);

#ifdef DEBUG_REPAINT
  print_timestamp(ps);
  struct timespec now = get_time_timespec();
  struct timespec diff = { 0 };
  timespec_subtract(&diff, &now, &last_paint);
  printf("[ %5ld:%09ld ] ", diff.tv_sec, diff.tv_nsec);
  last_paint = now;
  printf("paint:");
  for (w = t; w; w = w->prev_trans)
    printf(" %#010lx", w->id);
  putchar('\n');
  fflush(stdout);
#endif
}

static void
add_damage(session_t *ps, XserverRegion damage) {
  if (ps->all_damage) {
    XFixesUnionRegion(ps->dpy, ps->all_damage, ps->all_damage, damage);
    XFixesDestroyRegion(ps->dpy, damage);
  } else {
    ps->all_damage = damage;
  }
}

static void
repair_win(session_t *ps, win *w) {
  if (IsViewable != w->a.map_state)
    return;

  XserverRegion parts;

  if (!w->damaged) {
    parts = win_extents(ps, w);
    set_ignore_next(ps);
    XDamageSubtract(ps->dpy, w->damage, None, None);
  } else {
    parts = XFixesCreateRegion(ps->dpy, 0, 0);
    set_ignore_next(ps);
    XDamageSubtract(ps->dpy, w->damage, None, parts);
    XFixesTranslateRegion(ps->dpy, parts,
      w->a.x + w->a.border_width,
      w->a.y + w->a.border_width);
  }

  // Remove the part in the damage area that could be ignored
  if (!ps->reg_ignore_expire && w->prev_trans && w->prev_trans->reg_ignore)
    XFixesSubtractRegion(ps->dpy, parts, parts, w->prev_trans->reg_ignore);

  add_damage(ps, parts);
  w->damaged = true;
}

static wintype_t
wid_get_prop_wintype(session_t *ps, Window wid) {
  int i;

  set_ignore_next(ps);
  winprop_t prop = wid_get_prop(ps, wid, ps->atom_win_type, 32L, XA_ATOM, 32);

  for (i = 0; i < prop.nitems; ++i) {
    for (wintype_t j = 1; j < NUM_WINTYPES; ++j) {
      if (ps->atoms_wintypes[j] == (Atom) prop.data.p32[i]) {
        free_winprop(&prop);
        return j;
      }
    }
  }

  free_winprop(&prop);

  return WINTYPE_UNKNOWN;
}

static void
map_win(session_t *ps, Window id) {
  win *w = find_win(ps, id);

  // Don't care about window mapping if it's an InputOnly window
  // Try avoiding mapping a window twice
  if (!w || InputOnly == w->a.class
      || IsViewable == w->a.map_state)
    return;

  assert(!w->focused_real);

  w->a.map_state = IsViewable;

  // Set focused to false
  bool focused_real = false;
  if (ps->o.track_focus && ps->o.use_ewmh_active_win
      && w == ps->active_win)
    focused_real = true;
  win_set_focused(ps, w, focused_real);

  // Call XSelectInput() before reading properties so that no property
  // changes are lost
  XSelectInput(ps->dpy, id, determine_evmask(ps, id, WIN_EVMODE_FRAME));

  // Notify compton when the shape of a window changes
  if (ps->shape_exists) {
    XShapeSelectInput(ps->dpy, id, ShapeNotifyMask);
  }

  // Make sure the XSelectInput() requests are sent
  XSync(ps->dpy, False);

  // Detect client window here instead of in add_win() as the client
  // window should have been prepared at this point
  if (!w->client_win) {
    win_recheck_client(ps, w);
  }
  else {
    // Re-mark client window here
    win_mark_client(ps, w, w->client_win);
  }

  assert(w->client_win);

#ifdef DEBUG_WINTYPE
  printf_dbgf("(%#010lx): type %s\n", w->id, WINTYPES[w->window_type]);
#endif

  // Detect if the window is shaped or has rounded corners
  win_update_shape_raw(ps, w);

  // Occasionally compton does not seem able to get a FocusIn event from
  // a window just mapped. I suspect it's a timing issue again when the
  // XSelectInput() is called too late. We have to recheck the focused
  // window here. It makes no sense if we are using EWMH
  // _NET_ACTIVE_WINDOW.
  if (ps->o.track_focus && !ps->o.use_ewmh_active_win) {
    recheck_focus(ps);
  }

  // Update window focus state
  win_update_focused(ps, w);

  // Update opacity and dim state
  win_update_opacity_prop(ps, w);
  w->flags |= WFLAG_OPCT_CHANGE;

  // Check for _COMPTON_SHADOW
  if (ps->o.respect_prop_shadow)
    win_update_prop_shadow_raw(ps, w);

  // Many things above could affect shadow
  win_determine_shadow(ps, w);

  // Set fading state
  w->in_openclose = false;
  if (ps->o.no_fading_openclose) {
    set_fade_callback(ps, w, finish_map_win, true);
    w->in_openclose = true;
  }
  else {
    set_fade_callback(ps, w, NULL, true);
  }
  win_determine_fade(ps, w);

  w->damaged = true;

  /* if any configure events happened while
     the window was unmapped, then configure
     the window to its correct place */
  if (w->need_configure) {
    configure_win(ps, &w->queue_configure);
  }

#ifdef CONFIG_DBUS
  // Send D-Bus signal
  if (ps->o.dbus) {
    cdbus_ev_win_mapped(ps, w);
  }
#endif
}

static void
finish_map_win(session_t *ps, win *w) {
  w->in_openclose = false;
  if (ps->o.no_fading_openclose) {
    win_determine_fade(ps, w);
  }
}

static void
finish_unmap_win(session_t *ps, win *w) {
  w->damaged = false;

  w->in_openclose = false;

  update_reg_ignore_expire(ps, w);

  if (w->extents != None) {
    /* destroys region */
    add_damage(ps, w->extents);
    w->extents = None;
  }

  free_pixmap(ps, &w->pixmap);
  free_picture(ps, &w->picture);
  free_region(ps, &w->border_size);
  free_picture(ps, &w->shadow_pict);
}

static void
unmap_callback(session_t *ps, win *w) {
  finish_unmap_win(ps, w);
}

static void
unmap_win(session_t *ps, Window id) {
  win *w = find_win(ps, id);

  if (!w || IsUnmapped == w->a.map_state) return;

  // Set focus out
  win_set_focused(ps, w, false);

  w->a.map_state = IsUnmapped;

  // Fading out
  w->flags |= WFLAG_OPCT_CHANGE;
  set_fade_callback(ps, w, unmap_callback, false);
  if (ps->o.no_fading_openclose) {
    w->in_openclose = true;
    win_determine_fade(ps, w);
  }

  // don't care about properties anymore
  win_ev_stop(ps, w);

#ifdef CONFIG_DBUS
  // Send D-Bus signal
  if (ps->o.dbus) {
    cdbus_ev_win_unmapped(ps, w);
  }
#endif
}

static opacity_t
wid_get_opacity_prop(session_t *ps, Window wid, opacity_t def) {
  opacity_t val = def;

  winprop_t prop = wid_get_prop(ps, wid, ps->atom_opacity, 1L,
      XA_CARDINAL, 32);

  if (prop.nitems)
    val = *prop.data.p32;

  free_winprop(&prop);

  return val;
}

static double
get_opacity_percent(win *w) {
  return ((double) w->opacity) / OPAQUE;
}

static void
win_determine_mode(session_t *ps, win *w) {
  winmode_t mode = WMODE_SOLID;
  XRenderPictFormat *format;

  /* if trans prop == -1 fall back on previous tests */

  if (w->a.class == InputOnly) {
    format = 0;
  } else {
    format = XRenderFindVisualFormat(ps->dpy, w->a.visual);
  }

  if (format && format->type == PictTypeDirect
      && format->direct.alphaMask) {
    mode = WMODE_ARGB;
  } else if (w->opacity != OPAQUE) {
    mode = WMODE_TRANS;
  } else {
    mode = WMODE_SOLID;
  }

  w->mode = mode;
}

/**
 * Calculate and set the opacity of a window.
 *
 * If window is inactive and inactive_opacity_override is set, the
 * priority is: (Simulates the old behavior)
 *
 * inactive_opacity > _NET_WM_WINDOW_OPACITY (if not opaque)
 * > window type default opacity
 *
 * Otherwise:
 *
 * _NET_WM_WINDOW_OPACITY (if not opaque)
 * > window type default opacity (if not opaque)
 * > inactive_opacity
 *
 * @param ps current session
 * @param w struct _win object representing the window
 */
static void
calc_opacity(session_t *ps, win *w) {
  opacity_t opacity = OPAQUE;

  if (w->destroyed || IsViewable != w->a.map_state)
    opacity = 0;
  else {
    // Try obeying opacity property and window type opacity firstly
    if (OPAQUE == (opacity = w->opacity_prop)
        && OPAQUE == (opacity = w->opacity_prop_client)) {
      opacity = ps->o.wintype_opacity[w->window_type] * OPAQUE;
    }

    // Respect inactive_opacity in some cases
    if (ps->o.inactive_opacity && false == w->focused
        && (OPAQUE == opacity || ps->o.inactive_opacity_override)) {
      opacity = ps->o.inactive_opacity;
    }
  }

  w->opacity_tgt = opacity;
}

/**
 * Determine whether a window is to be dimmed.
 */
static void
calc_dim(session_t *ps, win *w) {
  bool dim;

  // Make sure we do nothing if the window is unmapped / destroyed
  if (w->destroyed || IsViewable != w->a.map_state)
    return;

  if (ps->o.inactive_dim && !(w->focused)) {
    dim = true;
  } else {
    dim = false;
  }

  if (dim != w->dim) {
    w->dim = dim;
    add_damage_win(ps, w);
  }
}

/**
 * Determine if a window should fade on opacity change.
 */
static void
win_determine_fade(session_t *ps, win *w) {
  if (ps->o.no_fading_openclose && w->in_openclose)
    w->fade = false;
  else
    w->fade = ps->o.wintype_fade[w->window_type];
}

/**
 * Update window-shape.
 */
static void
win_update_shape_raw(session_t *ps, win *w) {
  if (ps->shape_exists) {
    w->bounding_shaped = wid_bounding_shaped(ps, w->id);
    if (w->bounding_shaped && ps->o.detect_rounded_corners)
      win_rounded_corners(ps, w);
  }
}

/**
 * Update window-shape related information.
 */
static void
win_update_shape(session_t *ps, win *w) {
  if (ps->shape_exists) {
    // bool bounding_shaped_old = w->bounding_shaped;

    win_update_shape_raw(ps, w);

    // Shadow state could be changed
    win_determine_shadow(ps, w);

    /*
    // If clear_shadow state on the window possibly changed, destroy the old
    // shadow_pict
    if (ps->o.clear_shadow && w->bounding_shaped != bounding_shaped_old)
      free_picture(ps, &w->shadow_pict);
    */
  }
}

/**
 * Reread _COMPTON_SHADOW property from a window.
 *
 * The property must be set on the outermost window, usually the WM frame.
 */
static void
win_update_prop_shadow_raw(session_t *ps, win *w) {
  winprop_t prop = wid_get_prop(ps, w->id, ps->atom_compton_shadow, 1,
      XA_CARDINAL, 32);

  if (!prop.nitems) {
    w->prop_shadow = -1;
  }
  else {
    w->prop_shadow = *prop.data.p32;
  }

  free_winprop(&prop);
}

/**
 * Reread _COMPTON_SHADOW property from a window and update related
 * things.
 */
static void
win_update_prop_shadow(session_t *ps, win *w) {
  long attr_shadow_old = w->prop_shadow;

  win_update_prop_shadow_raw(ps, w);

  if (w->prop_shadow != attr_shadow_old)
    win_determine_shadow(ps, w);
}

/**
 * Determine if a window should have shadow, and update things depending
 * on shadow state.
 */
static void
win_determine_shadow(session_t *ps, win *w) {
  bool shadow_old = w->shadow;

  w->shadow = (UNSET == w->shadow_force ?
      (ps->o.wintype_shadow[w->window_type]
       && !win_match(w, ps->o.shadow_blacklist, &w->cache_sblst)
       && !(ps->o.shadow_ignore_shaped && w->bounding_shaped
         && !w->rounded_corners)
       && !(ps->o.respect_prop_shadow && 0 == w->prop_shadow))
       : w->shadow_force);

  // Window extents need update on shadow state change
  if (w->shadow != shadow_old) {
    // Shadow geometry currently doesn't change on shadow state change
    // calc_shadow_geometry(ps, w);
    if (w->extents) {
      // Mark the old extents as damaged if the shadow is removed
      if (!w->shadow)
        add_damage(ps, w->extents);
      else
        free_region(ps, &w->extents);
      w->extents = win_extents(ps, w);
      // Mark the new extents as damaged if the shadow is added
      if (w->shadow)
        add_damage_win(ps, w);
    }
  }
}

/**
 * Determine if a window should have color inverted.
 */
static void
win_determine_invert_color(session_t *ps, win *w) {
  bool invert_color_old = w->invert_color;

  if (UNSET != w->invert_color_force)
    w->invert_color = w->invert_color_force;
  else 
    w->invert_color = win_match(w, ps->o.invert_color_list, &w->cache_ivclst);

  if (w->invert_color != invert_color_old)
    add_damage_win(ps, w);
}

/**
 * Function to be called on window type changes.
 */
static void
win_on_wtype_change(session_t *ps, win *w) {
  win_determine_shadow(ps, w);
  win_determine_fade(ps, w);
  win_update_focused(ps, w);
}

/**
 * Function to be called on window data changes.
 */
static void
win_on_wdata_change(session_t *ps, win *w) {
  if (ps->o.shadow_blacklist)
    win_determine_shadow(ps, w);
  if (ps->o.fade_blacklist)
    win_determine_fade(ps, w);
  if (ps->o.invert_color_list)
    win_determine_invert_color(ps, w);
  if (ps->o.focus_blacklist)
    win_update_focused(ps, w);
}

/**
 * Process needed window updates.
 */
static void
win_upd_run(session_t *ps, win *w, win_upd_t *pupd) {
  if (pupd->shadow) {
    win_determine_shadow(ps, w);
    pupd->shadow = false;
  }
  if (pupd->fade) {
    win_determine_fade(ps, w);
    pupd->fade = false;
  }
  if (pupd->invert_color) {
    win_determine_invert_color(ps, w);
    pupd->invert_color = false;
  }
  if (pupd->focus) {
    win_update_focused(ps, w);
    pupd->focus = false;
  }
}

/**
 * Update cache data in struct _win that depends on window size.
 */
static void
calc_win_size(session_t *ps, win *w) {
  w->widthb = w->a.width + w->a.border_width * 2;
  w->heightb = w->a.height + w->a.border_width * 2;
  calc_shadow_geometry(ps, w);
  w->flags |= WFLAG_SIZE_CHANGE;
}

/**
 * Calculate and update geometry of the shadow of a window.
 */
static void
calc_shadow_geometry(session_t *ps, win *w) {
  w->shadow_dx = ps->o.shadow_offset_x;
  w->shadow_dy = ps->o.shadow_offset_y;
  w->shadow_width = w->widthb + ps->gaussian_map->size;
  w->shadow_height = w->heightb + ps->gaussian_map->size;
}

/**
 * Mark a window as the client window of another.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 * @param client window ID of the client window
 */
static void
win_mark_client(session_t *ps, win *w, Window client) {
  w->client_win = client;

  // If the window isn't mapped yet, stop here, as the function will be
  // called in map_win()
  if (IsViewable != w->a.map_state)
    return;

  XSelectInput(ps->dpy, client,
      determine_evmask(ps, client, WIN_EVMODE_CLIENT));

  // Make sure the XSelectInput() requests are sent
  XSync(ps->dpy, False);

  // Get frame widths if needed
  if (ps->o.frame_opacity) {
    get_frame_extents(ps, w, client);
  }

  {
    wintype_t wtype_old = w->window_type;

    // Detect window type here
    if (WINTYPE_UNKNOWN == w->window_type)
      w->window_type = wid_get_prop_wintype(ps, w->client_win);

    // Conform to EWMH standard, if _NET_WM_WINDOW_TYPE is not present, take
    // override-redirect windows or windows without WM_TRANSIENT_FOR as
    // _NET_WM_WINDOW_TYPE_NORMAL, otherwise as _NET_WM_WINDOW_TYPE_DIALOG.
    if (WINTYPE_UNKNOWN == w->window_type) {
      if (w->a.override_redirect
          || !wid_has_prop(ps, client, ps->atom_transient))
        w->window_type = WINTYPE_NORMAL;
      else
        w->window_type = WINTYPE_DIALOG;
    }

    if (w->window_type != wtype_old)
      win_on_wtype_change(ps, w);
  }

  // Get window group
  if (ps->o.track_leader)
    win_update_leader(ps, w);

  // Get window name and class if we are tracking them
  if (ps->o.track_wdata) {
    win_get_name(ps, w);
    win_get_class(ps, w);
    win_get_role(ps, w);
    win_on_wdata_change(ps, w);
  }

  // Update window focus state
  win_update_focused(ps, w);
}

/**
 * Unmark current client window of a window.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 */
static void
win_unmark_client(session_t *ps, win *w) {
  Window client = w->client_win;

  w->client_win = None;

  // Recheck event mask
  XSelectInput(ps->dpy, client,
      determine_evmask(ps, client, WIN_EVMODE_UNKNOWN));
}

/**
 * Recheck client window of a window.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 */
static void
win_recheck_client(session_t *ps, win *w) {
  // Initialize wmwin to false
  w->wmwin = false;

  // Look for the client window

  // Always recursively look for a window with WM_STATE, as Fluxbox
  // sets override-redirect flags on all frame windows.
  Window cw = find_client_win(ps, w->id);
#ifdef DEBUG_CLIENTWIN
  if (cw)
    printf_dbgf("(%#010lx): client %#010lx\n", w->id, cw);
#endif
  // Set a window's client window to itself if we couldn't find a
  // client window
  if (!cw) {
    cw = w->id;
    w->wmwin = !w->a.override_redirect;
#ifdef DEBUG_CLIENTWIN
    printf_dbgf("(%#010lx): client self (%s)\n", w->id,
        (w->wmwin ? "wmwin": "override-redirected"));
#endif
  }

  // Unmark the old one
  if (w->client_win && w->client_win != cw)
    win_unmark_client(ps, w);

  // Mark the new one
  win_mark_client(ps, w, cw);
}

static bool
add_win(session_t *ps, Window id, Window prev) {
  const static win win_def = {
    .next = NULL,
    .prev_trans = NULL,

    .id = None,
    .a = { },
    .mode = WMODE_TRANS,
    .damaged = false,
    .damage = None,
    .pixmap = None,
    .picture = None,
    .border_size = None,
    .extents = None,
    .flags = 0,
    .need_configure = false,
    .queue_configure = { },
    .reg_ignore = None,
    .widthb = 0,
    .heightb = 0,
    .destroyed = false,
    .bounding_shaped = false,
    .rounded_corners = false,
    .to_paint = false,
    .in_openclose = false,

    .client_win = None,
    .window_type = WINTYPE_UNKNOWN,
    .wmwin = false,
    .leader = None,
    .cache_leader = None,

    .focused = false,
    .focused_force = UNSET,
    .focused_real = false,

    .name = NULL,
    .class_instance = NULL,
    .class_general = NULL,
    .role = NULL,
    .cache_sblst = NULL,
    .cache_fblst = NULL,
    .cache_fcblst = NULL,
    .cache_ivclst = NULL,

    .opacity = 0,
    .opacity_tgt = 0,
    .opacity_prop = OPAQUE,
    .opacity_prop_client = OPAQUE,
    .alpha_pict = None,

    .fade = false,
    .fade_callback = NULL,

    .frame_opacity = 0.0,
    .frame_alpha_pict = None,
    .left_width = 0,
    .right_width = 0,
    .top_width = 0,
    .bottom_width = 0,

    .shadow = false,
    .shadow_force = UNSET,
    .shadow_opacity = 0.0,
    .shadow_dx = 0,
    .shadow_dy = 0,
    .shadow_width = 0,
    .shadow_height = 0,
    .shadow_pict = None,
    .shadow_alpha_pict = None,
    .prop_shadow = -1,

    .dim = false,
    .dim_alpha_pict = None,

    .invert_color = false,
    .invert_color_force = UNSET,
  };

  // Reject overlay window and already added windows
  if (id == ps->overlay || find_win(ps, id)) {
    return false;
  }

  // Allocate and initialize the new win structure
  win *new = malloc(sizeof(win));

  if (!new) {
    printf_errf("(%#010lx): Failed to allocate memory for the new window.", id);
    return false;
  }

  memcpy(new, &win_def, sizeof(win));

  // Find window insertion point
  win **p = NULL;
  if (prev) {
    for (p = &ps->list; *p; p = &(*p)->next) {
      if ((*p)->id == prev && !(*p)->destroyed)
        break;
    }
  } else {
    p = &ps->list;
  }

  // Fill structure
  new->id = id;

  set_ignore_next(ps);
  if (!XGetWindowAttributes(ps->dpy, id, &new->a)) {
    // Failed to get window attributes. Which probably means, the window
    // is gone already.
    free(new);
    return false;
  }

  // Delay window mapping
  int map_state = new->a.map_state;
  assert(IsViewable == map_state || IsUnmapped == map_state);
  new->a.map_state = IsUnmapped;

  // Create Damage for window
  if (InputOutput == new->a.class) {
    set_ignore_next(ps);
    new->damage = XDamageCreate(ps->dpy, id, XDamageReportNonEmpty);
  }

  calc_win_size(ps, new);

  new->next = *p;
  *p = new;

#ifdef CONFIG_DBUS
  // Send D-Bus signal
  if (ps->o.dbus) {
    cdbus_ev_win_added(ps, new);
  }
#endif

  if (IsViewable == map_state) {
    map_win(ps, id);
  }

  return true;
}

static void
restack_win(session_t *ps, win *w, Window new_above) {
  Window old_above;

  update_reg_ignore_expire(ps, w);

  if (w->next) {
    old_above = w->next->id;
  } else {
    old_above = None;
  }

  if (old_above != new_above) {
    win **prev;

    /* unhook */
    for (prev = &ps->list; *prev; prev = &(*prev)->next) {
      if ((*prev) == w) break;
    }

    *prev = w->next;

    /* rehook */
    for (prev = &ps->list; *prev; prev = &(*prev)->next) {
      if ((*prev)->id == new_above && !(*prev)->destroyed)
        break;
    }

    w->next = *prev;
    *prev = w;

#ifdef DEBUG_RESTACK
    {
      const char *desc;
      char *window_name = NULL;
      bool to_free;
      win* c = ps->list;

      printf_dbgf("(%#010lx, %#010lx): "
             "Window stack modified. Current stack:\n", w->id, new_above);

      for (; c; c = c->next) {
        window_name = "(Failed to get title)";

        to_free = ev_window_name(ps, c->id, &window_name);

        desc = "";
        if (c->destroyed) desc = "(D) ";
        printf("%#010lx \"%s\" %s-> ", c->id, window_name, desc);

        if (to_free) {
          XFree(window_name);
          window_name = NULL;
        }
      }
      fputs("\n", stdout);
    }
#endif
  }
}

static void
configure_win(session_t *ps, XConfigureEvent *ce) {
  if (ce->window == ps->root) {
    if (ps->tgt_buffer) {
      XRenderFreePicture(ps->dpy, ps->tgt_buffer);
      ps->tgt_buffer = None;
    }
    ps->root_width = ce->width;
    ps->root_height = ce->height;

    rebuild_screen_reg(ps);

    return;
  }

  win *w = find_win(ps, ce->window);
  XserverRegion damage = None;

  if (!w)
    return;

  if (w->a.map_state == IsUnmapped) {
    /* save the configure event for when the window maps */
    w->need_configure = true;
    w->queue_configure = *ce;
    restack_win(ps, w, ce->above);
  } else {
    if (!(w->need_configure)) {
      restack_win(ps, w, ce->above);
    }

    // Windows restack (including window restacks happened when this
    // window is not mapped) could mess up all reg_ignore
    ps->reg_ignore_expire = true;

    w->need_configure = false;

    damage = XFixesCreateRegion(ps->dpy, 0, 0);
    if (w->extents != None) {
      XFixesCopyRegion(ps->dpy, damage, w->extents);
    }

    // If window geometry did not change, don't free extents here
    if (w->a.x != ce->x || w->a.y != ce->y
        || w->a.width != ce->width || w->a.height != ce->height
        || w->a.border_width != ce->border_width) {
      free_region(ps, &w->extents);
      free_region(ps, &w->border_size);
    }

    w->a.x = ce->x;
    w->a.y = ce->y;

    if (w->a.width != ce->width || w->a.height != ce->height
        || w->a.border_width != ce->border_width) {
      free_pixmap(ps, &w->pixmap);
      free_picture(ps, &w->picture);
    }

    if (w->a.width != ce->width || w->a.height != ce->height
        || w->a.border_width != ce->border_width) {
      w->a.width = ce->width;
      w->a.height = ce->height;
      w->a.border_width = ce->border_width;
      calc_win_size(ps, w);

      // Rounded corner detection is affected by window size
      if (ps->shape_exists && ps->o.shadow_ignore_shaped
          && ps->o.detect_rounded_corners && w->bounding_shaped)
        win_update_shape(ps, w);
    }

    if (damage) {
      XserverRegion extents = win_extents(ps, w);
      XFixesUnionRegion(ps->dpy, damage, damage, extents);
      XFixesDestroyRegion(ps->dpy, extents);
      add_damage(ps, damage);
    }
  }

  w->a.override_redirect = ce->override_redirect;
}

static void
circulate_win(session_t *ps, XCirculateEvent *ce) {
  win *w = find_win(ps, ce->window);
  Window new_above;

  if (!w) return;

  if (ce->place == PlaceOnTop) {
    new_above = ps->list->id;
  } else {
    new_above = None;
  }

  restack_win(ps, w, new_above);
}

static void
finish_destroy_win(session_t *ps, Window id) {
  win **prev, *w;

  for (prev = &ps->list; (w = *prev); prev = &w->next) {
    if (w->id == id && w->destroyed) {
      finish_unmap_win(ps, w);
      *prev = w->next;

      // Clear active_win if it's pointing to the destroyed window
      if (w == ps->active_win)
        ps->active_win = NULL;

      free_win_res(ps, w);

      free(w);
      break;
    }
  }
}

static void
destroy_callback(session_t *ps, win *w) {
  finish_destroy_win(ps, w->id);
}

static void
destroy_win(session_t *ps, Window id) {
  win *w = find_win(ps, id);

  if (w) {
    w->destroyed = true;

    // Fading out the window
    w->flags |= WFLAG_OPCT_CHANGE;
    set_fade_callback(ps, w, destroy_callback, false);

#ifdef CONFIG_DBUS
    // Send D-Bus signal
    if (ps->o.dbus) {
      cdbus_ev_win_destroyed(ps, w);
    }
#endif
  }
}

static inline void
root_damaged(session_t *ps) {
  if (ps->root_tile) {
    XClearArea(ps->dpy, ps->root, 0, 0, 0, 0, true);
    // if (ps->root_picture != ps->root_tile) {
      XRenderFreePicture(ps->dpy, ps->root_tile);
      ps->root_tile = None;
    /* }
    if (root_damage) {
      XserverRegion parts = XFixesCreateRegion(ps->dpy, 0, 0);
      XDamageSubtract(ps->dpy, root_damage, None, parts);
      add_damage(ps, parts);
    } */
  }

  // Mark screen damaged
  force_repaint(ps);
}

static void
damage_win(session_t *ps, XDamageNotifyEvent *de) {
  /*
  if (ps->root == de->drawable) {
    root_damaged();
    return;
  } */

  win *w = find_win(ps, de->drawable);

  if (!w) return;

  repair_win(ps, w);
}

/**
 * Xlib error handler function.
 */
static int
error(Display __attribute__((unused)) *dpy, XErrorEvent *ev) {
  session_t * const ps = ps_g;

  int o;
  const char *name = "Unknown";

  if (should_ignore(ps, ev->serial)) {
    return 0;
  }

  if (ev->request_code == ps->composite_opcode
      && ev->minor_code == X_CompositeRedirectSubwindows) {
    fprintf(stderr, "Another composite manager is already running\n");
    exit(1);
  }

#define CASESTRRET2(s)   case s: name = #s; break

  o = ev->error_code - ps->xfixes_error;
  switch (o) {
    CASESTRRET2(BadRegion);
  }

  o = ev->error_code - ps->damage_error;
  switch (o) {
    CASESTRRET2(BadDamage);
  }

  o = ev->error_code - ps->render_error;
  switch (o) {
    CASESTRRET2(BadPictFormat);
    CASESTRRET2(BadPicture);
    CASESTRRET2(BadPictOp);
    CASESTRRET2(BadGlyphSet);
    CASESTRRET2(BadGlyph);
  }

  switch (ev->error_code) {
    CASESTRRET2(BadAccess);
    CASESTRRET2(BadAlloc);
    CASESTRRET2(BadAtom);
    CASESTRRET2(BadColor);
    CASESTRRET2(BadCursor);
    CASESTRRET2(BadDrawable);
    CASESTRRET2(BadFont);
    CASESTRRET2(BadGC);
    CASESTRRET2(BadIDChoice);
    CASESTRRET2(BadImplementation);
    CASESTRRET2(BadLength);
    CASESTRRET2(BadMatch);
    CASESTRRET2(BadName);
    CASESTRRET2(BadPixmap);
    CASESTRRET2(BadRequest);
    CASESTRRET2(BadValue);
    CASESTRRET2(BadWindow);
  }

#undef CASESTRRET2

  print_timestamp(ps);
  printf("error %d (%s) request %d minor %d serial %lu\n",
    ev->error_code, name, ev->request_code,
    ev->minor_code, ev->serial);

  return 0;
}

static void
expose_root(session_t *ps, XRectangle *rects, int nrects) {
  XserverRegion region = XFixesCreateRegion(ps->dpy, rects, nrects);
  add_damage(ps, region);
}

/**
 * Get the value of a type-<code>Window</code> property of a window.
 *
 * @return the value if successful, 0 otherwise
 */
static Window
wid_get_prop_window(session_t *ps, Window wid, Atom aprop) {
  // Get the attribute
  Window p = None;
  winprop_t prop = wid_get_prop(ps, wid, aprop, 1L, XA_WINDOW, 32);

  // Return it
  if (prop.nitems) {
    p = *prop.data.p32;
  }

  free_winprop(&prop);

  return p;
}

/**
 * Update focused state of a window.
 */
static void
win_update_focused(session_t *ps, win *w) {
  bool focused_old = w->focused;

  if (UNSET != w->focused_force) {
    w->focused = w->focused_force;
  }
  else {
    w->focused = w->focused_real;

    // Use wintype_focus, and treat WM windows and override-redirected
    // windows specially
    if (ps->o.wintype_focus[w->window_type]
        || (ps->o.mark_wmwin_focused && w->wmwin)
        || (ps->o.mark_ovredir_focused
          && w->id == w->client_win && !w->wmwin)
        || win_match(w, ps->o.focus_blacklist, &w->cache_fcblst))
      w->focused = true;

    // If window grouping detection is enabled, mark the window active if
    // its group is
    if (ps->o.track_leader && ps->active_leader
        && win_get_leader(ps, w) == ps->active_leader) {
      w->focused = true;
    }
  }

  if (w->focused != focused_old)
    w->flags |= WFLAG_OPCT_CHANGE;
}

/**
 * Set real focused state of a window.
 */
static void
win_set_focused(session_t *ps, win *w, bool focused) {
  // Unmapped windows will have their focused state reset on map
  if (IsUnmapped == w->a.map_state)
    return;

  if (w->focused_real != focused) {
    w->focused_real = focused;

    // If window grouping detection is enabled
    if (ps->o.track_leader) {
      Window leader = win_get_leader(ps, w);

      // If the window gets focused, replace the old active_leader
      if (w->focused_real && leader != ps->active_leader) {
        Window active_leader_old = ps->active_leader;

        ps->active_leader = leader;

        group_update_focused(ps, active_leader_old);
        group_update_focused(ps, leader);
      }
      // If the group get unfocused, remove it from active_leader
      else if (!w->focused_real && leader && leader == ps->active_leader
          && !group_is_focused(ps, leader)) {
        ps->active_leader = None;
        group_update_focused(ps, leader);
      }

      // The window itself must be updated anyway
      win_update_focused(ps, w);
    }
    // Otherwise, only update the window itself
    else {
      win_update_focused(ps, w);
    }
  }
}
/**
 * Update leader of a window.
 */
static void
win_update_leader(session_t *ps, win *w) {
  Window leader = None;

  // Read the leader properties
  if (ps->o.detect_transient && !leader)
    leader = wid_get_prop_window(ps, w->client_win, ps->atom_transient);

  if (ps->o.detect_client_leader && !leader)
    leader = wid_get_prop_window(ps, w->client_win, ps->atom_client_leader);

  win_set_leader(ps, w, leader);

#ifdef DEBUG_LEADER
  printf_dbgf("(%#010lx): client %#010lx, leader %#010lx, cache %#010lx\n", w->id, w->client_win, w->leader, win_get_leader(ps, w));
#endif
}

/**
 * Set leader of a window.
 */
static void
win_set_leader(session_t *ps, win *w, Window nleader) {
  // If the leader changes
  if (w->leader != nleader) {
    Window cache_leader_old = win_get_leader(ps, w);

    w->leader = nleader;

    // Forcefully do this to deal with the case when a child window
    // gets mapped before parent, or when the window is a waypoint
    clear_cache_win_leaders(ps);

    // Update the old and new window group and active_leader if the window
    // could affect their state.
    Window cache_leader = win_get_leader(ps, w);
    if (w->focused_real && cache_leader_old != cache_leader) {
      ps->active_leader = cache_leader;

      group_update_focused(ps, cache_leader_old);
      group_update_focused(ps, cache_leader);
    }
    // Otherwise, at most the window itself is affected
    else {
      win_update_focused(ps, w);
    }
  }
}

/**
 * Internal function of win_get_leader().
 */
static Window
win_get_leader_raw(session_t *ps, win *w, int recursions) {
  // Rebuild the cache if needed
  if (!w->cache_leader && (w->client_win || w->leader)) {
    // Leader defaults to client window
    if (!(w->cache_leader = w->leader))
      w->cache_leader = w->client_win;

    // If the leader of this window isn't itself, look for its ancestors
    if (w->cache_leader && w->cache_leader != w->client_win) {
      win *wp = find_toplevel(ps, w->cache_leader);
      if (wp) {
        // Dead loop?
        if (recursions > WIN_GET_LEADER_MAX_RECURSION)
          return None;

        w->cache_leader = win_get_leader_raw(ps, wp, recursions + 1);
      }
    }
  }

  return w->cache_leader;
}

/**
 * Get the value of a text property of a window.
 */
bool
wid_get_text_prop(session_t *ps, Window wid, Atom prop,
    char ***pstrlst, int *pnstr) {
  XTextProperty text_prop = { NULL, None, 0, 0 };

  if (!(XGetTextProperty(ps->dpy, wid, &text_prop, prop) && text_prop.value))
    return false;

  if (Success !=
      XmbTextPropertyToTextList(ps->dpy, &text_prop, pstrlst, pnstr)
      || !*pnstr) {
    *pnstr = 0;
    if (*pstrlst)
      XFreeStringList(*pstrlst);
    XFree(text_prop.value);
    return false;
  }

  XFree(text_prop.value);
  return true;
}

/**
 * Get the name of a window from window ID.
 */
static bool
wid_get_name(session_t *ps, Window wid, char **name) {
  XTextProperty text_prop = { NULL, None, 0, 0 };
  char **strlst = NULL;
  int nstr = 0;

  if (!(wid_get_text_prop(ps, wid, ps->atom_name_ewmh, &strlst, &nstr))) {
#ifdef DEBUG_WINDATA
    printf_dbgf("(%#010lx): _NET_WM_NAME unset, falling back to WM_NAME.\n", wid);
#endif

    if (!(XGetWMName(ps->dpy, wid, &text_prop) && text_prop.value)) {
      return false;
    }
    if (Success !=
        XmbTextPropertyToTextList(ps->dpy, &text_prop, &strlst, &nstr)
        || !nstr || !strlst) {
      if (strlst)
        XFreeStringList(strlst);
      XFree(text_prop.value);
      return false;
    }
    XFree(text_prop.value);
  }

  *name = mstrcpy(strlst[0]);

  XFreeStringList(strlst);

  return true;
}

/**
 * Get the role of a window from window ID.
 */
static bool
wid_get_role(session_t *ps, Window wid, char **role) {
  char **strlst = NULL;
  int nstr = 0;

  if (!wid_get_text_prop(ps, wid, ps->atom_role, &strlst, &nstr)) {
    return false;
  }

  *role = mstrcpy(strlst[0]);

  XFreeStringList(strlst);

  return true;
}

/**
 * Retrieve a string property of a window and update its <code>win</code>
 * structure.
 */
static int
win_get_prop_str(session_t *ps, win *w, char **tgt,
    bool (*func_wid_get_prop_str)(session_t *ps, Window wid, char **tgt)) {
  int ret = -1;
  char *prop_old = *tgt;

  // Can't do anything if there's no client window
  if (!w->client_win)
    return false;

  // Get the property
  ret = func_wid_get_prop_str(ps, w->client_win, tgt);

  // Return -1 if func_wid_get_prop_str() failed, 0 if the property
  // doesn't change, 1 if it changes
  if (!ret)
    ret = -1;
  else if (prop_old && !strcmp(*tgt, prop_old))
    ret = 0;
  else
    ret = 1;

  // Keep the old property if there's no new one
  if (*tgt != prop_old)
    free(prop_old);

  return ret;
}

/**
 * Retrieve the <code>WM_CLASS</code> of a window and update its
 * <code>win</code> structure.
 */
static bool
win_get_class(session_t *ps, win *w) {
  char **strlst = NULL;
  int nstr = 0;

  // Can't do anything if there's no client window
  if (!w->client_win)
    return false;

  // Free and reset old strings
  free(w->class_instance);
  free(w->class_general);
  w->class_instance = NULL;
  w->class_general = NULL;

  // Retrieve the property string list
  if (!wid_get_text_prop(ps, w->client_win, ps->atom_class, &strlst, &nstr))
    return false;

  // Copy the strings if successful
  w->class_instance = mstrcpy(strlst[0]);

  if (nstr > 1)
    w->class_general = mstrcpy(strlst[1]);

  XFreeStringList(strlst);

#ifdef DEBUG_WINDATA
  printf_dbgf("(%#010lx): client = %#010lx, "
      "instance = \"%s\", general = \"%s\"\n",
      w->id, w->client_win, w->class_instance, w->class_general);
#endif

  return true;
}

/**
 * Force a full-screen repaint.
 */
void
force_repaint(session_t *ps) {
  assert(ps->screen_reg);
  XserverRegion reg = None;
  if (ps->screen_reg && (reg = copy_region(ps, ps->screen_reg))) {
    ps->ev_received = true;
    add_damage(ps, reg);
  }
}

#ifdef CONFIG_DBUS
/** @name DBus hooks
 */
///@{

/**
 * Set w->shadow_force of a window.
 */
void
win_set_shadow_force(session_t *ps, win *w, switch_t val) {
  if (val != w->shadow_force) {
    w->shadow_force = val;
    win_determine_shadow(ps, w);
  }
}

/**
 * Set w->focused_force of a window.
 */
void
win_set_focused_force(session_t *ps, win *w, switch_t val) {
  if (val != w->focused_force) {
    w->focused_force = val;
    win_update_focused(ps, w);
  }
}

/**
 * Set w->invert_color_force of a window.
 */
void
win_set_invert_color_force(session_t *ps, win *w, switch_t val) {
  if (val != w->invert_color_force) {
    w->invert_color_force = val;
    win_determine_invert_color(ps, w);
  }
}
//!@}
#endif

#ifdef DEBUG_EVENTS
static int
ev_serial(XEvent *ev) {
  if ((ev->type & 0x7f) != KeymapNotify) {
    return ev->xany.serial;
  }
  return NextRequest(ev->xany.display);
}

static const char *
ev_name(session_t *ps, XEvent *ev) {
  static char buf[128];
  switch (ev->type & 0x7f) {
    CASESTRRET(FocusIn);
    CASESTRRET(FocusOut);
    CASESTRRET(CreateNotify);
    CASESTRRET(ConfigureNotify);
    CASESTRRET(DestroyNotify);
    CASESTRRET(MapNotify);
    CASESTRRET(UnmapNotify);
    CASESTRRET(ReparentNotify);
    CASESTRRET(CirculateNotify);
    CASESTRRET(Expose);
    CASESTRRET(PropertyNotify);
    CASESTRRET(ClientMessage);
    default:
      if (ev->type == ps->damage_event + XDamageNotify) {
        return "Damage";
      }

      if (ps->shape_exists && ev->type == ps->shape_event) {
        return "ShapeNotify";
      }

      sprintf(buf, "Event %d", ev->type);

      return buf;
  }
}

static Window
ev_window(session_t *ps, XEvent *ev) {
  switch (ev->type) {
    case FocusIn:
    case FocusOut:
      return ev->xfocus.window;
    case CreateNotify:
      return ev->xcreatewindow.window;
    case ConfigureNotify:
      return ev->xconfigure.window;
    case DestroyNotify:
      return ev->xdestroywindow.window;
    case MapNotify:
      return ev->xmap.window;
    case UnmapNotify:
      return ev->xunmap.window;
    case ReparentNotify:
      return ev->xreparent.window;
    case CirculateNotify:
      return ev->xcirculate.window;
    case Expose:
      return ev->xexpose.window;
    case PropertyNotify:
      return ev->xproperty.window;
    case ClientMessage:
      return ev->xclient.window;
    default:
      if (ev->type == ps->damage_event + XDamageNotify) {
        return ((XDamageNotifyEvent *)ev)->drawable;
      }

      if (ps->shape_exists && ev->type == ps->shape_event) {
        return ((XShapeEvent *) ev)->window;
      }

      return 0;
  }
}

static inline const char *
ev_focus_mode_name(XFocusChangeEvent* ev) {
  switch (ev->mode) {
    CASESTRRET(NotifyNormal);
    CASESTRRET(NotifyWhileGrabbed);
    CASESTRRET(NotifyGrab);
    CASESTRRET(NotifyUngrab);
  }

  return "Unknown";
}

static inline const char *
ev_focus_detail_name(XFocusChangeEvent* ev) {
  switch (ev->detail) {
    CASESTRRET(NotifyAncestor);
    CASESTRRET(NotifyVirtual);
    CASESTRRET(NotifyInferior);
    CASESTRRET(NotifyNonlinear);
    CASESTRRET(NotifyNonlinearVirtual);
    CASESTRRET(NotifyPointer);
    CASESTRRET(NotifyPointerRoot);
    CASESTRRET(NotifyDetailNone);
  }

  return "Unknown";
}

static inline void
ev_focus_report(XFocusChangeEvent* ev) {
  printf("  { mode: %s, detail: %s }\n", ev_focus_mode_name(ev),
      ev_focus_detail_name(ev));
}

#endif

// === Events ===

/**
 * Determine whether we should respond to a <code>FocusIn/Out</code>
 * event.
 */
inline static bool
ev_focus_accept(XFocusChangeEvent *ev) {
  return ev->detail == NotifyNonlinear
    || ev->detail == NotifyNonlinearVirtual;
}

inline static void
ev_focus_in(session_t *ps, XFocusChangeEvent *ev) {
#ifdef DEBUG_EVENTS
  ev_focus_report(ev);
#endif

  if (!ev_focus_accept(ev))
    return;

  win *w = find_win(ps, ev->window);

  // To deal with events sent from windows just destroyed
  if (!w) return;

  win_set_focused(ps, w, true);
}

inline static void
ev_focus_out(session_t *ps, XFocusChangeEvent *ev) {
#ifdef DEBUG_EVENTS
  ev_focus_report(ev);
#endif

  if (!ev_focus_accept(ev))
    return;

  win *w = find_win(ps, ev->window);

  // To deal with events sent from windows just destroyed
  if (!w) return;

  win_set_focused(ps, w, false);
}

inline static void
ev_create_notify(session_t *ps, XCreateWindowEvent *ev) {
  assert(ev->parent == ps->root);
  add_win(ps, ev->window, 0);
}

inline static void
ev_configure_notify(session_t *ps, XConfigureEvent *ev) {
#ifdef DEBUG_EVENTS
  printf("  { send_event: %d, "
         " above: %#010lx, "
         " override_redirect: %d }\n",
         ev->send_event, ev->above, ev->override_redirect);
#endif
  configure_win(ps, ev);
}

inline static void
ev_destroy_notify(session_t *ps, XDestroyWindowEvent *ev) {
  destroy_win(ps, ev->window);
}

inline static void
ev_map_notify(session_t *ps, XMapEvent *ev) {
  map_win(ps, ev->window);
}

inline static void
ev_unmap_notify(session_t *ps, XUnmapEvent *ev) {
  unmap_win(ps, ev->window);
}

inline static void
ev_reparent_notify(session_t *ps, XReparentEvent *ev) {
  if (ev->parent == ps->root) {
    add_win(ps, ev->window, 0);
  } else {
    destroy_win(ps, ev->window);

    // Reset event mask in case something wrong happens
    XSelectInput(ps->dpy, ev->window,
        determine_evmask(ps, ev->window, WIN_EVMODE_UNKNOWN));

    // Check if the window is an undetected client window
    // Firstly, check if it's a known client window
    if (!find_toplevel(ps, ev->window)) {
      // If not, look for its frame window
      win *w_top = find_toplevel2(ps, ev->parent);
      // If found, and the client window has not been determined, or its
      // frame may not have a correct client, continue
      if (w_top && (!w_top->client_win
            || w_top->client_win == w_top->id)) {
        // If it has WM_STATE, mark it the client window
        if (wid_has_prop(ps, ev->window, ps->atom_client)) {
          w_top->wmwin = false;
          win_unmark_client(ps, w_top);
          win_mark_client(ps, w_top, ev->window);
        }
        // Otherwise, watch for WM_STATE on it
        else {
          XSelectInput(ps->dpy, ev->window,
              determine_evmask(ps, ev->window, WIN_EVMODE_UNKNOWN)
              | PropertyChangeMask);
        }
      }
    }
  }
}

inline static void
ev_circulate_notify(session_t *ps, XCirculateEvent *ev) {
  circulate_win(ps, ev);
}

inline static void
ev_expose(session_t *ps, XExposeEvent *ev) {
  if (ev->window == ps->root || (ps->overlay && ev->window == ps->overlay)) {
    int more = ev->count + 1;
    if (ps->n_expose == ps->size_expose) {
      if (ps->expose_rects) {
        ps->expose_rects = realloc(ps->expose_rects,
          (ps->size_expose + more) * sizeof(XRectangle));
        ps->size_expose += more;
      } else {
        ps->expose_rects = malloc(more * sizeof(XRectangle));
        ps->size_expose = more;
      }
    }

    ps->expose_rects[ps->n_expose].x = ev->x;
    ps->expose_rects[ps->n_expose].y = ev->y;
    ps->expose_rects[ps->n_expose].width = ev->width;
    ps->expose_rects[ps->n_expose].height = ev->height;
    ps->n_expose++;

    if (ev->count == 0) {
      expose_root(ps, ps->expose_rects, ps->n_expose);
      ps->n_expose = 0;
    }
  }
}

/**
 * Update current active window based on EWMH _NET_ACTIVE_WIN.
 *
 * Does not change anything if we fail to get the attribute or the window
 * returned could not be found.
 */
static void
update_ewmh_active_win(session_t *ps) {
  // Search for the window
  Window wid =
    wid_get_prop_window(ps, ps->root, ps->atom_ewmh_active_win);
  win *w = NULL;

  if (wid && !(w = find_toplevel(ps, wid)))
    if (!(w = find_win(ps, wid)))
      w = find_toplevel2(ps, wid);

  // Mark the window focused
  if (w) {
    if (ps->active_win && w != ps->active_win)
      win_set_focused(ps, ps->active_win, false);
    ps->active_win = w;
    win_set_focused(ps, w, true);
  }
}

inline static void
ev_property_notify(session_t *ps, XPropertyEvent *ev) {
  if (ps->root == ev->window) {
    if (ps->o.track_focus && ps->o.use_ewmh_active_win
        && ps->atom_ewmh_active_win == ev->atom) {
      update_ewmh_active_win(ps);
    }
    else {
      // Destroy the root "image" if the wallpaper probably changed
      for (int p = 0; background_props_str[p]; p++) {
        if (ev->atom == get_atom(ps, background_props_str[p])) {
          root_damaged(ps);
          break;
        }
      }
    }

    // Unconcerned about any other proprties on root window
    return;
  }

  // If WM_STATE changes
  if (ev->atom == ps->atom_client) {
    // Check whether it could be a client window
    if (!find_toplevel(ps, ev->window)) {
      // Reset event mask anyway
      XSelectInput(ps->dpy, ev->window,
          determine_evmask(ps, ev->window, WIN_EVMODE_UNKNOWN));

      win *w_top = find_toplevel2(ps, ev->window);
      // Initialize client_win as early as possible
      if (w_top && (!w_top->client_win || w_top->client_win == w_top->id)
          && wid_has_prop(ps, ev->window, ps->atom_client)) {
        w_top->wmwin = false;
        win_unmark_client(ps, w_top);
        win_mark_client(ps, w_top, ev->window);
      }
    }
  }

  // If _NET_WM_OPACITY changes
  if (ev->atom == ps->atom_opacity) {
    win *w = NULL;
    if ((w = find_win(ps, ev->window)))
      w->opacity_prop = wid_get_opacity_prop(ps, w->id, OPAQUE);
    else if (ps->o.detect_client_opacity
        && (w = find_toplevel(ps, ev->window)))
      w->opacity_prop_client = wid_get_opacity_prop(ps, w->client_win,
            OPAQUE);
    if (w) {
      w->flags |= WFLAG_OPCT_CHANGE;
    }
  }

  // If frame extents property changes
  if (ps->o.frame_opacity && ev->atom == ps->atom_frame_extents) {
    win *w = find_toplevel(ps, ev->window);
    if (w) {
      get_frame_extents(ps, w, ev->window);
      // If frame extents change, the window needs repaint
      add_damage_win(ps, w);
    }
  }

  // If name changes
  if (ps->o.track_wdata
      && (ps->atom_name == ev->atom || ps->atom_name_ewmh == ev->atom)) {
    win *w = find_toplevel(ps, ev->window);
    if (w && 1 == win_get_name(ps, w)) {
      win_on_wdata_change(ps, w);
    }
  }

  // If class changes
  if (ps->o.track_wdata && ps->atom_class == ev->atom) {
    win *w = find_toplevel(ps, ev->window);
    if (w) {
      win_get_class(ps, w);
      win_on_wdata_change(ps, w);
    }
  }

  // If role changes
  if (ps->o.track_wdata && ps->atom_role == ev->atom) {
    win *w = find_toplevel(ps, ev->window);
    if (w && 1 == win_get_role(ps, w)) {
      win_on_wdata_change(ps, w);
    }
  }

  // If _COMPTON_SHADOW changes
  if (ps->o.respect_prop_shadow && ps->atom_compton_shadow == ev->atom) {
    win *w = find_win(ps, ev->window);
    if (w)
      win_update_prop_shadow(ps, w);
  }

  // If a leader property changes
  if ((ps->o.detect_transient && ps->atom_transient == ev->atom)
      || (ps->o.detect_client_leader && ps->atom_client_leader == ev->atom)) {
    win *w = find_toplevel(ps, ev->window);
    if (w) {
      win_update_leader(ps, w);
    }
  }

  // Check for other atoms we are tracking
  for (latom_t *platom = ps->track_atom_lst; platom; platom = platom->next) {
    if (platom->atom == ev->atom) {
      win *w = find_win(ps, ev->window);
      if (!w)
        w = find_toplevel(ps, ev->window);
      if (w)
        win_on_wdata_change(ps, w);
      break;
    }
  }
}

inline static void
ev_damage_notify(session_t *ps, XDamageNotifyEvent *ev) {
  damage_win(ps, ev);
}

inline static void
ev_shape_notify(session_t *ps, XShapeEvent *ev) {
  win *w = find_win(ps, ev->window);
  if (!w || IsUnmapped == w->a.map_state) return;

  /*
   * Empty border_size may indicated an
   * unmapped/destroyed window, in which case
   * seemingly BadRegion errors would be triggered
   * if we attempt to rebuild border_size
   */
  if (w->border_size) {
    // Mark the old border_size as damaged
    add_damage(ps, w->border_size);

    w->border_size = border_size(ps, w, true);

    // Mark the new border_size as damaged
    add_damage(ps, copy_region(ps, w->border_size));
  }

  // Redo bounding shape detection and rounded corner detection
  win_update_shape(ps, w);

  update_reg_ignore_expire(ps, w);
}

/**
 * Handle ScreenChangeNotify events from X RandR extension.
 */
static void
ev_screen_change_notify(session_t *ps,
    XRRScreenChangeNotifyEvent __attribute__((unused)) *ev) {
  if (!ps->o.refresh_rate) {
    update_refresh_rate(ps);
    if (!ps->refresh_rate) {
      fprintf(stderr, "ev_screen_change_notify(): Refresh rate detection "
          "failed, software VSync disabled.");
      ps->o.vsync = VSYNC_NONE;
    }
  }
}

#if defined(DEBUG_EVENTS) || defined(DEBUG_RESTACK)
/**
 * Get a window's name from window ID.
 */
static bool
ev_window_name(session_t *ps, Window wid, char **name) {
  bool to_free = false;

  *name = "";
  if (wid) {
    *name = "(Failed to get title)";
    if (ps->root == wid)
      *name = "(Root window)";
    else if (ps->overlay == wid)
      *name = "(Overlay)";
    else {
      win *w = find_win(ps, wid);
      if (!w)
        w = find_toplevel(ps, wid);

      if (w && w->name)
        *name = w->name;
      else if (!(w && w->client_win
            && (to_free = wid_get_name(ps, w->client_win, name))))
          to_free = wid_get_name(ps, wid, name);
    }
  }

  return to_free;
}
#endif

static void
ev_handle(session_t *ps, XEvent *ev) {
  if ((ev->type & 0x7f) != KeymapNotify) {
    discard_ignore(ps, ev->xany.serial);
  }

#ifdef DEBUG_EVENTS
  if (ev->type != ps->damage_event + XDamageNotify) {
    Window wid = ev_window(ps, ev);
    char *window_name = NULL;
    bool to_free = false;

    to_free = ev_window_name(ps, wid, &window_name);

    print_timestamp(ps);
    printf("event %10.10s serial %#010x window %#010lx \"%s\"\n",
      ev_name(ps, ev), ev_serial(ev), wid, window_name);

    if (to_free) {
      XFree(window_name);
      window_name = NULL;
    }
  }

#endif

  switch (ev->type) {
    case FocusIn:
      ev_focus_in(ps, (XFocusChangeEvent *)ev);
      break;
    case FocusOut:
      ev_focus_out(ps, (XFocusChangeEvent *)ev);
      break;
    case CreateNotify:
      ev_create_notify(ps, (XCreateWindowEvent *)ev);
      break;
    case ConfigureNotify:
      ev_configure_notify(ps, (XConfigureEvent *)ev);
      break;
    case DestroyNotify:
      ev_destroy_notify(ps, (XDestroyWindowEvent *)ev);
      break;
    case MapNotify:
      ev_map_notify(ps, (XMapEvent *)ev);
      break;
    case UnmapNotify:
      ev_unmap_notify(ps, (XUnmapEvent *)ev);
      break;
    case ReparentNotify:
      ev_reparent_notify(ps, (XReparentEvent *)ev);
      break;
    case CirculateNotify:
      ev_circulate_notify(ps, (XCirculateEvent *)ev);
      break;
    case Expose:
      ev_expose(ps, (XExposeEvent *)ev);
      break;
    case PropertyNotify:
      ev_property_notify(ps, (XPropertyEvent *)ev);
      break;
    default:
      if (ps->shape_exists && ev->type == ps->shape_event) {
        ev_shape_notify(ps, (XShapeEvent *) ev);
        break;
      }
      if (ps->randr_exists && ev->type == (ps->randr_event + RRScreenChangeNotify)) {
        ev_screen_change_notify(ps, (XRRScreenChangeNotifyEvent *) ev);
        break;
      }
      if (ev->type == ps->damage_event + XDamageNotify) {
        ev_damage_notify(ps, (XDamageNotifyEvent *)ev);
      }
      break;
  }
}

// === Main ===

/**
 * Print usage text and exit.
 */
static void
usage(void) {
  const static char *usage_text =
    "compton (" COMPTON_VERSION ")\n"
    "usage: compton [options]\n"
    "Options:\n"
    "\n"
    "-d display\n"
    "  Which display should be managed.\n"
    "-r radius\n"
    "  The blur radius for shadows. (default 12)\n"
    "-o opacity\n"
    "  The translucency for shadows. (default .75)\n"
    "-l left-offset\n"
    "  The left offset for shadows. (default -15)\n"
    "-t top-offset\n"
    "  The top offset for shadows. (default -15)\n"
    "-I fade-in-step\n"
    "  Opacity change between steps while fading in. (default 0.028)\n"
    "-O fade-out-step\n"
    "  Opacity change between steps while fading out. (default 0.03)\n"
    "-D fade-delta-time\n"
    "  The time between steps in a fade in milliseconds. (default 10)\n"
    "-m opacity\n"
    "  The opacity for menus. (default 1.0)\n"
    "-c\n"
    "  Enabled client-side shadows on windows.\n"
    "-C\n"
    "  Avoid drawing shadows on dock/panel windows.\n"
    "-z\n"
    "  Zero the part of the shadow's mask behind the window (experimental).\n"
    "-f\n"
    "  Fade windows in/out when opening/closing and when opacity\n"
    "  changes, unless --no-fading-openclose is used.\n"
    "-F\n"
    "  Equals -f. Deprecated.\n"
    "-i opacity\n"
    "  Opacity of inactive windows. (0.1 - 1.0)\n"
    "-e opacity\n"
    "  Opacity of window titlebars and borders. (0.1 - 1.0)\n"
    "-G\n"
    "  Don't draw shadows on DND windows\n"
    "-b\n"
    "  Daemonize process.\n"
    "-S\n"
    "  Enable synchronous operation (for debugging).\n"
    "--config path\n"
    "  Look for configuration file at the path.\n"
    "--shadow-red value\n"
    "  Red color value of shadow (0.0 - 1.0, defaults to 0).\n"
    "--shadow-green value\n"
    "  Green color value of shadow (0.0 - 1.0, defaults to 0).\n"
    "--shadow-blue value\n"
    "  Blue color value of shadow (0.0 - 1.0, defaults to 0).\n"
    "--inactive-opacity-override\n"
    "  Inactive opacity set by -i overrides value of _NET_WM_OPACITY.\n"
    "--inactive-dim value\n"
    "  Dim inactive windows. (0.0 - 1.0, defaults to 0)\n"
    "--mark-wmwin-focused\n"
    "  Try to detect WM windows and mark them as active.\n"
    "--shadow-exclude condition\n"
    "  Exclude conditions for shadows.\n"
    "--mark-ovredir-focused\n"
    "  Mark windows that have no WM frame as active.\n"
    "--no-fading-openclose\n"
    "  Do not fade on window open/close.\n"
    "--shadow-ignore-shaped\n"
    "  Do not paint shadows on shaped windows.\n"
    "--detect-rounded-corners\n"
    "  Try to detect windows with rounded corners and don't consider\n"
    "  them shaped windows.\n"
    "--detect-client-opacity\n"
    "  Detect _NET_WM_OPACITY on client windows, useful for window\n"
    "  managers not passing _NET_WM_OPACITY of client windows to frame\n"
    "  windows.\n"
    "--refresh-rate val\n"
    "  Specify refresh rate of the screen. If not specified or 0, compton\n"
    "  will try detecting this with X RandR extension.\n"
    "--vsync vsync-method\n"
    "  Set VSync method. There are up to 2 VSync methods currently available\n"
    "  depending on your compile time settings:\n"
    "    none = No VSync\n"
#ifdef CONFIG_VSYNC_DRM
    "    drm = VSync with DRM_IOCTL_WAIT_VBLANK. May only work on some\n"
    "      drivers. Experimental.\n"
#endif
#ifdef CONFIG_VSYNC_OPENGL
    "    opengl = Try to VSync with SGI_swap_control OpenGL extension. Only\n"
    "      work on some drivers. Experimental.\n"
#endif
    "--alpha-step val\n"
    "  Step for pregenerating alpha pictures. 0.01 - 1.0. Defaults to\n"
    "  0.03.\n"
    "--dbe\n"
    "  Enable DBE painting mode, intended to use with VSync to\n"
    "  (hopefully) eliminate tearing.\n"
    "--paint-on-overlay\n"
    "  Painting on X Composite overlay window.\n"
    "--sw-opti\n"
    "  Limit compton to repaint at most once every 1 / refresh_rate\n"
    "  second to boost performance. Experimental.\n"
    "--vsync-aggressive\n"
    "  Attempt to send painting request before VBlank and do XFlush()\n"
    "  during VBlank. This switch may be lifted out at any moment.\n"
    "--use-ewmh-active-win\n"
    "  Use _NET_WM_ACTIVE_WINDOW on the root window to determine which\n"
    "  window is focused instead of using FocusIn/Out events.\n"
    "--respect-prop-shadow\n"
    "  Respect _COMPTON_SHADOW. This a prototype-level feature, which\n"
    "  you must not rely on.\n"
    "--unredir-if-possible\n"
    "  Unredirect all windows if a full-screen opaque window is\n"
    "  detected, to maximize performance for full-screen windows.\n"
    "  Experimental.\n"
    "--focus-exclude condition\n"
    "  Specify a list of conditions of windows that should always be\n"
    "  considered focused.\n"
    "--inactive-dim-fixed\n"
    "  Use fixed inactive dim value.\n"
    "--detect-transient\n"
    "  Use WM_TRANSIENT_FOR to group windows, and consider windows in\n"
    "  the same group focused at the same time.\n"
    "--detect-client-leader\n"
    "  Use WM_CLIENT_LEADER to group windows, and consider windows in\n"
    "  the same group focused at the same time. WM_TRANSIENT_FOR has\n"
    "  higher priority if --detect-transient is enabled, too.\n"
    "--blur-background\n"
    "  Blur background of semi-transparent / ARGB windows. Bad in\n"
    "  performance. The switch name may change without prior\n"
    "  notifications.\n"
    "--blur-background-frame\n"
    "  Blur background of windows when the window frame is not opaque.\n"
    "  Implies --blur-background. Bad in performance. The switch name\n"
    "  may change.\n"
    "--blur-background-fixed\n"
    "  Use fixed blur strength instead of adjusting according to window\n"
    "  opacity.\n"
    "--invert-color-include condition\n"
    "  Specify a list of conditions of windows that should be painted with\n"
    "  inverted color. Resource-hogging, and is not well tested.\n"
#ifdef CONFIG_DBUS
    "--dbus\n"
    "  Enable remote control via D-Bus. See the D-BUS API section in the\n"
    "  man page for more details.\n"
#endif
    "\n"
    "Format of a condition:\n"
    "\n"
    "  condition = <target>:<type>[<flags>]:<pattern>\n"
    "\n"
    "  <target> is one of \"n\" (window name), \"i\" (window class\n"
    "  instance), \"g\" (window general class), and \"r\"\n"
    "  (window role).\n"
    "\n"
    "  <type> is one of \"e\" (exact match), \"a\" (match anywhere),\n"
    "  \"s\" (match from start), \"w\" (wildcard)"
#ifdef CONFIG_REGEX_PCRE
    " and \"p\" (PCRE\n"
    "  regular expressions)."
#endif
    "\n\n"
    "  <flags> could be a series of flags. Currently the only defined\n"
    "  flag is \"i\" (ignore case).\n"
    "\n"
    "  <pattern> is the actual pattern string.\n";
  fputs(usage_text , stderr);

  exit(1);
}

/**
 * Register a window as symbol, and initialize GLX context if wanted.
 */
static void
register_cm(session_t *ps, bool want_glxct) {
  Atom a;
  char *buf;

#ifdef CONFIG_VSYNC_OPENGL
  // Create a window with the wanted GLX visual
  if (want_glxct) {
    XVisualInfo *pvi = NULL;
    bool ret = false;
    // Get visual for the window
    int attribs[] = { GLX_RGBA, GLX_RED_SIZE, 1, GLX_GREEN_SIZE, 1, GLX_BLUE_SIZE, 1, None };
    pvi = glXChooseVisual(ps->dpy, ps->scr, attribs);

    if (!pvi) {
      fprintf(stderr, "register_cm(): Failed to choose visual required "
          "by fake OpenGL VSync window. OpenGL VSync turned off.\n");
    }
    else {
      // Create the window
      XSetWindowAttributes swa = {
        .colormap = XCreateColormap(ps->dpy, ps->root, pvi->visual, AllocNone),
        .border_pixel = 0,
      };

      pvi->screen = ps->scr;
      ps->reg_win = XCreateWindow(ps->dpy, ps->root, 0, 0, 1, 1, 0, pvi->depth,
          InputOutput, pvi->visual, CWBorderPixel | CWColormap, &swa);

      if (!ps->reg_win)
        fprintf(stderr, "register_cm(): Failed to create window required "
            "by fake OpenGL VSync. OpenGL VSync turned off.\n");
      else {
        // Get GLX context
        ps->glx_context = glXCreateContext(ps->dpy, pvi, None, GL_TRUE);
        if (!ps->glx_context) {
          fprintf(stderr, "register_cm(): Failed to get GLX context. "
              "OpenGL VSync turned off.\n");
          ps->o.vsync = VSYNC_NONE;
        }
        else {
          // Attach GLX context
          if (!(ret = glXMakeCurrent(ps->dpy, ps->reg_win, ps->glx_context)))
            fprintf(stderr, "register_cm(): Failed to attach GLX context."
              " OpenGL VSync turned off.\n");
        }
      }
    }
    if (pvi)
      XFree(pvi);

    if (!ret)
      ps->o.vsync = VSYNC_NONE;
  }
#endif

  if (!ps->reg_win)
    ps->reg_win = XCreateSimpleWindow(ps->dpy, ps->root, 0, 0, 1, 1, 0,
        None, None);

  Xutf8SetWMProperties(ps->dpy, ps->reg_win, "xcompmgr", "xcompmgr",
    NULL, 0, NULL, NULL, NULL);

  unsigned len = strlen(REGISTER_PROP) + 2;
  int s = ps->scr;

  while (s >= 10) {
    ++len;
    s /= 10;
  }

  buf = malloc(len);
  snprintf(buf, len, REGISTER_PROP"%d", ps->scr);

  a = get_atom(ps, buf);
  free(buf);

  XSetSelectionOwner(ps->dpy, a, ps->reg_win, 0);
}

/**
 * Reopen streams for logging.
 */
static bool
ostream_reopen(session_t *ps, const char *path) {
  if (!path)
    path = ps->o.logpath;
  if (!path)
    path = "/dev/null";

  bool success = freopen(path, "a", stdout);
  success = freopen(path, "a", stderr) && success;
  if (!success)
    printf_errfq(1, "(%s): freopen() failed.", path);

  return success;
}

/**
 * Fork program to background and disable all I/O streams.
 */
static bool
fork_after(session_t *ps) {
  if (getppid() == 1)
    return true;

  int pid = fork();

  if (-1 == pid) {
    printf_errf("(): fork() failed.");
    return false;
  }

  if (pid > 0) _exit(0);

  setsid();

  // Mainly to suppress the _FORTIFY_SOURCE warning
  bool success = freopen("/dev/null", "r", stdin);
  if (!success) {
    printf_errf("(): freopen() failed.");
    return false;
  }
  success = ostream_reopen(ps, NULL);

  return success;
}

#ifdef CONFIG_LIBCONFIG
/**
 * Get a file stream of the configuration file to read.
 *
 * Follows the XDG specification to search for the configuration file.
 */
static FILE *
open_config_file(char *cpath, char **ppath) {
  const static char *config_filename = "/compton.conf";
  const static char *config_filename_legacy = "/.compton.conf";
  const static char *config_home_suffix = "/.config";
  const static char *config_system_dir = "/etc/xdg";

  char *dir = NULL, *home = NULL;
  char *path = cpath;
  FILE *f = NULL;

  if (path) {
    f = fopen(path, "r");
    if (f && ppath)
      *ppath = path;
    else
      free(path);
    return f;
  }

  // Check user configuration file in $XDG_CONFIG_HOME firstly
  if (!((dir = getenv("XDG_CONFIG_HOME")) && strlen(dir))) {
    if (!((home = getenv("HOME")) && strlen(home)))
      return NULL;

    path = mstrjoin3(home, config_home_suffix, config_filename);
  }
  else
    path = mstrjoin(dir, config_filename);

  f = fopen(path, "r");

  if (f && ppath)
    *ppath = path;
  else
    free(path);
  if (f)
    return f;

  // Then check user configuration file in $HOME
  if ((home = getenv("HOME")) && strlen(home)) {
    path = mstrjoin(home, config_filename_legacy);
    f = fopen(path, "r");
    if (f && ppath)
      *ppath = path;
    else
      free(path);
    if (f)
      return f;
  }

  // Check system configuration file in $XDG_CONFIG_DIRS at last
  if ((dir = getenv("XDG_CONFIG_DIRS")) && strlen(dir)) {
    char *part = strtok(dir, ":");
    while (part) {
      path = mstrjoin(part, config_filename);
      f = fopen(path, "r");
      if (f && ppath)
        *ppath = path;
      else
        free(path);
      if (f)
        return f;
      part = strtok(NULL, ":");
    }
  }
  else {
    path = mstrjoin(config_system_dir, config_filename);
    f = fopen(path, "r");
    if (f && ppath)
      *ppath = path;
    else
      free(path);
    if (f)
      return f;
  }

  return NULL;
}

/**
 * Parse a VSync option argument.
 */
static inline void
parse_vsync(session_t *ps, const char *optarg) {
  vsync_t i;

  for (i = 0; i < (sizeof(VSYNC_STRS) / sizeof(VSYNC_STRS[0])); ++i)
    if (!strcasecmp(optarg, VSYNC_STRS[i])) {
      ps->o.vsync = i;
      break;
    }
  if ((sizeof(VSYNC_STRS) / sizeof(VSYNC_STRS[0])) == i) {
    printf_errfq(1, "(\"%s\"): Invalid --vsync argument.", optarg);
  }
}

/**
 * Parse a condition list in configuration file.
 */
static void
parse_cfg_condlst(const config_t *pcfg, wincond_t **pcondlst,
    const char *name) {
  config_setting_t *setting = config_lookup(pcfg, name);
  if (setting) {
    // Parse an array of options
    if (config_setting_is_array(setting)) {
      int i = config_setting_length(setting);
      while (i--) {
        condlst_add(pcondlst, config_setting_get_string_elem(setting, i));
      }
    }
    // Treat it as a single pattern if it's a string
    else if (CONFIG_TYPE_STRING == config_setting_type(setting)) {
      condlst_add(pcondlst, config_setting_get_string(setting));
    }
  }
}

/**
 * Parse a configuration file from default location.
 */
static void
parse_config(session_t *ps, char *cpath, struct options_tmp *pcfgtmp) {
  char *path = NULL;
  FILE *f;
  config_t cfg;
  int ival = 0;
  double dval = 0.0;
  const char *sval = NULL;

  f = open_config_file(cpath, &path);
  if (!f) {
    if (cpath)
      printf_errfq(1, "(): Failed to read the specified configuration file.");
    return;
  }

  config_init(&cfg);
#ifndef CONFIG_LIBCONFIG_LEGACY
  {
    // dirname() could modify the original string, thus we must pass a
    // copy
    char *path2 = mstrcpy(path);
    char *parent = dirname(path2);

    if (parent)
      config_set_include_dir(&cfg, parent);

    free(path2);
  }
#endif

  if (CONFIG_FALSE == config_read(&cfg, f)) {
    printf("Error when reading configuration file \"%s\", line %d: %s\n",
        path, config_error_line(&cfg), config_error_text(&cfg));
    config_destroy(&cfg);
    free(path);
    return;
  }
  config_set_auto_convert(&cfg, 1);

  free(path);

  // Get options from the configuration file. We don't do range checking
  // right now. It will be done later

  // -D (fade_delta)
  if (lcfg_lookup_int(&cfg, "fade-delta", &ival))
    ps->o.fade_delta = ival;
  // -I (fade_in_step)
  if (config_lookup_float(&cfg, "fade-in-step", &dval))
    ps->o.fade_in_step = normalize_d(dval) * OPAQUE;
  // -O (fade_out_step)
  if (config_lookup_float(&cfg, "fade-out-step", &dval))
    ps->o.fade_out_step = normalize_d(dval) * OPAQUE;
  // -r (shadow_radius)
  lcfg_lookup_int(&cfg, "shadow-radius", &ps->o.shadow_radius);
  // -o (shadow_opacity)
  config_lookup_float(&cfg, "shadow-opacity", &ps->o.shadow_opacity);
  // -l (shadow_offset_x)
  lcfg_lookup_int(&cfg, "shadow-offset-x", &ps->o.shadow_offset_x);
  // -t (shadow_offset_y)
  lcfg_lookup_int(&cfg, "shadow-offset-y", &ps->o.shadow_offset_y);
  // -i (inactive_opacity)
  if (config_lookup_float(&cfg, "inactive-opacity", &dval))
    ps->o.inactive_opacity = normalize_d(dval) * OPAQUE;
  // -e (frame_opacity)
  config_lookup_float(&cfg, "frame-opacity", &ps->o.frame_opacity);
  // -z (clear_shadow)
  lcfg_lookup_bool(&cfg, "clear-shadow", &ps->o.clear_shadow);
  // -c (shadow_enable)
  if (config_lookup_bool(&cfg, "shadow", &ival) && ival)
    wintype_arr_enable(ps->o.wintype_shadow);
  // -C (no_dock_shadow)
  lcfg_lookup_bool(&cfg, "no-dock-shadow", &pcfgtmp->no_dock_shadow);
  // -G (no_dnd_shadow)
  lcfg_lookup_bool(&cfg, "no-dnd-shadow", &pcfgtmp->no_dnd_shadow);
  // -m (menu_opacity)
  config_lookup_float(&cfg, "menu-opacity", &pcfgtmp->menu_opacity);
  // -f (fading_enable)
  if (config_lookup_bool(&cfg, "fading", &ival) && ival)
    wintype_arr_enable(ps->o.wintype_fade);
  // --no-fading-open-close
  lcfg_lookup_bool(&cfg, "no-fading-openclose", &ps->o.no_fading_openclose);
  // --shadow-red
  config_lookup_float(&cfg, "shadow-red", &ps->o.shadow_red);
  // --shadow-green
  config_lookup_float(&cfg, "shadow-green", &ps->o.shadow_green);
  // --shadow-blue
  config_lookup_float(&cfg, "shadow-blue", &ps->o.shadow_blue);
  // --inactive-opacity-override
  lcfg_lookup_bool(&cfg, "inactive-opacity-override",
      &ps->o.inactive_opacity_override);
  // --inactive-dim
  config_lookup_float(&cfg, "inactive-dim", &ps->o.inactive_dim);
  // --mark-wmwin-focused
  lcfg_lookup_bool(&cfg, "mark-wmwin-focused", &ps->o.mark_wmwin_focused);
  // --mark-ovredir-focused
  lcfg_lookup_bool(&cfg, "mark-ovredir-focused",
      &ps->o.mark_ovredir_focused);
  // --shadow-ignore-shaped
  lcfg_lookup_bool(&cfg, "shadow-ignore-shaped",
      &ps->o.shadow_ignore_shaped);
  // --detect-rounded-corners
  lcfg_lookup_bool(&cfg, "detect-rounded-corners",
      &ps->o.detect_rounded_corners);
  // --detect-client-opacity
  lcfg_lookup_bool(&cfg, "detect-client-opacity",
      &ps->o.detect_client_opacity);
  // --refresh-rate
  lcfg_lookup_int(&cfg, "refresh-rate", &ps->o.refresh_rate);
  // --vsync
  if (config_lookup_string(&cfg, "vsync", &sval))
    parse_vsync(ps, sval);
  // --alpha-step
  config_lookup_float(&cfg, "alpha-step", &ps->o.alpha_step);
  // --dbe
  lcfg_lookup_bool(&cfg, "dbe", &ps->o.dbe);
  // --paint-on-overlay
  lcfg_lookup_bool(&cfg, "paint-on-overlay", &ps->o.paint_on_overlay);
  // --sw-opti
  lcfg_lookup_bool(&cfg, "sw-opti", &ps->o.sw_opti);
  // --use-ewmh-active-win
  lcfg_lookup_bool(&cfg, "use-ewmh-active-win",
      &ps->o.use_ewmh_active_win);
  // --unredir-if-possible
  lcfg_lookup_bool(&cfg, "unredir-if-possible",
      &ps->o.unredir_if_possible);
  // --inactive-dim-fixed
  lcfg_lookup_bool(&cfg, "inactive-dim-fixed", &ps->o.inactive_dim_fixed);
  // --detect-transient
  lcfg_lookup_bool(&cfg, "detect-transient", &ps->o.detect_transient);
  // --detect-client-leader
  lcfg_lookup_bool(&cfg, "detect-client-leader",
      &ps->o.detect_client_leader);
  // --shadow-exclude
  parse_cfg_condlst(&cfg, &ps->o.shadow_blacklist, "shadow-exclude");
  // --focus-exclude
  parse_cfg_condlst(&cfg, &ps->o.focus_blacklist, "focus-exclude");
  // --invert-color-include
  parse_cfg_condlst(&cfg, &ps->o.invert_color_list, "invert-color-include");
  // --blur-background
  lcfg_lookup_bool(&cfg, "blur-background", &ps->o.blur_background);
  // --blur-background-frame
  lcfg_lookup_bool(&cfg, "blur-background-frame",
      &ps->o.blur_background_frame);
  // --blur-background-fixed
  lcfg_lookup_bool(&cfg, "blur-background-fixed",
      &ps->o.blur_background_fixed);
  // Wintype settings
  {
    wintype_t i;

    for (i = 0; i < NUM_WINTYPES; ++i) {
      char *str = mstrjoin("wintypes.", WINTYPES[i]);
      config_setting_t *setting = config_lookup(&cfg, str);
      free(str);
      if (setting) {
        if (config_setting_lookup_bool(setting, "shadow", &ival))
          ps->o.wintype_shadow[i] = (bool) ival;
        if (config_setting_lookup_bool(setting, "fade", &ival))
          ps->o.wintype_fade[i] = (bool) ival;
        if (config_setting_lookup_bool(setting, "focus", &ival))
          ps->o.wintype_focus[i] = (bool) ival;
        config_setting_lookup_float(setting, "opacity",
            &ps->o.wintype_opacity[i]);
      }
    }
  }

  config_destroy(&cfg);
}
#endif

/**
 * Process arguments and configuration files.
 */
static void
get_cfg(session_t *ps, int argc, char *const *argv) {
  const static char *shortopts = "D:I:O:d:r:o:m:l:t:i:e:hscnfFCaSzGb";
  const static struct option longopts[] = {
    { "help", no_argument, NULL, 'h' },
    { "config", required_argument, NULL, 256 },
    { "shadow-red", required_argument, NULL, 257 },
    { "shadow-green", required_argument, NULL, 258 },
    { "shadow-blue", required_argument, NULL, 259 },
    { "inactive-opacity-override", no_argument, NULL, 260 },
    { "inactive-dim", required_argument, NULL, 261 },
    { "mark-wmwin-focused", no_argument, NULL, 262 },
    { "shadow-exclude", required_argument, NULL, 263 },
    { "mark-ovredir-focused", no_argument, NULL, 264 },
    { "no-fading-openclose", no_argument, NULL, 265 },
    { "shadow-ignore-shaped", no_argument, NULL, 266 },
    { "detect-rounded-corners", no_argument, NULL, 267 },
    { "detect-client-opacity", no_argument, NULL, 268 },
    { "refresh-rate", required_argument, NULL, 269 },
    { "vsync", required_argument, NULL, 270 },
    { "alpha-step", required_argument, NULL, 271 },
    { "dbe", no_argument, NULL, 272 },
    { "paint-on-overlay", no_argument, NULL, 273 },
    { "sw-opti", no_argument, NULL, 274 },
    { "vsync-aggressive", no_argument, NULL, 275 },
    { "use-ewmh-active-win", no_argument, NULL, 276 },
    { "respect-prop-shadow", no_argument, NULL, 277 },
    { "unredir-if-possible", no_argument, NULL, 278 },
    { "focus-exclude", required_argument, NULL, 279 },
    { "inactive-dim-fixed", no_argument, NULL, 280 },
    { "detect-transient", no_argument, NULL, 281 },
    { "detect-client-leader", no_argument, NULL, 282 },
    { "blur-background", no_argument, NULL, 283 },
    { "blur-background-frame", no_argument, NULL, 284 },
    { "blur-background-fixed", no_argument, NULL, 285 },
    { "dbus", no_argument, NULL, 286 },
    { "logpath", required_argument, NULL, 287 },
    { "invert-color-include", required_argument, NULL, 288 },
    // Must terminate with a NULL entry
    { NULL, 0, NULL, 0 },
  };

  struct options_tmp cfgtmp = {
    .no_dock_shadow = false,
    .no_dnd_shadow = false,
    .menu_opacity = 1.0,
  };
  bool shadow_enable = false, fading_enable = false;
  int o, longopt_idx, i;
  char *config_file = NULL;
  char *lc_numeric_old = mstrcpy(setlocale(LC_NUMERIC, NULL));

  for (i = 0; i < NUM_WINTYPES; ++i) {
    ps->o.wintype_fade[i] = false;
    ps->o.wintype_shadow[i] = false;
    ps->o.wintype_opacity[i] = 1.0;
  }

  // Pre-parse the commandline arguments to check for --config and invalid
  // switches
  // Must reset optind to 0 here in case we reread the commandline
  // arguments
  optind = 1;
  while (-1 !=
      (o = getopt_long(argc, argv, shortopts, longopts, &longopt_idx))) {
    if (256 == o)
      config_file = mstrcpy(optarg);
    else if ('?' == o || ':' == o)
      usage();
  }

#ifdef CONFIG_LIBCONFIG
  parse_config(ps, config_file, &cfgtmp);
#endif

  // Parse commandline arguments. Range checking will be done later.

  // Enforce LC_NUMERIC locale "C" here to make sure dots are recognized
  // instead of commas in atof().
  setlocale(LC_NUMERIC, "C");

  optind = 1;
  while (-1 !=
      (o = getopt_long(argc, argv, shortopts, longopts, &longopt_idx))) {
    switch (o) {
      // Short options
      case 'h':
        usage();
        break;
      case 'd':
        ps->o.display = mstrcpy(optarg);
        break;
      case 'D':
        ps->o.fade_delta = atoi(optarg);
        break;
      case 'I':
        ps->o.fade_in_step = normalize_d(atof(optarg)) * OPAQUE;
        break;
      case 'O':
        ps->o.fade_out_step = normalize_d(atof(optarg)) * OPAQUE;
        break;
      case 'c':
        shadow_enable = true;
        break;
      case 'C':
        cfgtmp.no_dock_shadow = true;
        break;
      case 'G':
        cfgtmp.no_dnd_shadow = true;
        break;
      case 'm':
        cfgtmp.menu_opacity = atof(optarg);
        break;
      case 'f':
      case 'F':
        fading_enable = true;
        break;
      case 'S':
        ps->o.synchronize = true;
        break;
      case 'r':
        ps->o.shadow_radius = atoi(optarg);
        break;
      case 'o':
        ps->o.shadow_opacity = atof(optarg);
        break;
      case 'l':
        ps->o.shadow_offset_x = atoi(optarg);
        break;
      case 't':
        ps->o.shadow_offset_y = atoi(optarg);
        break;
      case 'i':
        ps->o.inactive_opacity = (normalize_d(atof(optarg)) * OPAQUE);
        break;
      case 'e':
        ps->o.frame_opacity = atof(optarg);
        break;
      case 'z':
        ps->o.clear_shadow = true;
        break;
      case 'n':
      case 'a':
      case 's':
        printf_errfq(1, "(): -n, -a, and -s have been removed.");
        break;
      case 'b':
        ps->o.fork_after_register = true;
        break;
      // Long options
      case 256:
        // --config
        break;
      case 257:
        // --shadow-red
        ps->o.shadow_red = atof(optarg);
        break;
      case 258:
        // --shadow-green
        ps->o.shadow_green = atof(optarg);
        break;
      case 259:
        // --shadow-blue
        ps->o.shadow_blue = atof(optarg);
        break;
      case 260:
        // --inactive-opacity-override
        ps->o.inactive_opacity_override = true;
        break;
      case 261:
        // --inactive-dim
        ps->o.inactive_dim = atof(optarg);
        break;
      case 262:
        // --mark-wmwin-focused
        ps->o.mark_wmwin_focused = true;
        break;
      case 263:
        // --shadow-exclude
        condlst_add(&ps->o.shadow_blacklist, optarg);
        break;
      case 264:
        // --mark-ovredir-focused
        ps->o.mark_ovredir_focused = true;
        break;
      case 265:
        // --no-fading-openclose
        ps->o.no_fading_openclose = true;
        break;
      case 266:
        // --shadow-ignore-shaped
        ps->o.shadow_ignore_shaped = true;
        break;
      case 267:
        // --detect-rounded-corners
        ps->o.detect_rounded_corners = true;
        break;
      case 268:
        // --detect-client-opacity
        ps->o.detect_client_opacity = true;
        break;
      case 269:
        // --refresh-rate
        ps->o.refresh_rate = atoi(optarg);
        break;
      case 270:
        // --vsync
        parse_vsync(ps, optarg);
        break;
      case 271:
        // --alpha-step
        ps->o.alpha_step = atof(optarg);
        break;
      case 272:
        // --dbe
        ps->o.dbe = true;
        break;
      case 273:
        // --paint-on-overlay
        ps->o.paint_on_overlay = true;
        break;
      case 274:
        // --sw-opti
        ps->o.sw_opti = true;
        break;
      case 275:
        // --vsync-aggressive
        ps->o.vsync_aggressive = true;
        break;
      case 276:
        // --use-ewmh-active-win
        ps->o.use_ewmh_active_win = true;
        break;
      case 277:
        // --respect-prop-shadow
        ps->o.respect_prop_shadow = true;
        break;
      case 278:
        // --unredir-if-possible
        ps->o.unredir_if_possible = true;
        break;
      case 279:
        // --focus-exclude
        condlst_add(&ps->o.focus_blacklist, optarg);
        break;
      case 280:
        // --inactive-dim-fixed
        ps->o.inactive_dim_fixed = true;
        break;
      case 281:
        // --detect-transient
        ps->o.detect_transient = true;
        break;
      case 282:
        // --detect-client-leader
        ps->o.detect_client_leader = true;
        break;
      case 283:
        // --blur-background
        ps->o.blur_background = true;
        break;
      case 284:
        // --blur-background-frame
        ps->o.blur_background_frame = true;
        break;
      case 285:
        // --blur-background-fixed
        ps->o.blur_background_fixed = true;
        break;
      case 286:
        // --dbus
        ps->o.dbus = true;
        break;
      case 287:
        // --logpath
        ps->o.logpath = mstrcpy(optarg);
        break;
      case 288:
        // --invert-color-include
        condlst_add(&ps->o.invert_color_list, optarg);
        break;
      default:
        usage();
        break;
    }
  }

  // Restore LC_NUMERIC
  setlocale(LC_NUMERIC, lc_numeric_old);
  free(lc_numeric_old);

  // Range checking and option assignments
  ps->o.fade_delta = max_i(ps->o.fade_delta, 1);
  ps->o.shadow_radius = max_i(ps->o.shadow_radius, 1);
  ps->o.shadow_red = normalize_d(ps->o.shadow_red);
  ps->o.shadow_green = normalize_d(ps->o.shadow_green);
  ps->o.shadow_blue = normalize_d(ps->o.shadow_blue);
  ps->o.inactive_dim = normalize_d(ps->o.inactive_dim);
  ps->o.frame_opacity = normalize_d(ps->o.frame_opacity);
  ps->o.shadow_opacity = normalize_d(ps->o.shadow_opacity);
  cfgtmp.menu_opacity = normalize_d(cfgtmp.menu_opacity);
  ps->o.refresh_rate = normalize_i_range(ps->o.refresh_rate, 0, 300);
  ps->o.alpha_step = normalize_d_range(ps->o.alpha_step, 0.01, 1.0);
  if (OPAQUE == ps->o.inactive_opacity) {
    ps->o.inactive_opacity = 0;
  }
  if (shadow_enable)
    wintype_arr_enable(ps->o.wintype_shadow);
  ps->o.wintype_shadow[WINTYPE_DESKTOP] = false;
  if (cfgtmp.no_dock_shadow)
    ps->o.wintype_shadow[WINTYPE_DOCK] = false;
  if (cfgtmp.no_dnd_shadow)
    ps->o.wintype_shadow[WINTYPE_DND] = false;
  if (fading_enable)
    wintype_arr_enable(ps->o.wintype_fade);
  if (1.0 != cfgtmp.menu_opacity) {
    ps->o.wintype_opacity[WINTYPE_DROPDOWN_MENU] = cfgtmp.menu_opacity;
    ps->o.wintype_opacity[WINTYPE_POPUP_MENU] = cfgtmp.menu_opacity;
  }

  // --blur-background-frame implies --blur-background
  if (ps->o.blur_background_frame)
    ps->o.blur_background = true;

  // Other variables determined by options

  // Determine whether we need to track focus changes
  if (ps->o.inactive_opacity || ps->o.inactive_dim) {
    ps->o.track_focus = true;
  }

  // Determine whether we need to track window name and class
  if (ps->o.shadow_blacklist || ps->o.fade_blacklist
      || ps->o.focus_blacklist || ps->o.invert_color_list)
    ps->o.track_wdata = true;

  // Determine whether we track window grouping
  if (ps->o.detect_transient || ps->o.detect_client_leader) {
    ps->o.track_leader = true;
  }

}

/**
 * Fetch all required atoms and save them to a session.
 */
static void
init_atoms(session_t *ps) {
  ps->atom_opacity = get_atom(ps, "_NET_WM_WINDOW_OPACITY");
  ps->atom_frame_extents = get_atom(ps, "_NET_FRAME_EXTENTS");
  ps->atom_client = get_atom(ps, "WM_STATE");
  ps->atom_name = XA_WM_NAME;
  ps->atom_name_ewmh = get_atom(ps, "_NET_WM_NAME");
  ps->atom_class = XA_WM_CLASS;
  ps->atom_role = get_atom(ps, "WM_WINDOW_ROLE");
  ps->atom_transient = XA_WM_TRANSIENT_FOR;
  ps->atom_client_leader = get_atom(ps, "WM_CLIENT_LEADER");
  ps->atom_ewmh_active_win = get_atom(ps, "_NET_ACTIVE_WINDOW");
  ps->atom_compton_shadow = get_atom(ps, "_COMPTON_SHADOW");

  ps->atom_win_type = get_atom(ps, "_NET_WM_WINDOW_TYPE");
  ps->atoms_wintypes[WINTYPE_UNKNOWN] = 0;
  ps->atoms_wintypes[WINTYPE_DESKTOP] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_DESKTOP");
  ps->atoms_wintypes[WINTYPE_DOCK] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_DOCK");
  ps->atoms_wintypes[WINTYPE_TOOLBAR] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_TOOLBAR");
  ps->atoms_wintypes[WINTYPE_MENU] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_MENU");
  ps->atoms_wintypes[WINTYPE_UTILITY] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_UTILITY");
  ps->atoms_wintypes[WINTYPE_SPLASH] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_SPLASH");
  ps->atoms_wintypes[WINTYPE_DIALOG] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_DIALOG");
  ps->atoms_wintypes[WINTYPE_NORMAL] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_NORMAL");
  ps->atoms_wintypes[WINTYPE_DROPDOWN_MENU] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU");
  ps->atoms_wintypes[WINTYPE_POPUP_MENU] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_POPUP_MENU");
  ps->atoms_wintypes[WINTYPE_TOOLTIP] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_TOOLTIP");
  ps->atoms_wintypes[WINTYPE_NOTIFY] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_NOTIFICATION");
  ps->atoms_wintypes[WINTYPE_COMBO] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_COMBO");
  ps->atoms_wintypes[WINTYPE_DND] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_DND");
}

/**
 * Update refresh rate info with X Randr extension.
 */
static void
update_refresh_rate(session_t *ps) {
  XRRScreenConfiguration* randr_info;

  if (!(randr_info = XRRGetScreenInfo(ps->dpy, ps->root)))
    return;
  ps->refresh_rate = XRRConfigCurrentRate(randr_info);

  XRRFreeScreenConfigInfo(randr_info);

  if (ps->refresh_rate)
    ps->refresh_intv = US_PER_SEC / ps->refresh_rate;
  else
    ps->refresh_intv = 0;
}

/**
 * Initialize refresh-rated based software optimization.
 *
 * @return true for success, false otherwise
 */
static bool
swopti_init(session_t *ps) {
  // Prepare refresh rate
  // Check if user provides one
  ps->refresh_rate = ps->o.refresh_rate;
  if (ps->refresh_rate)
    ps->refresh_intv = US_PER_SEC / ps->refresh_rate;

  // Auto-detect refresh rate otherwise
  if (!ps->refresh_rate && ps->randr_exists) {
    update_refresh_rate(ps);
  }

  // Turn off vsync_sw if we can't get the refresh rate
  if (!ps->refresh_rate)
    return false;

  // Monitor screen changes only if vsync_sw is enabled and we are using
  // an auto-detected refresh rate
  if (ps->randr_exists && !ps->o.refresh_rate)
    XRRSelectInput(ps->dpy, ps->root, RRScreenChangeNotify);

  return true;
}

/**
 * Modify a struct timeval timeout value to render at a fixed pace.
 *
 * @param ps current session
 * @param[in,out] ptv pointer to the timeout
 */
static void
swopti_handle_timeout(session_t *ps, struct timeval *ptv) {
  if (!ptv)
    return;

  // Get the microsecond offset of the time when the we reach the timeout
  // I don't think a 32-bit long could overflow here.
  long offset = (ptv->tv_usec + get_time_timeval().tv_usec - ps->paint_tm_offset) % ps->refresh_intv;
  if (offset < 0)
    offset += ps->refresh_intv;

  assert(offset >= 0 && offset < ps->refresh_intv);

  // If the target time is sufficiently close to a refresh time, don't add
  // an offset, to avoid certain blocking conditions.
  if (offset < SWOPTI_TOLERANCE
      || offset > ps->refresh_intv - SWOPTI_TOLERANCE)
    return;

  // Add an offset so we wait until the next refresh after timeout
  ptv->tv_usec += ps->refresh_intv - offset;
  if (ptv->tv_usec > US_PER_SEC) {
    ptv->tv_usec -= US_PER_SEC;
    ++ptv->tv_sec;
  }
}

/**
 * Initialize DRM VSync.
 *
 * @return true for success, false otherwise
 */
static bool
vsync_drm_init(session_t *ps) {
#ifdef CONFIG_VSYNC_DRM
  // Should we always open card0?
  if ((ps->drm_fd = open("/dev/dri/card0", O_RDWR)) < 0) {
    fprintf(stderr, "vsync_drm_init(): Failed to open device.\n");
    return false;
  }

  if (vsync_drm_wait(ps))
    return false;

  return true;
#else
  fprintf(stderr, "Program not compiled with DRM VSync support.\n");
  return false;
#endif
}

#ifdef CONFIG_VSYNC_DRM
/**
 * Wait for next VSync, DRM method.
 *
 * Stolen from: https://github.com/MythTV/mythtv/blob/master/mythtv/libs/libmythtv/vsync.cpp
 */
static int
vsync_drm_wait(session_t *ps) {
  int ret = -1;
  drm_wait_vblank_t vbl;

  vbl.request.type = _DRM_VBLANK_RELATIVE,
  vbl.request.sequence = 1;

  do {
     ret = ioctl(ps->drm_fd, DRM_IOCTL_WAIT_VBLANK, &vbl);
     vbl.request.type &= ~_DRM_VBLANK_RELATIVE;
  } while (ret && errno == EINTR);

  if (ret)
    fprintf(stderr, "vsync_drm_wait(): VBlank ioctl did not work, "
        "unimplemented in this drmver?\n");

  return ret;

}
#endif

/**
 * Initialize OpenGL VSync.
 *
 * Stolen from: http://git.tuxfamily.org/?p=ccm/cairocompmgr.git;a=commitdiff;h=efa4ceb97da501e8630ca7f12c99b1dce853c73e
 * Possible original source: http://www.inb.uni-luebeck.de/~boehme/xvideo_sync.html
 *
 * @return true for success, false otherwise
 */
static bool
vsync_opengl_init(session_t *ps) {
#ifdef CONFIG_VSYNC_OPENGL
  // Get video sync functions
  ps->glx_get_video_sync = (f_GetVideoSync)
    glXGetProcAddress ((const GLubyte *) "glXGetVideoSyncSGI");
  ps->glx_wait_video_sync = (f_WaitVideoSync)
    glXGetProcAddress ((const GLubyte *) "glXWaitVideoSyncSGI");
  if (!ps->glx_wait_video_sync || !ps->glx_get_video_sync) {
    fprintf(stderr, "vsync_opengl_init(): "
        "Failed to get glXWait/GetVideoSyncSGI function.\n");
    return false;
  }

  return true;
#else
  fprintf(stderr, "Program not compiled with OpenGL VSync support.\n");
  return false;
#endif
}

#ifdef CONFIG_VSYNC_OPENGL
/**
 * Wait for next VSync, OpenGL method.
 */
static void
vsync_opengl_wait(session_t *ps) {
  unsigned vblank_count;

  ps->glx_get_video_sync(&vblank_count);
  ps->glx_wait_video_sync(2, (vblank_count + 1) % 2, &vblank_count);
  // I see some code calling glXSwapIntervalSGI(1) afterwards, is it required?
}
#endif

/**
 * Wait for next VSync.
 */
static void
vsync_wait(session_t *ps) {
  if (VSYNC_NONE == ps->o.vsync)
    return;

#ifdef CONFIG_VSYNC_DRM
  if (VSYNC_DRM == ps->o.vsync) {
    vsync_drm_wait(ps);
    return;
  }
#endif

#ifdef CONFIG_VSYNC_OPENGL
  if (VSYNC_OPENGL == ps->o.vsync) {
    vsync_opengl_wait(ps);
    return;
  }
#endif

  // This place should not reached!
  assert(0);

  return;
}

/**
 * Pregenerate alpha pictures.
 */
static void
init_alpha_picts(session_t *ps) {
  int i;
  int num = round(1.0 / ps->o.alpha_step) + 1;

  ps->alpha_picts = malloc(sizeof(Picture) * num);

  for (i = 0; i < num; ++i) {
    double o = i * ps->o.alpha_step;
    if ((1.0 - o) > ps->o.alpha_step)
      ps->alpha_picts[i] = solid_picture(ps, false, o, 0, 0, 0);
    else
      ps->alpha_picts[i] = None;
  }
}

/**
 * Initialize double buffer.
 */
static void
init_dbe(session_t *ps) {
  if (!(ps->root_dbe = XdbeAllocateBackBufferName(ps->dpy,
          (ps->o.paint_on_overlay ? ps->overlay: ps->root), XdbeCopied))) {
    fprintf(stderr, "Failed to create double buffer. Double buffering "
        "turned off.\n");
    ps->o.dbe = false;
    return;
  }
}

/**
 * Initialize X composite overlay window.
 */
static void
init_overlay(session_t *ps) {
  ps->overlay = XCompositeGetOverlayWindow(ps->dpy, ps->root);
  if (ps->overlay) {
    // Set window region of the overlay window, code stolen from
    // compiz-0.8.8
    XserverRegion region = XFixesCreateRegion(ps->dpy, NULL, 0);
    XFixesSetWindowShapeRegion(ps->dpy, ps->overlay, ShapeBounding, 0, 0, 0);
    XFixesSetWindowShapeRegion(ps->dpy, ps->overlay, ShapeInput, 0, 0, region);
    XFixesDestroyRegion(ps->dpy, region);

    // Listen to Expose events on the overlay
    XSelectInput(ps->dpy, ps->overlay, ExposureMask);

    // Retrieve DamageNotify on root window if we are painting on an
    // overlay
    // root_damage = XDamageCreate(ps->dpy, root, XDamageReportNonEmpty);
  }
  else {
    fprintf(stderr, "Cannot get X Composite overlay window. Falling "
        "back to painting on root window.\n");
    ps->o.paint_on_overlay = false;
  }
}

/**
 * Query needed X Render filters to check for their existence.
 */
static void
init_filters(session_t *ps) {
  if (ps->o.blur_background || ps->o.blur_background_frame) {
    // Query filters
    XFilters *pf = XRenderQueryFilters(ps->dpy, get_tgt_window(ps));
    if (pf) {
      for (int i = 0; i < pf->nfilter; ++i) {
        // Convolution filter
        if (!strcmp(pf->filter[i], XRFILTER_CONVOLUTION))
          ps->xrfilter_convolution_exists = true;
      }
    }
    XFree(pf);

    // Turn features off if any required filter is not present
    if (!ps->xrfilter_convolution_exists) {
      fprintf(stderr, "X Render convolution filter unsupported by your X server. Background blur disabled.\n");
      ps->o.blur_background = false;
      ps->o.blur_background_frame = false;
    }
  }
}

/**
 * Redirect all windows.
 */
static void
redir_start(session_t *ps) {
  if (!ps->redirected) {
#ifdef DEBUG_REDIR
    printf("redir_start(): Screen redirected.\n");
#endif

    // Map overlay window. Done firstly according to this:
    // https://bugzilla.gnome.org/show_bug.cgi?id=597014
    if (ps->overlay)
      XMapWindow(ps->dpy, ps->overlay);

    XCompositeRedirectSubwindows(ps->dpy, ps->root, CompositeRedirectManual);

    // Must call XSync() here
    XSync(ps->dpy, False);

    ps->redirected = true;
  }
}

/**
 * Get the poll time.
 */
static time_ms_t
timeout_get_poll_time(session_t *ps) {
  const time_ms_t now = get_time_ms();
  time_ms_t wait = TIME_MS_MAX;

  // Traverse throught the timeout linked list
  for (timeout_t *ptmout = ps->tmout_lst; ptmout; ptmout = ptmout->next) {
    if (ptmout->enabled) {
      time_ms_t newrun = timeout_get_newrun(ptmout);
      if (newrun <= now) {
        wait = 0;
        break;
      }
      else {
        time_ms_t newwait = newrun - now;
        if (newwait < wait)
          wait = newwait;
      }
    }
  }

  return wait;
}

/**
 * Insert a new timeout.
 */
timeout_t *
timeout_insert(session_t *ps, time_ms_t interval,
    bool (*callback)(session_t *ps, timeout_t *ptmout), void *data) {
  const static timeout_t tmout_def = {
    .enabled = true,
    .data = NULL,
    .callback = NULL,
    .firstrun = 0L,
    .lastrun = 0L,
    .interval = 0L,
  };

  const time_ms_t now = get_time_ms();
  timeout_t *ptmout = malloc(sizeof(timeout_t));
  if (!ptmout)
    printf_errfq(1, "(): Failed to allocate memory for timeout.");
  memcpy(ptmout, &tmout_def, sizeof(timeout_t));

  ptmout->interval = interval;
  ptmout->firstrun = now;
  ptmout->lastrun = now;
  ptmout->data = data;
  ptmout->callback = callback;
  ptmout->next = ps->tmout_lst;
  ps->tmout_lst = ptmout;

  return ptmout;
}

/**
 * Drop a timeout.
 *
 * @return true if we have found the timeout and removed it, false
 *         otherwise
 */
bool
timeout_drop(session_t *ps, timeout_t *prm) {
  timeout_t **pplast = &ps->tmout_lst;

  for (timeout_t *ptmout = ps->tmout_lst; ptmout;
      pplast = &ptmout->next, ptmout = ptmout->next) {
    if (prm == ptmout) {
      *pplast = ptmout->next;
      free(ptmout);

      return true;
    }
  }

  return false;
}

/**
 * Clear all timeouts.
 */
static void
timeout_clear(session_t *ps) {
  timeout_t *ptmout = ps->tmout_lst;
  timeout_t *next = NULL;
  while (ptmout) {
    next = ptmout->next;
    free(ptmout);
    ptmout = next;
  }
}

/**
 * Run timeouts.
 *
 * @return true if we have ran a timeout, false otherwise
 */
static bool
timeout_run(session_t *ps) {
  const time_ms_t now = get_time_ms();
  bool ret = false;
  timeout_t *pnext = NULL;

  for (timeout_t *ptmout = ps->tmout_lst; ptmout; ptmout = pnext) {
    pnext = ptmout->next;
    if (ptmout->enabled) {
      const time_ms_t max = now +
        (time_ms_t) (ptmout->interval * TIMEOUT_RUN_TOLERANCE);
      time_ms_t newrun = timeout_get_newrun(ptmout);
      if (newrun <= max) {
        ret = true;
        timeout_invoke(ps, ptmout);
      }
    }
  }

  return ret;
}

/**
 * Invoke a timeout.
 */
void
timeout_invoke(session_t *ps, timeout_t *ptmout) {
  const time_ms_t now = get_time_ms();
  ptmout->lastrun = now;
  // Avoid modifying the timeout structure after running timeout, to
  // make it possible to remove timeout in callback
  if (ptmout->callback)
    ptmout->callback(ps, ptmout);
}

/**
 * Unredirect all windows.
 */
static void
redir_stop(session_t *ps) {
  if (ps->redirected) {
#ifdef DEBUG_REDIR
    printf("redir_stop(): Screen unredirected.\n");
#endif
    // Destroy all Pictures as they expire once windows are unredirected
    // If we don't destroy them here, looks like the resources are just
    // kept inaccessible somehow
    for (win *w = ps->list; w; w = w->next) {
      free_pixmap(ps, &w->pixmap);
      free_picture(ps, &w->picture);
    }

    XCompositeUnredirectSubwindows(ps->dpy, ps->root, CompositeRedirectManual);
    // Unmap overlay window
    if (ps->overlay)
      XUnmapWindow(ps->dpy, ps->overlay);

    // Must call XSync() here
    XSync(ps->dpy, False);

    ps->redirected = false;
  }
}

/**
 * Main loop.
 */
static bool
mainloop(session_t *ps) {
  // Process existing events
  // Sometimes poll() returns 1 but no events are actually read,
  // causing XNextEvent() to block, I have no idea what's wrong, so we
  // check for the number of events here.
  if (XEventsQueued(ps->dpy, QueuedAfterReading)) {
    XEvent ev = { };

    XNextEvent(ps->dpy, &ev);
    ev_handle(ps, &ev);
    ps->ev_received = true;

    return true;
  }

#ifdef CONFIG_DBUS
  if (ps->o.dbus) {
    cdbus_loop(ps);
  }
#endif

  if (ps->reset)
    return false;

  // Calculate timeout
  struct timeval *ptv = NULL;
  {
    // Consider ev_received firstly
    if (ps->ev_received) {
      ptv = malloc(sizeof(struct timeval));
      ptv->tv_sec = 0L;
      ptv->tv_usec = 0L;
    }
    // Then consider fading timeout
    else if (!ps->idling) {
      ptv = malloc(sizeof(struct timeval));
      *ptv = ms_to_tv(fade_timeout(ps));
    }

    // Software optimization is to be applied on timeouts that require
    // immediate painting only
    if (ptv && ps->o.sw_opti)
      swopti_handle_timeout(ps, ptv);

    // Don't continue looping for 0 timeout
    if (ptv && timeval_isempty(ptv)) {
      free(ptv);
      return false;
    }

    // Now consider the waiting time of other timeouts
    time_ms_t tmout_ms = timeout_get_poll_time(ps);
    if (tmout_ms < TIME_MS_MAX) {
      if (!ptv) {
        ptv = malloc(sizeof(struct timeval));
        *ptv = ms_to_tv(tmout_ms);
      }
      else if (timeval_ms_cmp(ptv, tmout_ms) > 0) {
        *ptv = ms_to_tv(tmout_ms);
      }
    }

    // Don't continue looping for 0 timeout
    if (ptv && timeval_isempty(ptv)) {
      free(ptv);
      return false;
    }
  }

  // Polling
  fds_poll(ps, ptv);
  free(ptv);
  ptv = NULL;

  timeout_run(ps);

  return true;
}

/**
 * Initialize a session.
 *
 * @param ps_old old session, from which the function will take the X
 *    connection, then free it
 * @param argc number of commandline arguments
 * @param argv commandline arguments
 */
static session_t *
session_init(session_t *ps_old, int argc, char **argv) {
  const static session_t s_def = {
    .dpy = NULL,
    .scr = 0,
    .vis = NULL,
    .depth = 0,
    .root = None,
    .root_height = 0,
    .root_width = 0,
    // .root_damage = None,
    .overlay = None,
    .root_tile = None,
    .screen_reg = None,
    .tgt_picture = None,
    .tgt_buffer = None,
    .root_dbe = None,
    .reg_win = None,
    .o = {
      .display = NULL,
      .mark_wmwin_focused = false,
      .mark_ovredir_focused = false,
      .fork_after_register = false,
      .synchronize = false,
      .detect_rounded_corners = false,
      .paint_on_overlay = false,
      .unredir_if_possible = false,
      .dbus = false,
      .logpath = NULL,

      .refresh_rate = 0,
      .sw_opti = false,
      .vsync = VSYNC_NONE,
      .dbe = false,
      .vsync_aggressive = false,

      .wintype_shadow = { false },
      .shadow_red = 0.0,
      .shadow_green = 0.0,
      .shadow_blue = 0.0,
      .shadow_radius = 12,
      .shadow_offset_x = -15,
      .shadow_offset_y = -15,
      .shadow_opacity = .75,
      .clear_shadow = false,
      .shadow_blacklist = NULL,
      .shadow_ignore_shaped = false,
      .respect_prop_shadow = false,

      .wintype_fade = { false },
      .fade_in_step = 0.028 * OPAQUE,
      .fade_out_step = 0.03 * OPAQUE,
      .fade_delta = 10,
      .no_fading_openclose = false,
      .fade_blacklist = NULL,

      .wintype_opacity = { 0.0 },
      .inactive_opacity = 0,
      .inactive_opacity_override = false,
      .frame_opacity = 0.0,
      .detect_client_opacity = false,
      .alpha_step = 0.03,

      .blur_background = false,
      .blur_background_frame = false,
      .blur_background_fixed = false,
      .inactive_dim = 0.0,
      .inactive_dim_fixed = false,
      .invert_color_list = NULL,

      .wintype_focus = { false },
      .use_ewmh_active_win = false,
      .focus_blacklist = NULL,
      .detect_transient = false,
      .detect_client_leader = false,

      .track_focus = false,
      .track_wdata = false,
      .track_leader = false,
    },

    .pfds_read = NULL,
    .pfds_write = NULL,
    .pfds_except = NULL,
    .nfds_max = 0,
    .tmout_lst = NULL,

    .all_damage = None,
    .time_start = { 0, 0 },
    .redirected = false,
    .unredir_possible = false,
    .alpha_picts = NULL,
    .reg_ignore_expire = false,
    .idling = false,
    .fade_time = 0L,
    .ignore_head = NULL,
    .ignore_tail = NULL,
    .reset = false,

    .expose_rects = NULL,
    .size_expose = 0,
    .n_expose = 0,

    .list = NULL,
    .active_win = NULL,
    .active_leader = None,

    .black_picture = None,
    .cshadow_picture = None,
    .white_picture = None,
    .gaussian_map = NULL,
    .cgsize = 0,
    .shadow_corner = NULL,
    .shadow_top = NULL,

    .refresh_rate = 0,
    .refresh_intv = 0UL,
    .paint_tm_offset = 0L,

#ifdef CONFIG_VSYNC_DRM
    .drm_fd = 0,
#endif

#ifdef CONFIG_VSYNC_OPENGL
    .glx_context = None,
    .glx_get_video_sync = NULL,
    .glx_wait_video_sync = NULL,
#endif

    .xfixes_event = 0,
    .xfixes_error = 0,
    .damage_event = 0,
    .damage_error = 0,
    .render_event = 0,
    .render_error = 0,
    .composite_event = 0,
    .composite_error = 0,
    .composite_opcode = 0,
    .has_name_pixmap = false,
    .shape_exists = false,
    .shape_event = 0,
    .shape_error = 0,
    .randr_exists = 0,
    .randr_event = 0,
    .randr_error = 0,
#ifdef CONFIG_VSYNC_OPENGL
    .glx_exists = false,
    .glx_event = 0,
    .glx_error = 0,
#endif
    .dbe_exists = false,
    .xrfilter_convolution_exists = false,

    .atom_opacity = None,
    .atom_frame_extents = None,
    .atom_client = None,
    .atom_name = None,
    .atom_name_ewmh = None,
    .atom_class = None,
    .atom_role = None,
    .atom_transient = None,
    .atom_ewmh_active_win = None,
    .atom_compton_shadow = None,
    .atom_win_type = None,
    .atoms_wintypes = { 0 },
    .track_atom_lst = NULL,

#ifdef CONFIG_DBUS
    .dbus_conn = NULL,
    .dbus_service = NULL,
#endif
  };

  // Allocate a session and copy default values into it
  session_t *ps = malloc(sizeof(session_t));
  memcpy(ps, &s_def, sizeof(session_t));
  ps_g = ps;
  ps->ignore_tail = &ps->ignore_head;
  gettimeofday(&ps->time_start, NULL);

  wintype_arr_enable(ps->o.wintype_focus);
  ps->o.wintype_focus[WINTYPE_UNKNOWN] = false;
  ps->o.wintype_focus[WINTYPE_NORMAL] = false;
  ps->o.wintype_focus[WINTYPE_UTILITY] = false;

  get_cfg(ps, argc, argv);

  // Inherit old Display if possible, primarily for resource leak checking
  if (ps_old && ps_old->dpy)
    ps->dpy = ps_old->dpy;

  // Open Display
  if (!ps->dpy) {
    ps->dpy = XOpenDisplay(ps->o.display);
    if (!ps->dpy) {
      fprintf(stderr, "Can't open display\n");
      exit(1);
    }
  }

  XSetErrorHandler(error);
  if (ps->o.synchronize) {
    XSynchronize(ps->dpy, 1);
  }

  ps->scr = DefaultScreen(ps->dpy);
  ps->root = RootWindow(ps->dpy, ps->scr);

  ps->vis = DefaultVisual(ps->dpy, ps->scr);
  ps->depth = DefaultDepth(ps->dpy, ps->scr);

  if (!XRenderQueryExtension(ps->dpy,
        &ps->render_event, &ps->render_error)) {
    fprintf(stderr, "No render extension\n");
    exit(1);
  }

  if (!XQueryExtension(ps->dpy, COMPOSITE_NAME, &ps->composite_opcode,
        &ps->composite_event, &ps->composite_error)) {
    fprintf(stderr, "No composite extension\n");
    exit(1);
  }

  {
    int composite_major = 0, composite_minor = 0;

    XCompositeQueryVersion(ps->dpy, &composite_major, &composite_minor);

    if (composite_major > 0 || composite_minor >= 2) {
      ps->has_name_pixmap = true;
    }
  }

  if (!XDamageQueryExtension(ps->dpy, &ps->damage_event, &ps->damage_error)) {
    fprintf(stderr, "No damage extension\n");
    exit(1);
  }

  if (!XFixesQueryExtension(ps->dpy, &ps->xfixes_event, &ps->xfixes_error)) {
    fprintf(stderr, "No XFixes extension\n");
    exit(1);
  }

  // Query X Shape
  if (XShapeQueryExtension(ps->dpy, &ps->shape_event, &ps->shape_error)) {
    ps->shape_exists = true;
  }

  // Query X RandR
  if (ps->o.sw_opti && !ps->o.refresh_rate) {
    if (XRRQueryExtension(ps->dpy, &ps->randr_event, &ps->randr_error))
      ps->randr_exists = true;
    else
      fprintf(stderr, "No XRandR extension, automatic refresh rate "
          "detection impossible.\n");
  }

#ifdef CONFIG_VSYNC_OPENGL
  // Query X GLX extension
  if (VSYNC_OPENGL == ps->o.vsync) {
    if (glXQueryExtension(ps->dpy, &ps->glx_event, &ps->glx_error))
      ps->glx_exists = true;
    else {
      fprintf(stderr, "No GLX extension, OpenGL VSync impossible.\n");
      ps->o.vsync = VSYNC_NONE;
    }
  }
#endif

  // Query X DBE extension
  if (ps->o.dbe) {
    int dbe_ver_major = 0, dbe_ver_minor = 0;
    if (XdbeQueryExtension(ps->dpy, &dbe_ver_major, &dbe_ver_minor))
      if (dbe_ver_major >= 1)
        ps->dbe_exists = true;
      else
        fprintf(stderr, "DBE extension version too low. Double buffering "
            "impossible.\n");
    else {
      fprintf(stderr, "No DBE extension. Double buffering impossible.\n");
    }
    if (!ps->dbe_exists)
      ps->o.dbe = false;
  }

  register_cm(ps, (VSYNC_OPENGL == ps->o.vsync));

  // Initialize software optimization
  if (ps->o.sw_opti)
    ps->o.sw_opti = swopti_init(ps);

  // Initialize DRM/OpenGL VSync
  if ((VSYNC_DRM == ps->o.vsync && !vsync_drm_init(ps))
      || (VSYNC_OPENGL == ps->o.vsync && !vsync_opengl_init(ps)))
    ps->o.vsync = VSYNC_NONE;

  // Overlay must be initialized before double buffer
  if (ps->o.paint_on_overlay)
    init_overlay(ps);

  if (ps->o.dbe)
    init_dbe(ps);

  init_atoms(ps);
  init_alpha_picts(ps);

  ps->gaussian_map = make_gaussian_map(ps->o.shadow_radius);
  presum_gaussian(ps, ps->gaussian_map);

  ps->root_width = DisplayWidth(ps->dpy, ps->scr);
  ps->root_height = DisplayHeight(ps->dpy, ps->scr);

  rebuild_screen_reg(ps);

  {
    XRenderPictureAttributes pa;
    pa.subwindow_mode = IncludeInferiors;

    ps->root_picture = XRenderCreatePicture(ps->dpy, ps->root,
        XRenderFindVisualFormat(ps->dpy, ps->vis),
        CPSubwindowMode, &pa);
    if (ps->o.paint_on_overlay) {
      ps->tgt_picture = XRenderCreatePicture(ps->dpy, ps->overlay,
          XRenderFindVisualFormat(ps->dpy, ps->vis),
          CPSubwindowMode, &pa);
    }
    else {
      ps->tgt_picture = ps->root_picture;
    }
  }

  init_filters(ps);

  ps->black_picture = solid_picture(ps, true, 1, 0, 0, 0);
  ps->white_picture = solid_picture(ps, true, 1, 1, 1, 1);

  // Generates another Picture for shadows if the color is modified by
  // user
  if (!ps->o.shadow_red && !ps->o.shadow_green && !ps->o.shadow_blue) {
    ps->cshadow_picture = ps->black_picture;
  } else {
    ps->cshadow_picture = solid_picture(ps, true, 1,
        ps->o.shadow_red, ps->o.shadow_green, ps->o.shadow_blue);
  }

  fds_insert(ps, ConnectionNumber(ps->dpy), POLLIN);

  XGrabServer(ps->dpy);

  redir_start(ps);

  XSelectInput(ps->dpy, ps->root,
    SubstructureNotifyMask
    | ExposureMask
    | StructureNotifyMask
    | PropertyChangeMask);

  {
    Window root_return, parent_return;
    Window *children;
    unsigned int nchildren;

    XQueryTree(ps->dpy, ps->root, &root_return,
      &parent_return, &children, &nchildren);

    for (unsigned i = 0; i < nchildren; i++) {
      add_win(ps, children[i], i ? children[i-1] : None);
    }

    XFree(children);
  }


  if (ps->o.track_focus) {
    recheck_focus(ps);
  }

  XUngrabServer(ps->dpy);

  // Initialize DBus
  if (ps->o.dbus) {
#ifdef CONFIG_DBUS
    cdbus_init(ps);
    if (!ps->dbus_conn) {
      cdbus_destroy(ps);
      ps->o.dbus = false;
    }
#else
    printf_errfq(1, "(): DBus support not compiled in!");
#endif
  }

  // Fork to background, if asked
  if (ps->o.fork_after_register) {
    if (!fork_after(ps)) {
      session_destroy(ps);
      return NULL;
    }
  }

  // Free the old session
  if (ps_old)
    free(ps_old);

  return ps;
}

/**
 * Destroy a session.
 *
 * Does not close the X connection or free the <code>session_t</code>
 * structure, though.
 *
 * @param ps session to destroy
 */
static void
session_destroy(session_t *ps) {
  redir_stop(ps);

  // Stop listening to events on root window
  XSelectInput(ps->dpy, ps->root, 0);

#ifdef CONFIG_DBUS
  // Kill DBus connection
  if (ps->o.dbus)
    cdbus_destroy(ps);

  free(ps->dbus_service);
#endif

  // Free window linked list
  {
    win *next = NULL;
    for (win *w = ps->list; w; w = next) {
      // Must be put here to avoid segfault
      next = w->next;

      if (IsViewable == w->a.map_state && !w->destroyed)
        win_ev_stop(ps, w);

      free_win_res(ps, w);
      free(w);
    }

    ps->list = NULL;
  }

  // Free alpha_picts
  {
    const int max = round(1.0 / ps->o.alpha_step) + 1;
    for (int i = 0; i < max; ++i)
      free_picture(ps, &ps->alpha_picts[i]);
    free(ps->alpha_picts);
    ps->alpha_picts = NULL;
  }

  // Free blacklists
  free_wincondlst(&ps->o.shadow_blacklist);
  free_wincondlst(&ps->o.fade_blacklist);
  free_wincondlst(&ps->o.focus_blacklist);
  free_wincondlst(&ps->o.invert_color_list);

  // Free ignore linked list
  {
    ignore_t *next = NULL;
    for (ignore_t *ign = ps->ignore_head; ign; ign = next) {
      next = ign->next;

      free(ign);
    }

    // Reset head and tail
    ps->ignore_head = NULL;
    ps->ignore_tail = &ps->ignore_head;
  }

  // Free cshadow_picture and black_picture
  if (ps->cshadow_picture == ps->black_picture)
    ps->cshadow_picture = None;
  else
    free_picture(ps, &ps->cshadow_picture);

  free_picture(ps, &ps->black_picture);
  free_picture(ps, &ps->white_picture);

  // Free tgt_{buffer,picture} and root_picture
  if (ps->tgt_buffer == ps->tgt_picture)
    ps->tgt_buffer = None;
  else
    free_picture(ps, &ps->tgt_buffer);

  if (ps->tgt_picture == ps->root_picture)
    ps->tgt_picture = None;
  else
    free_picture(ps, &ps->tgt_picture);

  free_picture(ps, &ps->root_picture);

  // Free other X resources
  free_picture(ps, &ps->root_tile);
  free_region(ps, &ps->screen_reg);
  free_region(ps, &ps->all_damage);
  free(ps->expose_rects);
  free(ps->shadow_corner);
  free(ps->shadow_top);
  free(ps->gaussian_map);
  free(ps->o.display);
  free(ps->o.logpath);
  free(ps->pfds_read);
  free(ps->pfds_write);
  free(ps->pfds_except);

  // Free reg_win and glx_context
  if (ps->reg_win) {
    XDestroyWindow(ps->dpy, ps->reg_win);
    ps->reg_win = None;
  }
#ifdef CONFIG_VSYNC_OPENGL
  if (ps->glx_context) {
    glXDestroyContext(ps->dpy, ps->glx_context);
    ps->glx_context = None;
  }
#endif

  // Free double buffer
  if (ps->root_dbe) {
    XdbeDeallocateBackBufferName(ps->dpy, ps->root_dbe);
    ps->root_dbe = None;
  }

#ifdef CONFIG_VSYNC_DRM
  // Close file opened for DRM VSync
  if (ps->drm_fd) {
    close(ps->drm_fd);
    ps->drm_fd = 0;
  }
#endif

  // Release overlay window
  if (ps->overlay) {
    XCompositeReleaseOverlayWindow(ps->dpy, ps->overlay);
    ps->overlay = None;
  }

  // Flush all events
  XSync(ps->dpy, True);

  // Free timeouts
  timeout_clear(ps);

  if (ps == ps_g)
    ps_g = NULL;
}

/**
 * Do the actual work.
 *
 * @param ps current session
 */
static void
session_run(session_t *ps) {
  win *t;

  if (ps->o.sw_opti)
    ps->paint_tm_offset = get_time_timeval().tv_usec;

  ps->reg_ignore_expire = true;

  t = paint_preprocess(ps, ps->list);

  if (ps->redirected)
    paint_all(ps, None, t);

  // Initialize idling
  ps->idling = false;

  // Main loop
  while (!ps->reset) {
    ps->ev_received = false;

    while (mainloop(ps))
      continue;

    // idling will be turned off during paint_preprocess() if needed
    ps->idling = true;

    t = paint_preprocess(ps, ps->list);

    // If the screen is unredirected, free all_damage to stop painting
    if (!ps->redirected)
      free_region(ps, &ps->all_damage);

    if (ps->all_damage && !is_region_empty(ps, ps->all_damage)) {
      static int paint = 0;
      paint_all(ps, ps->all_damage, t);
      ps->reg_ignore_expire = false;
      paint++;
      XSync(ps->dpy, False);
      ps->all_damage = None;
    }

    if (ps->idling)
      ps->fade_time = 0L;
  }
}

/**
 * Turn on the program reset flag.
 *
 * This will result in compton resetting itself after next paint.
 */
static void
reset_enable(int __attribute__((unused)) signum) {
  session_t * const ps = ps_g;

  ps->reset = true;
}

/**
 * The function that everybody knows.
 */
int
main(int argc, char **argv) {
  // Set locale so window names with special characters are interpreted
  // correctly
  setlocale(LC_ALL, "");

  // Set up SIGUSR1 signal handler to reset program
  {
    sigset_t block_mask;
    sigemptyset(&block_mask);
    const struct sigaction action= {
      .sa_handler = reset_enable,
      .sa_mask = block_mask,
      .sa_flags = 0
    };
    sigaction(SIGUSR1, &action, NULL);
  }

  // Main loop
  session_t *ps_old = ps_g;
  while (1) {
    ps_g = session_init(ps_old, argc, argv);
    if (!ps_g) {
      printf_errf("Failed to create new session.");
      return 1;
    }
#ifdef DEBUG_C2
    // c2_parse(ps_g, NULL, "name ~= \"master\"");
    // c2_parse(ps_g, NULL, "n:e:Notification");
    c2_parse(ps_g, NULL, "(WM_NAME:16s = 'Notification' || class_g = 'fox') && class_g = 'fox'");
#endif
    session_run(ps_g);
    ps_old = ps_g;
    session_destroy(ps_g);
  }

  free(ps_g);

  return 0;
}
