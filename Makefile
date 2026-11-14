# Makefile — Resumen de ventas en USD con OpenMP
#
#   make              compila ./programa
#   make test         pruebas locales sin credenciales (unitarias + validación + API simulada)
#   make bench-local  benchmark de la fase de validación (sin red)
#   make CXX=g++-14   compila con un compilador específico (p. ej. GCC 14.2.0)
#
# Comando equivalente pedido en el enunciado:
#   g++ -O2 -std=c++17 -fopenmp main.cpp -lcurl -o programa

CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -fopenmp -Wall -Wextra -pedantic
TARGET   := programa
HEADERS  := $(wildcard src/*.hpp)

# Si faltan los headers de libcurl se compila igual, pero solo funciona --validate-only.
HASH    := \#
CURL_OK := $(shell printf '$(HASH)include <curl/curl.h>\nint main(){return 0;}\n' | $(CXX) -x c++ - -lcurl -o /dev/null >/dev/null 2>&1 && echo si)
ifeq ($(CURL_OK),si)
LDLIBS ?= -lcurl
else
LDLIBS ?=
$(warning No se encontró libcurl (instale libcurl4-openssl-dev). Solo funcionará --validate-only.)
endif

.PHONY: all test unit validate e2e bench bench-local clean

all: $(TARGET)

$(TARGET): main.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) main.cpp $(LDLIBS) -o $@

tests/unit_tests: tests/unit_tests.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) tests/unit_tests.cpp -o $@

unit: tests/unit_tests
	./tests/unit_tests

validate: $(TARGET)
	bash tests/validate.sh

e2e: $(TARGET)
	python3 -I tests/e2e_mock.py --bin ./$(TARGET)

test: unit validate e2e

# Benchmark real (requiere CPYD_EMAIL y CPYD_APIKEY): ver README
bench: $(TARGET)
	bash scripts/benchmark.sh

bench-local: $(TARGET)
	MODO=validacion bash scripts/benchmark.sh

clean:
	rm -f $(TARGET) tests/unit_tests
	rm -rf benchmark-results
