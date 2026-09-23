.PHONY: assets test screens build package
assets:
	python3 tools/generate_assets.py
	python3 tools/generate_audio.py
test: assets
	python3 tests/source_checks.py
	bash tests/host_syntax.sh
	bash tests/run_harness.sh
screens:
	python3 tools/render_screens.py
build:
	./build.sh
package: test
	python3 tools/package_source.py
