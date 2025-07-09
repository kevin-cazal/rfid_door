#include <Arduino.h>
#include <SPI.h>
#include <SoftwareSerial.h>
#include "MFRC522.h"
#include "EEPROM.h"

#define NAME_SIZE 12
#define MAX_TAG 42
#define TAG_ADDR(id) (id * sizeof(tag_t) < 0 || id * sizeof(tag_t) > EEPROM.length() - sizeof(tag_t) ? -1 : id * sizeof(tag_t))
#define MAX_CMDS 6
#define MAX_ARGS 5
#define SERIAL_BAUD_RATE 9600
#define DOOR_PIN 8
#define UID_SIZE 10
#define MFRC522_CHIP_ENABLE_PIN 10
#define MFRC522_RST_POW_DOWN_PIN 9

SoftwareSerial softSerial(2, 3); //RX, TX

/*
  EEPROM total size: 1024 bytes
  sizeof(tag_t): 24
  Max number of tag: 42
*/

typedef struct {
  uint8_t id; //1 byte
  uint8_t perm; //1 byte (set to 2 to allow unlocking, set to 1 to deny unlocking, set to 0 to mark this tag's memory space as "free")
  uint8_t uid[UID_SIZE]; // 10 bytes (Left padded with zeroes if the actual MFRC522::Uid::size is less than 10 bytes)
  uint8_t name[NAME_SIZE]; //12 bytes (11 bytes + a NUL character at the end)
} tag_t;

typedef struct  {
    String name;
    void (*fn) (String args[MAX_ARGS]);
    String help;
} cmd_t;

/* Shell built-ins */
void shell_del(String args[MAX_ARGS]);
void shell_update(String args[MAX_ARGS]);
void shell_add(String args[MAX_ARGS]);
void shell_list(String args[MAX_ARGS]);
void shell_help(String args[MAX_ARGS]);
void shell_open(String args[MAX_ARGS]);


/* Tag CRUD utilities */
int16_t eeprom_add_tag(tag_t *t);
int16_t eeprom_get_tag_by_id(uint16_t id);

int16_t eeprom_get_tag_id_by_uid(uint8_t uid[UID_SIZE]);
void eeprom_update_tag_by_id(int16_t id, tag_t *t);
void eeprom_del_tag_by_id(int16_t id);

/* Misc utilities */
String bytes2hexstr(uint8_t *bytes, size_t sz);
void tag_print(tag_t *tag);
bool uid_is_allowed(uint8_t uid[10]);
void door_open(void);
void PCD_DumpVersionToSoftSerial(MFRC522 reader);

MFRC522 card_reader(MFRC522_CHIP_ENABLE_PIN, MFRC522_RST_POW_DOWN_PIN);

cmd_t cmds[] = {
        {.name = "add", .fn = shell_add, .help = "add name uid perm"},
        {.name = "del", .fn = shell_del, .help = "del id"},
        {.name = "update", .fn = shell_update, .help = "update id name uid perm"},
        {.name = "list", .fn = shell_list, .help = "list"},
        {.name = "open", .fn = shell_open, .help = "open"},
        {.name = "help", .fn = shell_help, .help = "help"},
};

void PCD_DumpVersionToSoftSerial(MFRC522 reader) {
	// Get the MFRC522 firmware version
	byte v = reader.PCD_ReadRegister(reader.VersionReg);
	softSerial.print(F("Firmware Version: 0x"));
	softSerial.print(v, HEX);
	// Lookup which version
	switch(v) {
		case 0x88: softSerial.println(F(" = (clone)"));  break;
		case 0x90: softSerial.println(F(" = v0.0"));     break;
		case 0x91: softSerial.println(F(" = v1.0"));     break;
		case 0x92: softSerial.println(F(" = v2.0"));     break;
		case 0x12: softSerial.println(F(" = counterfeit chip"));     break;
		default:   softSerial.println(F(" = (unknown)"));
	}
	// When 0x00 or 0xFF is returned, communication probably failed
	if ((v == 0x00) || (v == 0xFF))
		softSerial.println(F("WARNING: Communication failure, is the MFRC522 properly connected?"));
} 

