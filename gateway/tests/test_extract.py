from __future__ import annotations

import unittest
import urllib.parse

from gateway.extract import (
    MAX_BLOCKS, MAX_BLOCK_TEXT_BYTES, MAX_TEXT_BYTES, MAX_TREE_DEPTH, MAX_TREE_NODES,
    GatewayError, extract_html,
)


class ExtractTests(unittest.TestCase):
    def test_article_order_entities_inline_content_and_navigation(self) -> None:
        document = extract_html(
            """<!doctype html><html><head><title> Café &amp; Tea </title>
            <link rel="prev" href="/articles/0"><script>alert('never read')</script></head>
            <body><nav><p>Navigation</p><a rel="next" href="/articles/2">Next</a></nav>
            <aside><p>Sidebar</p></aside><main>
            <h1>A &amp; B <em>Together</em></h1>
            <p>First <strong>bold</strong> and <a href="/inside">inside link</a>.</p>
            <div>Standalone <span>text</span><br>next line</div>
            <a href="/outside"> Go &amp; read </a>
            <p>First <strong>bold</strong> and <a href="/inside">inside link</a>.</p>
            </main><footer>Colophon</footer></body></html>""",
            "https://example.com/articles/1",
        )

        self.assertEqual(document["format"], "microbrowser-reader-v1")
        self.assertEqual(document["kind"], "article")
        self.assertEqual(document["mode"], "reader")
        self.assertIsNone(document["session"])
        self.assertEqual(document["title"], "Café & Tea")
        self.assertEqual(document["previous_url"], "https://example.com/articles/0")
        self.assertEqual(document["next_url"], "https://example.com/articles/2")
        self.assertEqual(
            document["blocks"],
            [
                {"type": "heading", "text": "A & B Together"},
                {"type": "paragraph", "text": "First bold and inside link."},
                {"type": "text", "text": "Standalone text next line"},
                {"type": "link", "text": "Go & read", "url": "https://example.com/outside"},
            ],
        )
        serialized = str(document)
        self.assertNotIn("Navigation", serialized)
        self.assertNotIn("Sidebar", serialized)
        self.assertNotIn("never read", serialized)

    def test_manga_images_asset_urls_source_urls_and_absolute_link(self) -> None:
        document = extract_html(
            """<main class="manga-reader">
            <a rel="next" href="../2">Next chapter</a>
            <img src="/pages/one page.jpg" width="720" height="1280" alt="Page 1">
            <img data-src="https://images.example/two.png" alt="Page 1">
            <img src="/pages/one page.jpg" alt="Duplicate image">
            </main>""",
            "https://example.com/manga/chapter/1",
        )

        self.assertEqual(document["kind"], "manga")
        self.assertEqual(document["next_url"], "https://example.com/manga/2")
        blocks = document["blocks"]
        self.assertEqual(len(blocks), 3)
        first_image = blocks[1]
        self.assertEqual(first_image["type"], "image")
        self.assertEqual(first_image["width"], 720)
        self.assertEqual(first_image["height"], 1280)
        self.assertEqual(first_image["source_url"], "https://example.com/pages/one%20page.jpg")
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(first_image["url"]).query)
        self.assertEqual(query["url"], ["https://example.com/pages/one%20page.jpg"])
        self.assertEqual(first_image["url"], "/v1/asset?" + urllib.parse.urlencode({
            "url": "https://example.com/pages/one%20page.jpg"
        }))
        self.assertEqual(blocks[2]["url"], "/v1/asset?" + urllib.parse.urlencode({
            "url": "https://images.example/two.png"
        }))

    def test_script_only_page_is_unsupported_with_interactive_offer(self) -> None:
        document = extract_html(
            """<html><head><script>window.required = true;</script></head>
            <body><main><script>document.write('do not read')</script>
            <style>.secret {display:none}</style><noscript>Enable JavaScript</noscript>
            <svg><text>vector text</text></svg></main></body></html>""",
            "https://example.com/application",
        )

        self.assertEqual(document["kind"], "unsupported")
        self.assertTrue(document["interactive_available"])
        self.assertIn("scripts", document["unsupported_reason"])
        self.assertEqual(len(document["blocks"]), 1)
        self.assertIn("interactive mode", document["blocks"][0]["text"])
        self.assertNotIn("do not read", str(document))

    def test_text_blocks_obey_device_utf8_byte_limits(self) -> None:
        many_blocks = extract_html(
            "<main>" + "".join(f"<p>Paragraph {index}</p>" for index in range(300)) + "</main>",
            "https://example.com/many",
        )
        self.assertEqual(len(many_blocks["blocks"]), MAX_BLOCKS)

        long_text = extract_html(
            "<main><p>" + ("€" * (MAX_TEXT_BYTES + 100)) + "</p></main>",
            "https://example.com/long",
        )
        text = long_text["blocks"][0]["text"]
        self.assertLessEqual(len(text.encode("utf-8")), MAX_BLOCK_TEXT_BYTES)
        self.assertTrue(text.startswith("€"))
        total = sum(len(block.get("text", "").encode("utf-8")) for block in long_text["blocks"])
        self.assertLessEqual(total, MAX_TEXT_BYTES)

    def test_rejects_html_beyond_tree_depth_and_node_limits(self) -> None:
        cases = (
            "<div>" * (MAX_TREE_DEPTH + 4) + "text",
            "<main>" + "<span>" * (MAX_TREE_NODES + 1) + "</main>",
        )
        for html in cases:
            with self.subTest(length=len(html)), self.assertRaises(GatewayError) as rejected:
                extract_html(html, "https://example.com/complex")
            self.assertEqual(rejected.exception.code, "document_too_complex")

    def test_image_attributes_above_device_dimensions_are_omitted(self) -> None:
        document = extract_html(
            '<main class="manga"><img src="/panel.png" width="16385" height="99999"></main>',
            "https://example.com/manga",
        )
        self.assertNotIn("width", document["blocks"][0])
        self.assertNotIn("height", document["blocks"][0])

    def test_hidden_chrome_and_sensitive_urls_are_not_emitted(self) -> None:
        document = extract_html(
            """<main><p>Readable</p><div hidden>Hidden</div>
            <div aria-hidden="true">Also hidden</div>
            <a href="https://user:password@example.com/private">Credential link</a>
            <a href="javascript:alert(1)">Script link</a></main>""",
            "https://example.com/public",
        )
        self.assertEqual(document["blocks"], [{"type": "paragraph", "text": "Readable"}])


    def test_reader_title_matches_device_utf8_limit(self) -> None:
        source = "<html><head><title>" + "é" * 512 + "</title></head><body><main lang='en'><h1>Article</h1><p>Reader title content.</p></main></body></html>"
        reader = extract_html(source, "https://example.com")
        self.assertEqual(reader["title"], "é" * 256)

if __name__ == "__main__":
    unittest.main()
