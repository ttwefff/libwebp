AM_CPPFLAGS += -I$(top_builddir)/src -I$(top_srcdir)/src

bin_PROGRAMS = dwebp cwebp
if BUILD_ANIMDIFF
  noinst_PROGRAMS = anim_diff
endif
if BUILD_GIF2WEBP
  bin_PROGRAMS += gif2webp
endif
if BUILD_IMG2WEBP
  bin_PROGRAMS += img2webp
endif
if WANT_MUX
  bin_PROGRAMS += webpmux
endif
if BUILD_VWEBP
  bin_PROGRAMS += vwebp
endif
if BUILD_WEBPINFO
  bin_PROGRAMS += webpinfo
endif

noinst_LTLIBRARIES = libexample_util.la

libexample_util_la_SOURCES = example_util.c example_util.h
libexample_util_la_LIBADD = ../src/libwebp.la

anim_diff_SOURCES = anim_diff.c anim_util.c anim_util.h
anim_diff_CPPFLAGS = $(AM_CPPFLAGS) $(USE_EXPERIMENTAL_CODE) $(GIF_INCLUDES)
anim_diff_LDADD  = ../src/demux/libwebpdemux.la
anim_diff_LDADD += libexample_util.la ../imageio/libimageio_util.la
anim_diff_LDADD += $(GIF_LIBS) -lm

cwebp_SOURCES  = cwebp.c stopwatch.h
cwebp_CPPFLAGS  = $(AM_CPPFLAGS) $(USE_EXPERIMENTAL_CODE)
cwebp_LDADD  = libexample_util.la ../imageio/libimageio_util.la
cwebp_LDADD += ../imageio/libimagedec.la ../src/libwebp.la
cwebp_LDADD += $(JPEG_LIBS) $(PNG_LIBS) $(TIFF_LIBS)

dwebp_SOURCES = dwebp.c stopwatch.h
dwebp_CPPFLAGS  = $(AM_CPPFLAGS) $(USE_EXPERIMENTAL_CODE)
dwebp_CPPFLAGS += $(JPEG_INCLUDES) $(PNG_INCLUDES)
dwebp_LDADD  = libexample_util.la
dwebp_LDADD += ../imageio/libimagedec.la
dwebp_LDADD += ../imageio/libimageenc.la
dwebp_LDADD += ../imageio/libimageio_util.la
dwebp_LDADD += ../src/libwebp.la
dwebp_LDADD +=$(PNG_LIBS) $(JPEG_LIBS)

gif2webp_SOURCES = gif2webp.c gifdec.c gifdec.h
gif2webp_CPPFLAGS = $(AM_CPPFLAGS) $(USE_EXPERIMENTAL_CODE) $(GIF_INCLUDES)
gif2webp_LDADD  = libexample_util.la ../imageio/libimageio_util.la
gif2webp_LDADD += ../src/mux/libwebpmux.la ../src/libwebp.la $(GIF_LIBS)

vwebp_SOURCES = vwebp.c
vwebp_CPPFLAGS = $(AM_CPPFLAGS) $(USE_EXPERIMENTAL_CODE) $(GL_INCLUDES)
vwebp_LDADD  = libexample_util.la ../imageio/libimageio_util.la
vwebp_LDADD += ../src/demux/libwebpdemux.la $(GL_LIBS)

webpmux_SOURCES = webpmux.c
webpmux_CPPFLAGS = $(AM_CPPFLAGS) $(USE_EXPERIMENTAL_CODE)
webpmux_LDADD  = libexample_util.la ../imageio/libimageio_util.la
webpmux_LDADD += ../src/mux/libwebpmux.la ../src/libwebp.la

img2webp_SOURCES = img2webp.c
img2webp_CPPFLAGS = $(AM_CPPFLAGS) $(USE_EXPERIMENTAL_CODE)
img2webp_LDADD  = libexample_util.la ../imageio/libimageio_util.la
img2webp_LDADD += ../imageio/libimagedec.la
img2webp_LDADD += ../src/mux/libwebpmux.la ../src/libwebp.la
img2webp_LDADD += $(PNG_LIBS) $(JPEG_LIBS) $(TIFF_LIBS)

webpinfo_SOURCES = webpinfo.c
webpinfo_CPPFLAGS = $(AM_CPPFLAGS) $(USE_EXPERIMENTAL_CODE)
webpinfo_LDADD  = libexample_util.la ../imageio/libimageio_util.la
webpinfo_LDADD += ../src/libwebp.la

if BUILD_LIBWEBPDECODER
  anim_diff_LDADD += ../src/libwebpdecoder.la
  vwebp_LDADD += ../src/libwebpdecoder.la
else
  anim_diff_LDADD += ../src/libwebp.la
  vwebp_LDADD += ../src/libwebp.la
endif
