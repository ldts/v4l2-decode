ACLOCAL_AMFLAGS = -I m4 ${ACLOCAL_FLAGS}

bin_PROGRAMS = v4l2decode

v4l2decode_LDFLAGS = -no-undefined

v4l2decode_CFLAGS = -fPIC -std=gnu99 -shared \
		    $(AVCODEC_CFLAGS) \
		    $(AVFORMAT_CFLAGS) \
		    $(AVUTIL_CFLAGS) \
		    $(LIBDRM_CFLAGS)

v4l2decode_LDADD =  -lm -lpthread \
		    $(AVCODEC_LIBS) \
		    $(AVFORMAT_LIBS) \
		    $(AVUTIL_LIBS) \
		    $(LIBDRM_LIBS)

v4l2decode_SOURCES = args.c args.h common.h \
		     drm.c drm-funcs.h \
		     queue.c queue.h \
		     video.c video.h \
		     main.c

if ENABLE_WAYLAND
v4l2decode_LDADD += $(WAYLANDCLIENT)
v4l2decode_SOURCES += display.c display.h
endif
