all: main.pdf

RESULTS = $(wildcard ../benchmarks/results/*/summary.json) $(wildcard ../benchmarks/results/*/replay_summary.json) $(wildcard ../benchmarks/results/*/*/replay_summary.json) $(wildcard ../benchmarks/results/*/*/sim/sensitivity.json) $(wildcard ../benchmarks/results/*/*/block_bootstrap.json)

generated/numbers.tex: gen_numbers.py $(RESULTS)
	python3 gen_numbers.py

figures/histogram.tex figures/weights.tex figures/aging.tex figures/prodflow.tex figures/brokers.tex figures/traces.tex figures/cobham.tex figures/perclass.tex: gen_figures.py $(RESULTS)
	python3 gen_figures.py

main.pdf: main.tex sections/*.tex figures/*.tex references.bib generated/numbers.tex figures/histogram.tex figures/weights.tex figures/aging.tex figures/prodflow.tex figures/brokers.tex figures/traces.tex figures/cobham.tex figures/perclass.tex
	latexmk -pdf -interaction=nonstopmode main.tex

clean:
	latexmk -C

# IEEEtran.bst may be missing from a local TeX install; build in a container with the full TeX Live instead.
docker-pdf:
	docker run --rm --user $$(id -u):$$(id -g) -v $$PWD:/w -w /w texlive/texlive:latest latexmk -pdf -interaction=nonstopmode main.tex
