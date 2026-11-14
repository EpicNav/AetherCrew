"""build_site.py <Docs> <out>: the GitHub Pages site from the Docs folder.

Copies it as it is (the editor's Docs page reads the same files), turns each Markdown guide into an HTML page in the
site's own style, and points links at the .html. Run it locally to preview: python -m http.server -d <out>.
"""
import re
import shutil
import sys
from html import escape
from pathlib import Path

import markdown

src, out = Path(sys.argv[1]), Path(sys.argv[2])
if out.exists():
    shutil.rmtree(out)
shutil.copytree(src, out)
(out / ".nojekyll").write_text("")

# The frame of a guide page: index.html's head and top bar, a hero, the article, the footer.
index = (src / "index.html").read_text(encoding="utf-8")
head = index[: index.index("</head>")].replace(re.search(r"<title>.*?</title>", index).group(0), "<title>{title}</title>")
topbar = re.search(r'<header class="topbar">.*?</header>', index, re.S).group(0).replace(' aria-current="page"', "")
footer = re.search(r"<footer>.*?</footer>", index, re.S).group(0)

guides = sorted(src.glob("*.md"))
for md in guides:
    text = md.read_text(encoding="utf-8")
    title = next((line[2:].strip() for line in text.splitlines() if line.startswith("# ")), md.stem)
    text = "\n".join(line for line in text.splitlines() if not line.startswith("# "))  # the title goes in the hero
    converter = markdown.Markdown(extensions=["tables", "fenced_code", "toc"])
    body = converter.convert(text).replace("<table>", '<div class="table-wrap"><table>').replace("</table>", "</table></div>")
    nav = "".join(f'<a href="#{item["id"]}">{escape(item["name"])}</a>' for item in converter.toc_tokens)
    page = (head.format(title=escape(title)) + '</head><body><a class="skip" href="#main">Skip to content</a>\n' + topbar
            + '\n<main id="main"><section class="doc-hero"><div class="eyebrow accent">GUIDE</div><h1>' + escape(title) + "</h1></section>"
            + '<div class="doc-layout"><aside class="side-nav"><div class="eyebrow">ON THIS PAGE</div>' + nav + "</aside>"
            + '<article class="doc-article"><section>' + body + "</section></article></div>\n" + footer + "</main>\n</body></html>\n")
    (out / (md.stem + ".html")).write_text(page, encoding="utf-8")
    (out / md.name).unlink()

names = [md.name for md in guides]
for page in out.glob("*.html"):
    html = page.read_text(encoding="utf-8")
    for name in names:
        html = html.replace(f'href="{name}', f'href="{name[:-3]}.html')
    page.write_text(html, encoding="utf-8")
print(f"{out}: {len(list(out.glob('*.html')))} pages, {len(names)} from Markdown")
