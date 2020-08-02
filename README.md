# microbrowser
Microbrowser for Microcontroller

WIP, does not work for now.

# Status

- litehtml is now able to compile on ESP32 and Arduino
- Inkplate on Arduino is working
- Currently implementing litehtml document_container to use Inkplate rendering to draw onto the display from the output of the HTML container_esp32_inkplate

# About this project

A Microbrowser that is able to render HTML5 and CSS3 on a Microcontroller (ESP32). No Javascript support for now.

![image](https://github.com/leopck/microbrowser/raw/master/8bc182d0-0df6-47ae-82d6-84739b40b9bf.jpg)

Recently I just received my Inkplate 6 from CrowdSupply, an E-paper powered by ESP32 and wanted to browse my website from my Inkplate.

# How does it work?

- litehtml for rendering the HTML
- Inkplate library for rendering the text and graphics onto the E-paper
- (Future) LGVL - Light and Versatile Graphics Library https://lvgl.io/

# Getting Started

## Requirement
- Docker

## Compiling and Uploading to your Inkplate ESP32
```
git clone https://github.com/leopck/microbrowser/
docker run -it --rm --privileged -v /dev/ttyUSB0:/dev/ttyUSB0 -v $PWD/microbrowser/:/go/microbrowser arduino-cli
```

## Compile only
```
git clone https://github.com/leopck/microbrowser/
docker run -it --rm --privileged -v /dev/ttyUSB0:/dev/ttyUSB0 -v $PWD/microbrowser/:/go/microbrowser arduino-cli make compile
```
