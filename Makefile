.PHONY: all run test perf clean

CC := gcc
DEBUG ?= 1
OPSET ?= $(patsubst src/opset/%.c, %, $(wildcard src/opset/*))

override CFLAGS += -Iinclude -Wall -std=c99 -D_POSIX_C_SOURCE=200809

ifeq ($(DEBUG), 1)
	override CFLAGS += -pg -O0 -g -DDEBUG=1 -fsanitize=address
else
	override CFLAGS += -O3
endif

LIBS := -lm -pthread
SRCS := $(wildcard src/*.c) src/opset.c $(patsubst %, src/opset/%.c, $(OPSET))
OBJS := $(patsubst src/%.c, obj/%.o, $(SRCS))
DEPS := $(patsubst src/%.c, obj/%.d, $(SRCS))

all: connx

run: all
	# Run Asin test case
	rm -f tensorin
	rm -f tensorout
	mkfifo tensorin
	mkfifo tensorout
	# Run connx as daemon
	./connx  testcase/data/node/test_asin/ tensorin tensorout &
	# Run the testcase
	python bin/run.py tensorin tensorout testcase/data/node/test_asin/test_data_set_0/input-0_1_3_3_4_5.data
	# Shutdown the daemon
	python bin/run.py tensorin tensorout
	# Clean
	rm -f tensorin
	rm -f tensorout

test: connx
	python bin/test.py

perf:
	gprof ./connx gmon.out

clean:
	rm -f gmon.out
	rm -f tensorin
	rm -f tensorout
	rm -f src/ver.h
	rm -f src/opset.c
	rm -rf obj
	rm -f connx

connx: src/ver.h $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(filter %.o, $^) $(LIBS)

src/ver.h:
	bin/ver.sh

src/opset.c:
	bin/opset.sh $(OPSET)

obj/%.d: src/ver.h $(SRCS)
	mkdir -p obj/opset; $(CC) $(CFLAGS) -M $< > $@

obj/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $^

ifneq (clean,$(filter clean, $(MAKECMDGOALS)))
-include $(DEPS)
endif