void nfc_card_read(void)
{
    uint8_t uid[UID_SIZE] = {0};
    if (!card_reader.PICC_IsNewCardPresent())
        return;
    softSerial.println("Card present");
    if (!card_reader.PICC_ReadCardSerial())
        return;
    softSerial.println("Card read");
    memcpy(uid + (UID_SIZE - card_reader.uid.size), card_reader.uid.uidByte, card_reader.uid.size);
    if (uid_is_allowed(uid))
      door_open();
}

int16_t eeprom_get_tag_id_by_uid(uint8_t uid[UID_SIZE])
{
  int id = 0;
  tag_t current_tag;

  while (id < MAX_TAG) {
    EEPROM.get(TAG_ADDR(id), current_tag);
    if (memcmp(current_tag.uid, uid, UID_SIZE) == 0) {
      softSerial.print("Found: ");
      tag_print(&current_tag);
      return id;
    }
    id++;
  }
  softSerial.print("Unknown tag. Uid:");
  softSerial.println(bytes2hexstr(uid, UID_SIZE));
  return -1;
}

bool uid_is_allowed(uint8_t uid[10])
{
  int16_t tag_id = eeprom_get_tag_id_by_uid(uid);
  int16_t tag_addr = TAG_ADDR(tag_id);
  tag_t current_tag;

  if (tag_addr == -1)
    return  0;
  EEPROM.get(tag_addr, current_tag);
  return current_tag.perm >= 1;
}

void door_open(void)
{
    digitalWrite(DOOR_PIN, HIGH);
    delay(2000);
    digitalWrite(DOOR_PIN, LOW);
    delay(2000);
}

void shell_open(String args[MAX_ARGS])
{
  door_open();
}

void shell_del(String args[MAX_ARGS])
{
    int id = args[1].toInt();

    if (id < 0 || id > MAX_TAG) {
        softSerial.println("Invalid id");
        return;
    }
    eeprom_del_tag_by_id(id);
}

void shell_update(String args[MAX_ARGS])
{
    tag_t t;
    size_t lenUid = args[3].length() / 2;
    size_t uidSize = lenUid == 10 ? lenUid : 10;
    size_t lenName = args[2].length() >= NAME_SIZE ? NAME_SIZE - 1 : args[2].length();
    uint8_t perm = args[4].toInt() >= 0; 
    const char *name = args[2].c_str();
    size_t k = 0;
    int16_t id = args[1].toInt();

    memset(t.name, 0, sizeof(t.name));
    memcpy(t.name, name, lenName);
    memset(t.uid, 0, sizeof(t.uid));
    for (size_t i = 0; i < 10; i++) {
        t.uid[i] = strtol(args[3].substring((2 * k), 2 + (2 * k)).c_str(), 0, 16);
        k++;
    }
    t.perm = (perm >= 0 && perm <= 2) ? perm : 0;
    tag_print(&t);
    eeprom_update_tag_by_id(id, &t);
}

void shell_add(String args[MAX_ARGS]) //add name uid perm
{
    tag_t t = {0};
    size_t lenUid = args[2].length() / 2;
    size_t uidSize = lenUid == 10 ? lenUid : 10;
    size_t lenName = args[1].length() >= NAME_SIZE ? NAME_SIZE - 1 : args[1].length();
    uint8_t perm = args[3].toInt(); 
    const char *name = args[1].c_str();
    size_t k = 0;

    memset(t.name, 0, sizeof(t.name));
    memcpy(t.name, name, lenName);
    memset(t.uid, 0, sizeof(t.uid));
    for (size_t i = 0; i < 10; i++) {
        t.uid[i] = strtol(args[2].substring((2 * k), 2 + (2 * k)).c_str(), 0, 16);
        k++;
    }
    t.perm = (perm >= 0 && perm <= 2) ? perm : 0;

    tag_print(&t);
    eeprom_add_tag(&t);
}

void shell_list(String args[MAX_ARGS])
{
  int id = 0;
  tag_t current_tag;

  while (id < MAX_TAG) {
    EEPROM.get(TAG_ADDR(id), current_tag);
    tag_print(&current_tag);
    id++;
  }
}

