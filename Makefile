CXX = g++
# make host   the host tool (build/host/sendgraph)
# make qnx    the tool and the demo plant for QNX 8.0 x86_64 (build/qnx); needs qnxsdp-env.sh sourced
QCC ?= qcc
QFLAGS = -Vgcc_ntox86_64 -O2 -Wall -Wextra -g
DEMO = sensor control logger

host: build/host/sendgraph

build/host/sendgraph: src/sendgraph.cpp
	@mkdir -p build/host
	$(CXX) -std=c++17 -O2 -Wall -Wextra $< -o $@

qnx: $(DEMO:%=build/qnx/%) build/qnx/soak.sh build/qnx/run.sh build/qnx/sgcapture build/qnx/sendgraph

build/qnx/sendgraph: src/sendgraph.cpp
	@mkdir -p build/qnx
	$(QCC) -Vgcc_ntox86_64_gpp -std=gnu++17 -O2 -Wall -Wextra -static-libstdc++ $< -o $@

build/qnx/%: demo/%.c demo/plant.h
	@mkdir -p build/qnx
	$(QCC) $(QFLAGS) $< -o $@

clean:
	rm -rf build
.PHONY: host qnx clean

build/qnx/soak.sh: demo/soak.sh
	@mkdir -p build/qnx
	cp $< $@

build/qnx/sgcapture: src/sgcapture
	@mkdir -p build/qnx
	cp $< $@

build/qnx/run.sh: demo/run.sh
	@mkdir -p build/qnx
	cp $< $@
