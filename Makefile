CXX := g++
CC := gcc

CXXFLAGS := -g -Wall -Werror
# Statically compile to make it easier to parse the output assembly dumps.
CXXFLAGS += -static
#CXXFLAGS += -fPIE -fno-plt

default: src/memCap src/memBw src/cpu src/l1i src/l1d src/l2 src/l3 #docker-image

all: default

src/memCap: src/memCap.c
	$(CXX) $(CXXFLAGS) -o src/memCap src/memCap.c

src/memBw: src/memBw.c
	$(CXX) $(CXXFLAGS) -fopenmp -o src/memBw src/memBw.c

src/cpu: src/cpu.cpp
	$(CXX) $(CXXFLAGS) -fopenmp -o src/cpu src/cpu.cpp -lpthread -lgomp

src/l1i: src/l1i.c
	$(CXX) $(CXXFLAGS) -o src/l1i src/l1i.c -lrt

src/l1d: src/l1d.c
	$(CXX) $(CXXFLAGS) -o src/l1d src/l1d.c -lrt

src/l3: src/l3.c
	$(CC) $(CXXFLAGS) -o src/l3 src/l3.c -lrt -lpapi -lcap -lnuma
	# Try to let PAPI operate without adjusting the kernel paranoid levels.
	# Can also set 38=ep for now until CAP_PERFMON is widely supported.
	# https://mjmwired.net/kernel/Documentation/admin-guide/perf-security.rst
	# Either way, doesn't seem to work for now :'(
	sudo setcap 38,CAP_SYS_ADMIN=ep ./src/l3
	#sudo sysctl -w kernel.perf_event_paranoid=3
	sudo sysctl -w kernel.perf_event_paranoid=2

src/l2: src/l2.c
	$(CXX) $(CXXFLAGS) -o src/l2 src/l2.c -lrt

docker-image:
	docker build -t ibench:latest .

clean:
	rm src/memCap
	rm src/memBw
	rm src/cpu
	rm src/l1i
	rm src/l1d
	rm src/l3
	rm src/l2
