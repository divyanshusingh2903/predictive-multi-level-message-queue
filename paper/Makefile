all: main.pdf

generated/numbers.tex: gen_numbers.py $(wildcard ../benchmarks/results/*/summary.json)
	python3 gen_numbers.py

figures/histogram.tex figures/weights.tex figures/aging.tex: gen_figures.py
	python3 gen_figures.py

main.pdf: main.tex sections/*.tex figures/*.tex references.bib generated/numbers.tex figures/histogram.tex figures/weights.tex figures/aging.tex
	latexmk -pdf -interaction=nonstopmode main.tex

clean:
	latexmk -C
