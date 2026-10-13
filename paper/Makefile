all: main.pdf

generated/numbers.tex: gen_numbers.py $(wildcard ../benchmarks/results/*/summary.json)
	python3 gen_numbers.py

main.pdf: main.tex sections/*.tex references.bib generated/numbers.tex
	latexmk -pdf -interaction=nonstopmode main.tex

clean:
	latexmk -C
