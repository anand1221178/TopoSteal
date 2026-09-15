CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -I include
LDFLAGS = -lhwloc -lpthread -lm

SRCS = src/topo.c src/pmu.c src/deque.c src/weights.c src/feedback.c src/toposteal.c
OBJS = $(SRCS:.c=.o)

LIB_STATIC = libtoposteal.a

all: $(LIB_STATIC) parallel_sum bench_pointer_chase bench_latency bench_spmv bench_graph500 bench_dlrm_embedding deque_stress

$(LIB_STATIC): $(OBJS)
	ar rcs $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

parallel_sum: examples/parallel_sum.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -L. -ltoposteal $(LDFLAGS) -o $@

bench_pointer_chase: bench/pointer_chase.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -L. -ltoposteal $(LDFLAGS) -o $@

bench_latency: bench/latency_bench.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -L. -ltoposteal $(LDFLAGS) -o $@

bench_spmv: bench/spmv_bench.c $(LIB_STATIC)
	$(CC) $(CFLAGS) -fopenmp $< -L. -ltoposteal $(LDFLAGS) -o $@

bench_graph500: bench/graph500_bfs.c $(LIB_STATIC)
	$(CC) $(CFLAGS) -fopenmp $< -L. -ltoposteal $(LDFLAGS) -o $@

bench_dlrm_embedding: bench/dlrm_embedding.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -L. -ltoposteal $(LDFLAGS) -o $@

deque_stress: tests/deque_stress.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -L. -ltoposteal $(LDFLAGS) -o $@

test_topo: tests/test_topo.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -L. -ltoposteal $(LDFLAGS) -o $@

test_deque: tests/test_deque.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -L. -ltoposteal $(LDFLAGS) -o $@

test_weights: tests/test_weights.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -L. -ltoposteal $(LDFLAGS) -o $@

test_feedback: tests/test_feedback.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -L. -ltoposteal $(LDFLAGS) -o $@

test_pmu: tests/test_pmu.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -L. -ltoposteal $(LDFLAGS) -o $@

test_weights_urgent: tests/test_weights_urgent.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -L. -ltoposteal $(LDFLAGS) -o $@

test: test_topo test_deque test_weights test_feedback test_pmu test_weights_urgent deque_stress
	@echo "--- Running tests ---"
	sudo ./test_topo
	sudo ./test_deque
	sudo ./test_weights
	sudo ./test_feedback
	sudo ./test_pmu
	sudo ./test_weights_urgent
	sudo ./deque_stress

clean:
	rm -f src/*.o $(LIB_STATIC) parallel_sum bench_pointer_chase bench_latency bench_spmv bench_graph500 bench_dlrm_embedding deque_stress
	rm -f test_topo test_deque test_weights test_feedback test_pmu test_weights_urgent

.PHONY: all test clean
