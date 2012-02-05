#include "compton.h"

/* For shadow precomputation */
int Gsize = -1;
unsigned char *shadow_corner = NULL;
unsigned char *shadow_top = NULL;

double
gaussian(double r, double x, double y) {
  return ((1 / (sqrt(2 * M_PI * r))) *
      exp((- (x * x + y * y)) / (2 * r * r)));
}

conv *
make_gaussian_map(Display *dpy, double r) {
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
      g = gaussian(r, (double) (x - center), (double) (y - center));
      t += g;
      c->data[y * size + x] = g;
    }
  }

/*  printf("gaussian total %f\n", t); */

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

unsigned char
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
void
presum_gaussian(conv *map) {
  int center = map->size/2;
  int opacity, x, y;

  Gsize = map->size;

  if (shadow_corner) free((void *)shadow_corner);
  if (shadow_top) free((void *)shadow_top);

  shadow_corner = (unsigned char *)(malloc((Gsize + 1) * (Gsize + 1) * 26));
  shadow_top = (unsigned char *)(malloc((Gsize + 1) * 26));

  for (x = 0; x <= Gsize; x++) {
    shadow_top[25 * (Gsize + 1) + x] =
      sum_gaussian(map, 1, x - center, center, Gsize * 2, Gsize * 2);

    for (opacity = 0; opacity < 25; opacity++) {
      shadow_top[opacity * (Gsize + 1) + x] =
        shadow_top[25 * (Gsize + 1) + x] * opacity / 25;
    }

    for (y = 0; y <= x; y++) {
      shadow_corner[25 * (Gsize + 1) * (Gsize + 1) + y * (Gsize + 1) + x]
        = sum_gaussian(map, 1, x - center, y - center, Gsize * 2, Gsize * 2);
      shadow_corner[25 * (Gsize + 1) * (Gsize + 1) + x * (Gsize + 1) + y]
        = shadow_corner[25 * (Gsize + 1) * (Gsize + 1) + y * (Gsize + 1) + x];

      for (opacity = 0; opacity < 25; opacity++) {
        shadow_corner[opacity * (Gsize + 1) * (Gsize + 1)
                      + y * (Gsize + 1) + x]
          = shadow_corner[opacity * (Gsize + 1) * (Gsize + 1)
                          + x * (Gsize + 1) + y]
          = shadow_corner[25 * (Gsize + 1) * (Gsize + 1)
                          + y * (Gsize + 1) + x] * opacity / 25;
      }
    }
  }
}

XImage *
make_shadow(Display *dpy, double opacity,
            int width, int height) {
  XImage *ximage;
  unsigned char *data;
  int gsize = gaussian_map->size;
  int ylimit, xlimit;
  int swidth = width + gsize;
  int sheight = height + gsize;
  int center = gsize / 2;
  int x, y;
  unsigned char d;
  int x_diff;
  int opacity_int = (int)(opacity * 25);

  data = malloc(swidth * sheight * sizeof(unsigned char));
  if (!data) return 0;

  ximage = XCreateImage(
    dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 8,
    ZPixmap, 0, (char *) data, swidth, sheight, 8,
    swidth * sizeof(unsigned char));

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

if (!clear_shadow) {
  if (Gsize > 0) {
    d = shadow_top[opacity_int * (Gsize + 1) + Gsize];
  } else {
    d = sum_gaussian(gaussian_map,
      opacity, center, center, width, height);
  }

  memset(data, d, sheight * swidth);
} else {
  // zero the pixmap
  memset(data, 0, sheight * swidth);
}

  /*
   * corners
   */

  ylimit = gsize;
  if (ylimit > sheight / 2) ylimit = (sheight + 1) / 2;

  xlimit = gsize;
  if (xlimit > swidth / 2) xlimit = (swidth + 1) / 2;

  for (y = 0; y < ylimit; y++)
    for (x = 0; x < xlimit; x++) {
      if (xlimit == Gsize && ylimit == Gsize) {
        d = shadow_corner[opacity_int * (Gsize + 1) * (Gsize + 1)
                          + y * (Gsize + 1) + x];
      } else {
        d = sum_gaussian(gaussian_map,
          opacity, x - center, y - center, width, height);
      }
      data[y * swidth + x] = d;
      data[(sheight - y - 1) * swidth + x] = d;
      data[(sheight - y - 1) * swidth + (swidth - x - 1)] = d;
      data[y * swidth + (swidth - x - 1)] = d;
    }

  /*
   * top/bottom
   */

  x_diff = swidth - (gsize * 2);
  if (x_diff > 0 && ylimit > 0) {
    for (y = 0; y < ylimit; y++) {
      if (ylimit == Gsize) {
        d = shadow_top[opacity_int * (Gsize + 1) + y];
      } else {
        d = sum_gaussian(gaussian_map,
          opacity, center, y - center, width, height);
      }
      memset(&data[y * swidth + gsize], d, x_diff);
      memset(&data[(sheight - y - 1) * swidth + gsize], d, x_diff);
    }
  }

  /*
   * sides
   */

  for (x = 0; x < xlimit; x++) {
    if (xlimit == Gsize) {
      d = shadow_top[opacity_int * (Gsize + 1) + x];
    } else {
      d = sum_gaussian(gaussian_map,
        opacity, x - center, center, width, height);
    }
    for (y = gsize; y < sheight - gsize; y++) {
      data[y * swidth + x] = d;
      data[y * swidth + (swidth - x - 1)] = d;
    }
  }

  // zero extra pixels
  if (clear_shadow)
  if (width > gsize && height > gsize) {
    int r = gsize / 2;
    int sr = r - 2;
    int er = r + 4;
    for (y = sr; y < (sheight - er); y++) {
      for (x = sr; x < (swidth - er); x++) {
        data[y * swidth + x] = 0;
      }
    }
  }

  return ximage;
}

