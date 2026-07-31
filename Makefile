PWSH := pwsh -NoProfile -File scripts/build.ps1 -Action

all: build

build:
	$(PWSH) build

rebuild:
	$(PWSH) rebuild

run:
	$(PWSH) run

test:
	$(PWSH) test

clean:
	$(PWSH) clean

.PHONY: all build rebuild run test clean
