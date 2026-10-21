# IEEE conference paper (LaTeX)

`paper.tex` uses the standard `IEEEtran` class (conference mode).

- **Overleaf:** create a blank project, upload `paper.tex`, create a folder `figures/` and upload
  `fig_architecture.png`, `fig_signal.png`, `fig_models.png`, `fig_confusion.png` from
  [`docs/figures/`](../../figures). Compile with pdfLaTeX.
- **Locally:** `pdflatex paper.tex` twice in this folder (figures are found in `../../figures/`).

Replace the `[Author Name]` / `[Department]` placeholders in the `\author` block. The same content
is in `../IEEE_Paper.docx` / `.pdf` (two-column Word layout).