Picture
shadow_picture(Display *dpy, double opacity, Picture alpha_pict,
               int width, int height, int *wp, int *hp) {
  XImage *shadowImage;
  Pixmap shadowPixmap;
  Picture shadow_picture;
  GC gc;

  shadowImage = make_shadow(dpy, opacity, width, height);
  if (!shadowImage) return None;

  shadowPixmap = XCreatePixmap(dpy, root,
    shadowImage->width, shadowImage->height, 8);

  if (!shadowPixmap) {
    XDestroyImage(shadowImage);
    return None;
  }

  shadow_picture = XRenderCreatePicture(dpy, shadowPixmap,
    XRenderFindStandardFormat(dpy, PictStandardA8), 0, 0);
  if (!shadow_picture) {
    XDestroyImage(shadowImage);
    XFreePixmap(dpy, shadowPixmap);
    return None;
  }

  gc = XCreateGC(dpy, shadowPixmap, 0, 0);
  if (!gc) {
    XDestroyImage(shadowImage);
    XFreePixmap(dpy, shadowPixmap);
    XRenderFreePicture(dpy, shadow_picture);
    return None;
  }

  XPutImage(
    dpy, shadowPixmap, gc, shadowImage, 0, 0, 0, 0,
    shadowImage->width, shadowImage->height);

  *wp = shadowImage->width;
  *hp = shadowImage->height;
  XFreeGC(dpy, gc);
  XDestroyImage(shadowImage);
  XFreePixmap(dpy, shadowPixmap);

  return shadow_picture;
}

Picture
solid_picture(Display *dpy, Bool argb, double a,
              double r, double g, double b) {
  Pixmap pixmap;
  Picture picture;
  XRenderPictureAttributes pa;
  XRenderColor c;

  pixmap = XCreatePixmap(dpy, root, 1, 1, argb ? 32 : 8);

  if (!pixmap) return None;

  pa.repeat = True;
  picture = XRenderCreatePicture(dpy, pixmap,
    XRenderFindStandardFormat(dpy, argb
      ? PictStandardARGB32 : PictStandardA8),
    CPRepeat,
    &pa);

  if (!picture) {
    XFreePixmap(dpy, pixmap);
    return None;
  }

  c.alpha = a * 0xffff;
  c.red = r * 0xffff;
  c.green = g * 0xffff;
  c.blue = b * 0xffff;

  XRenderFillRectangle(dpy, PictOpSrc, picture, &c, 0, 0, 1, 1);
  XFreePixmap(dpy, pixmap);

  return picture;
}
