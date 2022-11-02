CPPFLAGS = -I../include/uapi
CFLAGS = -O2

VERBS_TARGETS = mr-reg-xe
XE_TARGETS = mr-reg-xe

$(VERBS_TARGETS): LDLIBS += -libverbs

$(XE_TARGETS): CFLAGS += -g
$(XE_TARGETS): LDFLAGS += -g
$(XE_TARGETS): LDLIBS += -lze_loader -ldl

ALL_TARGETS = $(XE_TARGETS) $(VERBS_TARGETS)

all: $(ALL_TARGETS)

clean:
	rm -f $(ALL_TARGETS)

