## Warning

This code may or may not contain some bugs.

## Quick schematic

```
RFID Module <------ SPI BUS ------> Arduino -- Activate 12v relay --> Electric door strike
                                       ^
                                       |
ESP8266 (optional) <--> Serial <-Shell-'
  ^                       ^
  |                       |
  v                       v
WiFi         Via Arduino USB port (connect Arduino D2 to Arduino D0 and Arduino D3 to Arduino D1)
  ^
  |
  v
TCP port 23 (WiFi settings can be configured via TCP port 42)
```

## TLDR

- Flash `arduino_access_control.ino` on an Arduino Nano v3 or UNO rev3.

- Flash `esp8266_wireless_serial.ino` on an ESP8266 ESP-01. 

## Quick code overview

```c
setup()
{
    // Init Serial
    // Init SPI & card reader
    // Set relay signal pin to OUTPUT mode
}

loop()
{
    // Has some data on the serial buffer to read ? process the command : do nothing
    // Has a tag in front of the reader ? read tag : do nothing
    // Soft reset the card reader to prevent a bug that make the card reader unresponsive after a moment
}
```

## The tag_t type and tag storage

Tags properties (id, uid, permission, holder's name) are stored in the Arduino's embedded EEPROM (https://docs.arduino.cc/learn/built-in-libraries/eeprom).
On the ATmega328P the EEPROM total size is: 1024 bytes.
A single tag is stored on 24 bytes.
So 42 tags in total can be stored in the EEPROM.

```c
#define MAX_TAG 42
#define UID_SIZE 10
#define NAME_SIZE 12

typedef struct {
  uint8_t id; //1 byte
  uint8_t perm; //1 byte (set to 2 to allow unlocking, set to 1 to deny unlocking, set to 0 to mark this tag's memory space as "free")
  uint8_t uid[UID_SIZE]; // 10 bytes (Left padded with zeroes if the actual MFRC522::Uid::size is less than 10 bytes)
  uint8_t name[NAME_SIZE]; //12 bytes (11 bytes + a NUL character at the end)
} tag_t;
```

Note: In the shell, tags are handled by their id for simplicity purpose.
In EEPROM they are handled by their address.

To get a tag address knowing it's id:
```c
#define TAG_ADDR(id) (id * sizeof(tag_t) < 0 || id * sizeof(tag_t) > EEPROM.length() ? -1 : id * sizeof(tag_t))
```
A value of -1 should be handled as an invalid tag id.


## Available command for the serial interface

Insert a tag:
```
USAGE: add <name> <uid> <perm>
EXAMPLE: add KevinC 000000DEADBEEFBABECAFE 2
```

Delete a tag by id:
```
USAGE: del <id>
EXAMPLE: del 24
```
Note: the tag is not actually "deleted" but "deactivated"


Update a tag's properties based on its id:
```
USAGE: update <id> <name> <uid> <perm>
EXAMPLE: update 21 Toto 000000DEADBEEFBABECAFE 2
```

Print the list of registered tags:
```
USAGE: list
```

Open the door:
```
USAGE: open
```

If you can't remember the 5 simple commands listed above:
```
USAGE: help
```
