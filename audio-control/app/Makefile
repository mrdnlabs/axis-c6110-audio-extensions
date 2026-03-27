PROG1	= $(shell jq -r '.acapPackageConf.setup.appName' manifest.json)
SRCS	= audio_control.c vapix_client.c vapix_credentials.c speaker_guard.c audio_forwarder.c mode_controller.c
OBJS	= $(SRCS:.c=.o)
PROGS	= $(PROG1)
DEBUG_DIR = debug

PKGS = libpipewire-0.3 libcurl jansson axparameter gio-2.0

CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS)) -lm -lpthread

CFLAGS += -Wall \
          -Wextra \
          -Wformat=2 \
          -Wpointer-arith \
          -Wstrict-prototypes \
          -Wmissing-prototypes \
          -Wdisabled-optimization \
          -Wfloat-equal \
          -W \
          -Werror

all:	$(PROGS)

$(PROG1): $(OBJS)
	install -d $(DEBUG_DIR)
	$(CC) $^ $(CFLAGS) $(LIBS) $(LDFLAGS) -lm $(LDLIBS) -o $(DEBUG_DIR)/$@
	cp $(DEBUG_DIR)/$@ .
	$(STRIP) $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(PROGS) *.o *.eap* *_LICENSE.txt package.conf* param.conf tmp* $(DEBUG_DIR)
