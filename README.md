# microbrowser
Microbrowser for Microcontroller

WIP, does not work for now.

# About this project

A Microbrowser that is able to render HTML5 and CSS3 on a Microcontroller (ESP32). No Javascript support for now.



Recently I just received my Inkplate 6 from CrowdSupply, an E-paper powered by ESP32 and wanted to browse my website from my Inkplate.

# How does it work?

- litehtml for rendering the HTML
- Inkplate library for rendering the text and graphics onto the E-paper
- (Future) LGVL - Light and Versatile Graphics Library https://lvgl.io/

# Status

- litehtml is now able to compile on ESP32 and Arduino
- Inkplate on Arduino is working
- Currently implementing litehtml document_container to use Inkplate rendering to draw onto the display from the output of the HTML