void shell_help(String args[MAX_ARGS])
{
    for (int i = 0; i < MAX_CMDS; i++) {
        softSerial.println(cmds[i].help);
    }
}

String bytes2hexstr(uint8_t *bytes, size_t sz)
{
  int i = 0;
  String str = "";

  while (i < sz) {
    if (bytes[i] < 0x10)
      str += "0";
    str += String(bytes[i], HEX);
    i++;
    if (i < sz)
      str += ",";
  }
  return str;
}

void shell_serial_read()
{
    String in;
    String args[MAX_ARGS];

    if (!softSerial.available())
        return;
    in = softSerial.readStringUntil('\n');
    in.trim();
    if (!in.length())
        return;
    for (int i = 0; i < MAX_ARGS; i++) {
        in.trim();
        args[i] = in.substring(0, in.indexOf(' '));
        in = in.substring(in.indexOf(' '));
    }
    for (int i = 0; i < MAX_CMDS; i++) {
        if (args[0].equalsIgnoreCase(cmds[i].name)) {
            softSerial.println("[" + cmds[i].name + "]");
            cmds[i].fn(args);
            softSerial.println("");
            return;
        }
    }
    softSerial.println("Command not found. Use 'help' to show available commands.");
}

void tag_print(tag_t *tag)
{
  softSerial.print("ID:");
  softSerial.print(tag->id);
  softSerial.print(",UID:");
  softSerial.print(bytes2hexstr(tag->uid, 10));
  softSerial.print(",PERM:");
  softSerial.print(tag->perm);
  softSerial.print(",NAME:");
  softSerial.println((char *)tag->name);
}



void reset_eeprom()
{
  size_t i = 0;

  while (i < EEPROM.length()) {
    EEPROM.update(i, 0);
    i++;
  }
}

int16_t eeprom_get_tag_by_id(int16_t id)
{
  tag_t tmp;
  int16_t ret = -1;

  if (id > MAX_TAG)
    return ret;
  EEPROM.get(TAG_ADDR(id), tmp);
  while (tmp.perm && id < MAX_TAG) {
    id++;
    EEPROM.get(TAG_ADDR(id), tmp);
  }
  if (id < MAX_TAG) {
    ret = id;
    softSerial.print("Next free tag:");
    softSerial.println(ret);
  }
  return ret;
}

void eeprom_update_tag_by_id(int16_t id, tag_t *t)
{
  EEPROM.put(TAG_ADDR(id), *t);
}

int16_t eeprom_add_tag(tag_t *t)
{
  int16_t new_id = eeprom_get_tag_by_id(0);

  if (new_id == -1)
    return -1;
  softSerial.println("Adding tag");
  softSerial.println(new_id);
  t->id = new_id;
  eeprom_update_tag_by_id(new_id, t);
  return new_id;
}

void eeprom_del_tag_by_id(int16_t id)
{
  /*
  The tag will not be actually deleted, it will only be marked as a "free" slot in EEPROM by setting its permission to 0
  A tag with a permission set to 0 will not be able to trigger the unlocking of the door and may be replaced by the next inserted tag
  */
  tag_t tag;
  
  EEPROM.get(TAG_ADDR(id), tag);
  tag.perm = 0;
  eeprom_update_tag_by_id(id, &tag);
}

void print_all_tags()
{
  int id = 0;
  tag_t current_tag;

  while (id < MAX_TAG) {
    EEPROM.get(TAG_ADDR(id), current_tag);
    tag_print(&current_tag);
    id++;
  }
}

void setup()
{
  softSerial.begin(SERIAL_BAUD_RATE);
  SPI.begin();
	card_reader.PCD_Init();
	delay(4);
	PCD_DumpVersionToSoftSerial(card_reader);
  pinMode(DOOR_PIN, OUTPUT);
  softSerial.println("Ready");
}

void card_reader_soft_reset()
{
  delay(4);
  card_reader.PCD_SoftPowerDown();
  delay(4);
  card_reader.PCD_SoftPowerUp();
  delay(4);
  card_reader.PCD_Init();
}

void loop()
{
  delay(1000);
  shell_serial_read();
  nfc_card_read();
  card_reader_soft_reset();
}
