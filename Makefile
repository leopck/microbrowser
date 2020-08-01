all: compile upload

compile:
	arduino-cli compile --fqbn Croduino_Boards:esp32:Inkplate6 microbrowser.ino

upload:
	arduino-cli upload -p /dev/ttyUSB0 --fqbn Croduino_Boards:esp32:Inkplate6
