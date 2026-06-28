CC ?= gcc
PD690XX_DIR ?= ../pd690xx
POSTMERKOS_DIR ?= ../postmerkos
ENABLE_WEBSOCKET ?= 1
INCLUDES = -I$(PD690XX_DIR) -I$(POSTMERKOS_DIR)
CFLAGS ?= -Os
override CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDLIBS += -ljson-c -lcrypt

BASE_SRCS = main.c click_port.c click_global.c status.c json_util.c \
       config_file.c config_apply.c hardware.c network.c result.c validation.c \
       console_cli.c release.c roles.c local_socket.c socket_io.c service_ops.c time_ops.c port_clone.c compatibility.c auth.c ssh_keys.c session.c

ifeq ($(ENABLE_WEBSOCKET),1)
override CPPFLAGS += -DCONFIGD_ENABLE_WEBSOCKET=1
LDLIBS += -lwebsockets
SRCS = $(BASE_SRCS) websocket.c system_ops.c
else
SRCS = $(BASE_SRCS) websocket_disabled.c
endif

SUPPORT_SRCS = $(PD690XX_DIR)/libpd690xx.c $(POSTMERKOS_DIR)/libpostmerkos.c

.PHONY: all clean test-host
all: configd postmerkosctl

configd: $(SRCS) $(SUPPORT_SRCS)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(CFLAGS) -o $@ $(SRCS) $(SUPPORT_SRCS) $(LDFLAGS) $(LDLIBS)

postmerkosctl: postmerkosctl.c socket_io.c socket_io.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ postmerkosctl.c socket_io.c $(LDFLAGS) -ljson-c

test-host:
	./tests/test-host.sh

clean:
	rm -f configd postmerkosctl *.o
